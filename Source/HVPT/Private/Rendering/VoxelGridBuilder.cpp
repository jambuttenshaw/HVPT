#include "VoxelGrid.h"

#include "HeterogeneousVolumeExInterface.h"

#include "ScenePrivate.h"
#include "MeshPassUtils.h"
#include "Materials/MaterialRenderProxy.h"
#include "HeterogeneousVolumeInterface.h"
#include "SceneCore.h"


static TAutoConsoleVariable<bool> CVarHVPTForceCubicTopLevelGrid(
	TEXT("r.HVPT.ForceCubicTopLevelGrid"),
	true,
	TEXT("Forces top level grid to be cubic in shape to allow for morton ordering."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTCubicTopLevelGridMaxSize(
	TEXT("r.HVPT.CubicTopLevelGridMaxSize"),
	128,
	TEXT("Max edge length of top level grid when it is cubic."),
	ECVF_RenderThreadSafe
);


struct FHVPT_RasterTileData
{
	uint32 TopLevelGridLinearIndex;
};

uint32 GetTypeHash(const FVolumetricMeshBatch& MeshBatch)
{
	return HashCombineFast(GetTypeHash(MeshBatch.Mesh), GetTypeHash(MeshBatch.Proxy));
}


class FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)

		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMax)

		SHADER_PARAMETER(FMatrix44f, ViewToWorld)
		SHADER_PARAMETER(float, TanHalfFOV)
		SHADER_PARAMETER(float, NearPlaneDepth)
		SHADER_PARAMETER(float, FarPlaneDepth)

		SHADER_PARAMETER(FIntVector, VoxelDimensions)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_MarkTopLevelGridVoxelsForFrustumGridCS", SF_Compute);

class FHVPT_GenerateRasterTilesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_GenerateRasterTilesCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_GenerateRasterTilesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(int, RasterTileVoxelResolution)
		SHADER_PARAMETER(int, MaxNumRasterTiles)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_TopLevelGridData>, TopLevelGridBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWRasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_RasterTileData>, RWRasterTileBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_GenerateRasterTilesCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_GenerateRasterTilesCS", SF_Compute);

class FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, MaxDispatchThreadGroupsPerDimension)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWRasterizeBottomLevelGridIndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_SetRasterizeBottomLevelGridIndirectArgsCS", SF_Compute);

class FHVPT_RasterizeBottomLevelFrustumGridCS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHVPT_RasterizeBottomLevelFrustumGridCS, MeshMaterial);

	class FEnableVelocity : SHADER_PERMUTATION_BOOL("DIM_ENABLE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FEnableVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)

		// Primitive data
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMax)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)
		SHADER_PARAMETER(FVector3f, LocalBoundsOrigin)
		SHADER_PARAMETER(FVector3f, LocalBoundsExtent)
		SHADER_PARAMETER(int32, PrimitiveId)

		// Volume data
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FIntVector, VoxelDimensions)
		SHADER_PARAMETER(FMatrix44f, ViewToWorld)
		SHADER_PARAMETER(float, TanHalfFOV)
		SHADER_PARAMETER(float, NearPlaneDepth)
		SHADER_PARAMETER(float, FarPlaneDepth)

		SHADER_PARAMETER(int, BottomLevelGridBufferSize)

		// Velocity data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld_Velocity)

		// Sampling data
		SHADER_PARAMETER(int, bJitter)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)

		// Raster tile data
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRasterTileData>, RasterTileBuffer)

		// Indirect args
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Grid data
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWBottomLevelGridAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWExtinctionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWEmissionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWScatteringGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWVelocityGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	FHVPT_RasterizeBottomLevelFrustumGridCS() = default;

	FHVPT_RasterizeBottomLevelFrustumGridCS(const FMeshMaterialShaderType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(
			this,
			Initializer.PermutationId,
			Initializer.ParameterMap,
			*FParameters::FTypeInfo::GetStructMetadata(),
			// Don't require full bindings, we use FMaterialShader::SetParameters
			false
		);
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform)
			&& HVPT::DoesMaterialShaderSupportHVPT(Parameters.MaterialParameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());

		// This shader takes a very long time to compile with FXC, so we pre-compile it with DXC first and then forward the optimized HLSL to FXC.
		OutEnvironment.CompilerFlags.Add(CFLAG_PrecompileWithDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);

		OutEnvironment.SetDefine(TEXT("GET_PRIMITIVE_DATA_OVERRIDE"), 1);
		OutEnvironment.SetDefine(TEXT("HVPT_MATERIAL_PASS"), 1);
	}

	void SetParameters(
		FRHIComputeCommandList& RHICmdList,
		FRHIComputeShader* ShaderRHI,
		const FViewInfo& View,
		const FMaterialRenderProxy* MaterialProxy,
		const FMaterial& Material
	)
	{
		FMaterialShader::SetViewParameters(RHICmdList, ShaderRHI, View, View.ViewUniformBuffer);
		FMaterialShader::SetParameters(RHICmdList, ShaderRHI, MaterialProxy, Material, View);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FHVPT_RasterizeBottomLevelFrustumGridCS, TEXT("/Plugin/HVPT/Private/VoxelGrid/RasterizeBottomLevel.usf"), 
	TEXT("HVPT_RasterizeBottomLevelFrustumGridCS"), SF_Compute);


class FHVPT_RasterizeFogFrustumGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_RasterizeFogFrustumGridCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RasterizeFogFrustumGridCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)

		// Fog data
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters)
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters2)
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters3)

		SHADER_PARAMETER(FVector4f, GlobalAlbedo)
		SHADER_PARAMETER(FVector4f, GlobalEmissive)
		SHADER_PARAMETER(float, GlobalExtinctionScale)

		// Volume data
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FIntVector, VoxelDimensions)
		SHADER_PARAMETER(FMatrix44f, ViewToWorld)
		SHADER_PARAMETER(float, TanHalfFOV)
		SHADER_PARAMETER(float, NearPlaneDepth)
		SHADER_PARAMETER(float, FarPlaneDepth)

		SHADER_PARAMETER(int, BottomLevelGridBufferSize)

		// Sampling data
		SHADER_PARAMETER(int, bJitter)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)

		// Raster tile data
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRasterTileData>, RasterTileBuffer)

		// Indirect args
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Grid data
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWBottomLevelGridAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWExtinctionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWEmissionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWScatteringGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWVelocityGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
		OutEnvironment.SetDefine(TEXT("HVPT_MATERIAL_PASS"), 0);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_RasterizeFogFrustumGridCS, "/Plugin/HVPT/Private/VoxelGrid/RasterizeBottomLevel.usf", "HVPT_RasterizeFogFrustumGridCS", SF_Compute)


class FHVPT_TopLevelGridCalculateVoxelSizeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_TopLevelGridCalculateVoxelSizeCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_TopLevelGridCalculateVoxelSizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)

		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMax)

		SHADER_PARAMETER(float, ShadingRateInFrustum)
		SHADER_PARAMETER(float, ShadingRateOutOfFrustum)
		SHADER_PARAMETER(float, MinVoxelSizeInFrustum)
		SHADER_PARAMETER(float, MinVoxelSizeOutOfFrustum)
		SHADER_PARAMETER(int, bUseProjectedPixelSize)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_TopLevelGridCalculateVoxelSizeCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_TopLevelGridCalculateVoxelSizeCS", SF_Compute);

class FHVPT_AllocateBottomLevelGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_AllocateBottomLevelGridCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_AllocateBottomLevelGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)

		SHADER_PARAMETER(int, MaxVoxelResolution)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_AllocateBottomLevelGridCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_AllocateBottomLevelGridCS", SF_Compute);

class FHVPT_RasterizeBottomLevelOrthoGridCS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHVPT_RasterizeBottomLevelOrthoGridCS, MeshMaterial);

	class FEnableVelocity : SHADER_PERMUTATION_BOOL("DIM_ENABLE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FEnableVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)

		// Primitive data
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, PrimitiveWorldBoundsMax)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)
		SHADER_PARAMETER(FVector3f, LocalBoundsOrigin)
		SHADER_PARAMETER(FVector3f, LocalBoundsExtent)
		SHADER_PARAMETER(int32, PrimitiveId)

		// Volume data
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)

		SHADER_PARAMETER(int, BottomLevelGridBufferSize)

		// Velocity data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld_Velocity)

		// Sampling data
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)

		// Volume sample mode
		SHADER_PARAMETER(int, bJitter)

		// Raster tile data
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRasterTileData>, RasterTileBuffer)

		// Indirect args
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Grid data
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_TopLevelGridData>, TopLevelGridBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWBottomLevelGridAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWExtinctionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWEmissionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWScatteringGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWVelocityGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	FHVPT_RasterizeBottomLevelOrthoGridCS() = default;

	FHVPT_RasterizeBottomLevelOrthoGridCS(const FMeshMaterialShaderType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		Bindings.BindForLegacyShaderParameters(
			this,
			Initializer.PermutationId,
			Initializer.ParameterMap,
			*FParameters::FTypeInfo::GetStructMetadata(),
			// Don't require full bindings, we use FMaterialShader::SetParameters
			false
		);
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform)
			&& HVPT::DoesMaterialShaderSupportHVPT(Parameters.MaterialParameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());

		// This shader takes a very long time to compile with FXC, so we pre-compile it with DXC first and then forward the optimized HLSL to FXC.
		OutEnvironment.CompilerFlags.Add(CFLAG_PrecompileWithDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);

		OutEnvironment.SetDefine(TEXT("GET_PRIMITIVE_DATA_OVERRIDE"), 1);
		OutEnvironment.SetDefine(TEXT("HVPT_MATERIAL_PASS"), 1);
	}

	void SetParameters(
		FRHIComputeCommandList& RHICmdList,
		FRHIComputeShader* ShaderRHI,
		const FViewInfo& View,
		const FMaterialRenderProxy* MaterialProxy,
		const FMaterial& Material
	)
	{
		FMaterialShader::SetViewParameters(RHICmdList, ShaderRHI, View, View.ViewUniformBuffer);
		FMaterialShader::SetParameters(RHICmdList, ShaderRHI, MaterialProxy, Material, View);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FHVPT_RasterizeBottomLevelOrthoGridCS, TEXT("/Plugin/HVPT/Private/VoxelGrid/RasterizeBottomLevel.usf"), 
	TEXT("HVPT_RasterizeBottomLevelOrthoGridCS"), SF_Compute);


class FHVPT_RasterizeFogOrthoGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_RasterizeFogOrthoGridCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RasterizeFogOrthoGridCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)

		// Fog data
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters)
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters2)
		SHADER_PARAMETER(FVector4f, ExponentialFogParameters3)

		SHADER_PARAMETER(FVector4f, GlobalAlbedo)
		SHADER_PARAMETER(FVector4f, GlobalEmissive)
		SHADER_PARAMETER(float, GlobalExtinctionScale)

		// Volume data
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
		SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)

		SHADER_PARAMETER(int, BottomLevelGridBufferSize)

		// Sampling data
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER(int, bJitter)

		// Raster tile data
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RasterTileAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRasterTileData>, RasterTileBuffer)

		// Indirect args
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Grid data
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWBottomLevelGridAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_TopLevelGridData>, RWTopLevelGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWExtinctionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWEmissionGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWScatteringGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_GridData>, RWVelocityGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
		OutEnvironment.SetDefine(TEXT("HVPT_MATERIAL_PASS"), 0);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_RasterizeFogOrthoGridCS, "/Plugin/HVPT/Private/VoxelGrid/RasterizeBottomLevel.usf", "HVPT_RasterizeFogOrthoGridCS", SF_Compute)


class FHVPT_BuildMajorantVoxelGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_BuildMajorantVoxelGridCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_BuildMajorantVoxelGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, TopLevelGridResolution)

		// Grid data
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_TopLevelGridData>, TopLevelGridBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, ExtinctionGridBuffer)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_MajorantGridData>, RWMajorantVoxelGridBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_3D"), GetThreadGroupSize3D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize3D() * GetThreadGroupSize3D() * GetThreadGroupSize3D(); }
	static int32 GetThreadGroupSize3D() { return 4; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_BuildMajorantVoxelGridCS, "/Plugin/HVPT/Private/VoxelGrid/VoxelGridBuild.usf", "HVPT_BuildMajorantVoxelGridCS", SF_Compute);


float HVPT::Private::CalcTanHalfFOV(float FOVInDegrees)
{
	return FMath::Tan(FMath::DegreesToRadians(FOVInDegrees * 0.5));
}


void HVPT::Private::CalcViewBoundsAndMinimumVoxelSize(
	const FViewInfo& View, const FHVPT_VoxelGridBuildOptions& BuildOptions, FBoxSphereBounds& TopLevelGridBounds, float& MinimumVoxelSize
)
{
	TopLevelGridBounds = FBoxSphereBounds(ForceInit);
	MinimumVoxelSize = HVPT::GetMinimumVoxelSizeOutsideFrustum();

	// Build view bounds
	FVector WorldCameraOrigin = View.ViewMatrices.GetViewOrigin();

	float TanHalfFOV = CalcTanHalfFOV(View.FOV);
	float HalfWidth = View.ViewRect.Width() * 0.5;
	float PixelWidth = TanHalfFOV / HalfWidth;

	for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.HeterogeneousVolumesMeshBatches.Num(); ++MeshBatchIndex)
	{
		// Only Niagara mesh particles bound to volume materials
		const FMeshBatch* Mesh = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Mesh;
		const FPrimitiveSceneProxy* PrimitiveSceneProxy = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Proxy;
		if (!HVPT::ShouldRenderMeshBatchWithHVPT(Mesh, PrimitiveSceneProxy, View.GetFeatureLevel()))
			continue;

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolume = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			// Only incorporate the primitive if it intersects with the camera bounding sphere where radius=MaxTraceDistance
			const FBoxSphereBounds& PrimitiveBounds = HeterogeneousVolume->GetBounds();
			if (View.ViewFrustum.IntersectBox(PrimitiveBounds.Origin, PrimitiveBounds.BoxExtent))
			{
				TopLevelGridBounds = Union(TopLevelGridBounds, PrimitiveBounds);

				if (View.ViewFrustum.IntersectBox(TopLevelGridBounds.Origin, TopLevelGridBounds.BoxExtent))
				{
					// Bandlimit minimum voxel size request with projected voxel size, based on shading rate
					float Distance = FMath::Max(FVector(PrimitiveBounds.Origin - WorldCameraOrigin).Length() - PrimitiveBounds.BoxExtent.Length(), 0.0);
					float VoxelWidth = Distance * PixelWidth * BuildOptions.ShadingRateInFrustum;

					float PerVolumeMinimumVoxelSize = FMath::Max(VoxelWidth, HeterogeneousVolume->GetMinimumVoxelSize());
					MinimumVoxelSize = FMath::Min(PerVolumeMinimumVoxelSize, MinimumVoxelSize);
				}
			}
		}
	}
}

void HVPT::Private::ClipNearFarDistances(
	const FViewInfo& View, const FBoxSphereBounds& TopLevelGridBounds, float& NearPlaneDistance, float& FarPlaneDistance
)
{
	// Determine near and far planes, in world-space
	float d = -FVector::DotProduct(View.GetViewDirection(), View.ViewLocation);

	// Analyze the input volumes and determine the near/far extents
	FVector Center = TopLevelGridBounds.GetSphere().Center;

	// Project center onto the camera view-plane
	float SignedDistance = FVector::DotProduct(View.GetViewDirection(), Center) + d;

	float Radius = TopLevelGridBounds.GetSphere().W;
	float NearDistance = SignedDistance - Radius;
	float FarDistance = SignedDistance + Radius;

	if (NearPlaneDistance < 0.0)
		NearPlaneDistance = NearDistance;

	if (FarPlaneDistance < 0.0)
		FarPlaneDistance = FarDistance;

	NearPlaneDistance = FMath::Max(NearPlaneDistance, 0.01);
	FarPlaneDistance = FMath::Max(FarPlaneDistance, NearDistance + 1.0);
}

void HVPT::Private::CalculateTopLevelGridResolutionForFrustumGrid(
	const FViewInfo& View, const FHVPT_VoxelGridBuildOptions& BuildOptions, float MinimumVoxelSize, float NearPlaneDistance, float& FarPlaneDistance, FIntVector& TopLevelGridResolution
)
{
	// Determine top-level grid resolution
	// Build a grid where the resolution is proportional to a bottom-level grid that is using all available memory
	int32 BottomLevelGridResolution = HVPT::GetBottomLevelGridResolution();

	float ShadingRate = BuildOptions.ShadingRateInFrustum;
	int32 Width = FMath::CeilToInt32(View.ViewRect.Width() / ShadingRate);
	int32 Height = FMath::CeilToInt32(View.ViewRect.Height() / ShadingRate);

	// Use view frustum field-of-view and preferred minimum voxel size to find optimal far-plane distance
	if (HVPT::EnableOrthoGrid() &&
		ShadingRate >= BuildOptions.ShadingRateOutOfFrustum)
	{
		float GlobalMinimumVoxelSize = FMath::Max(MinimumVoxelSize, HVPT::GetMinimumVoxelSizeInsideFrustum());

		float Theta = FMath::DegreesToRadians(View.FOV * 0.5);
		float PixelTheta = (2.0 * Theta) / Width;
		float TanOfPixel = FMath::Tan(PixelTheta);
		float OptimalFarPlaneDepth = GlobalMinimumVoxelSize / TanOfPixel;
		FarPlaneDistance = FMath::Min(FarPlaneDistance, OptimalFarPlaneDepth);
	}

	// Depth slices should not be smaller than the declared minimum voxel size
	int32 MaxDepth = FMath::CeilToInt32((FarPlaneDistance - NearPlaneDistance) / MinimumVoxelSize);
	int32 Depth = FMath::Min(FMath::CeilToInt32(HVPT::GetDepthSliceCountForFrustumGrid() / ShadingRate), MaxDepth);

	TopLevelGridResolution = FIntVector(Width, Height, Depth);
	TopLevelGridResolution.X = FMath::Max(FMath::DivideAndRoundUp(TopLevelGridResolution.X, BottomLevelGridResolution), 1);
	TopLevelGridResolution.Y = FMath::Max(FMath::DivideAndRoundUp(TopLevelGridResolution.Y, BottomLevelGridResolution), 1);
	TopLevelGridResolution.Z = FMath::Max(FMath::DivideAndRoundUp(TopLevelGridResolution.Z, BottomLevelGridResolution), 1);
}

void HVPT::Private::MarkTopLevelGridVoxelsForFrustumGrid(
	FRDGBuilder& GraphBuilder, const FViewInfo& View, FIntVector TopLevelGridResolution, float NearPlaneDistance, float FarPlaneDistance, const FMatrix& ViewToWorld, FRDGBufferRef& TopLevelGridBuffer
)
{
	int32 TopLevelVoxelCount = TopLevelGridResolution.X * TopLevelGridResolution.Y * TopLevelGridResolution.Z;

	TopLevelGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_TopLevelGridData), TopLevelVoxelCount),
		TEXT("HVPT.FrustumGrid.TopLevelGridBuffer")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TopLevelGridBuffer), 0xFFFFFFF8);

	for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.HeterogeneousVolumesMeshBatches.Num(); ++MeshBatchIndex)
	{
		const FMeshBatch* Mesh = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Mesh;
		const FPrimitiveSceneProxy* PrimitiveSceneProxy = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Proxy;

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolume = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			const FBoxSphereBounds& PrimitiveBounds = HeterogeneousVolume->GetBounds();

			FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS::FParameters>();
			{
				PassParameters->View = View.ViewUniformBuffer;

				PassParameters->PrimitiveWorldBoundsMin = FVector3f(PrimitiveBounds.Origin - PrimitiveBounds.BoxExtent);
				PassParameters->PrimitiveWorldBoundsMax = FVector3f(PrimitiveBounds.Origin + PrimitiveBounds.BoxExtent);

				PassParameters->ViewToWorld = FMatrix44f(ViewToWorld);
				PassParameters->TanHalfFOV = CalcTanHalfFOV(View.FOV);
				PassParameters->NearPlaneDepth = NearPlaneDistance;
				PassParameters->FarPlaneDepth = FarPlaneDistance;

				PassParameters->VoxelDimensions = TopLevelGridResolution;
				PassParameters->TopLevelGridResolution = TopLevelGridResolution;

				PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);
			}

			FIntVector GroupCount;
			GroupCount.X = FMath::DivideAndRoundUp(TopLevelGridResolution.X, FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS::GetThreadGroupSize3D());
			GroupCount.Y = FMath::DivideAndRoundUp(TopLevelGridResolution.Y, FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS::GetThreadGroupSize3D());
			GroupCount.Z = FMath::DivideAndRoundUp(TopLevelGridResolution.Z, FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS::GetThreadGroupSize3D());

			TShaderRef<FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS> ComputeShader = View.ShaderMap->GetShader<FHVPT_MarkTopLevelGridVoxelsForFrustumGridCS>();
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("MarkTopLevelGridVoxelsForFrustumGrid"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				ComputeShader,
				PassParameters,
				GroupCount
			);
		}
	}
}

void HVPT::Private::GenerateRasterTiles(
	FRDGBuilder& GraphBuilder, const FScene* Scene, FIntVector TopLevelGridResolution, FRDGBufferRef TopLevelGridBuffer, FRDGBufferRef& RasterTileBuffer, FRDGBufferRef& RasterTileAllocatorBuffer
)
{
	const uint32 RasterTileVoxelResolution = HVPT::GetBottomLevelGridResolution();
	uint32 MaxNumRasterTiles = TopLevelGridResolution.X * TopLevelGridResolution.Y * TopLevelGridResolution.Z;
	RasterTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_RasterTileData), MaxNumRasterTiles),
		TEXT("HVPT.RasterTileBuffer")
	);

	RasterTileAllocatorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
		TEXT("HVPT.RasterTileAllocatorBuffer")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RasterTileAllocatorBuffer, PF_R32_UINT), 0);

	FHVPT_GenerateRasterTilesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_GenerateRasterTilesCS::FParameters>();
	{
		PassParameters->TopLevelGridResolution = TopLevelGridResolution;
		PassParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(TopLevelGridBuffer);

		PassParameters->RasterTileVoxelResolution = RasterTileVoxelResolution;
		PassParameters->MaxNumRasterTiles = MaxNumRasterTiles;

		PassParameters->RWRasterTileAllocatorBuffer = GraphBuilder.CreateUAV(RasterTileAllocatorBuffer, PF_R32_UINT);
		PassParameters->RWRasterTileBuffer = GraphBuilder.CreateUAV(RasterTileBuffer);
	}

	FIntVector GroupCount;
	GroupCount.X = FMath::DivideAndRoundUp(TopLevelGridResolution.X, FHVPT_GenerateRasterTilesCS::GetThreadGroupSize3D());
	GroupCount.Y = FMath::DivideAndRoundUp(TopLevelGridResolution.Y, FHVPT_GenerateRasterTilesCS::GetThreadGroupSize3D());
	GroupCount.Z = FMath::DivideAndRoundUp(TopLevelGridResolution.Z, FHVPT_GenerateRasterTilesCS::GetThreadGroupSize3D());

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(Scene->GetFeatureLevel());

	TShaderRef<FHVPT_GenerateRasterTilesCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_GenerateRasterTilesCS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GenerateRasterTiles"),
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		GroupCount
	);
}

void HVPT::Private::RasterizeVolumesIntoFrustumVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	FMatrix& ViewToWorld,
	float NearPlaneDistance,
	float FarPlaneDistance,
	FRDGBufferRef RasterTileBuffer,
	FRDGBufferRef RasterTileAllocatorBuffer,
	FIntVector TopLevelGridResolution,
	FRDGBufferRef& TopLevelGridBuffer,
	FRDGBufferRef& ExtinctionGridBuffer,
	FRDGBufferRef& EmissionGridBuffer,
	FRDGBufferRef& ScatteringGridBuffer,
	FRDGBufferRef& VelocityGridBuffer
)
{
	//Setup indirect dispatch
	FRDGBufferRef RasterizeBottomLevelGridIndirectArgsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
		TEXT("HVPT.FrustumGrid.RasterizeBottomLevelGridIndirectArgs")
	);

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(Scene->GetFeatureLevel());

	{
		FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS::FParameters* IndirectArgsPassParameters = GraphBuilder.AllocParameters<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS::FParameters>();
		{
			IndirectArgsPassParameters->MaxDispatchThreadGroupsPerDimension = GRHIMaxDispatchThreadGroupsPerDimension;
			IndirectArgsPassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
			IndirectArgsPassParameters->RWRasterizeBottomLevelGridIndirectArgsBuffer = GraphBuilder.CreateUAV(RasterizeBottomLevelGridIndirectArgsBuffer, PF_R32_UINT);
		}

		FIntVector GroupCount(1, 1, 1);
		TShaderRef<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS>();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetRasterizeBottomLevelGridIndirectArgs"),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			ComputeShader,
			IndirectArgsPassParameters,
			GroupCount
		);
	}

	// Pre-allocate bottom-level voxel grid pool based on user-defined budget
	int32 MaxBottomLevelVoxelCount = (HVPT::GetMaxBottomLevelMemoryInMegabytesForFrustumGrid() * 1e6) / sizeof(FHVPT_GridData);
	int32 BottomLevelGridBufferSize = MaxBottomLevelVoxelCount;

	ExtinctionGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.FrustumGrid.ExtinctionGridBuffer")
	);
	EmissionGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.FrustumGrid.EmissionGridBuffer")
	);
	ScatteringGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.FrustumGrid.ScatteringGridBuffer")
	);
	VelocityGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.FrustumGrid.VelocityGridBuffer")
	);

	FRDGBufferRef BottomLevelGridAllocatorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2),
		TEXT("HVPT.FrustumGrid.BottomLevelGridAllocatorBuffer")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT), 0);

	// Rasterize volumes into bottom-level grid
	for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.HeterogeneousVolumesMeshBatches.Num(); ++MeshBatchIndex)
	{
		const FMeshBatch* Mesh = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Mesh;
		const FPrimitiveSceneProxy* PrimitiveSceneProxy = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex].Proxy;
		const FMaterialRenderProxy* MaterialRenderProxy = Mesh->MaterialRenderProxy;
		if (!HVPT::ShouldRenderMeshBatchWithHVPT(Mesh, PrimitiveSceneProxy, View.GetFeatureLevel()))
			continue;

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolumeInterface = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			bool bEnableVelocity = HVPT::ShouldWriteVelocity();
			if (HVPT::HasExtendedInterface(PrimitiveSceneProxy))
			{
				auto HeterogeneousVolumeExInterface = static_cast<const IHeterogeneousVolumeExInterface*>(HeterogeneousVolumeInterface);
				bEnableVelocity &= HeterogeneousVolumeExInterface->IsPlayingAnimation();
			}

			const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();
			const int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
			const FBoxSphereBounds LocalBoxSphereBounds = PrimitiveSceneProxy->GetLocalBounds();
			const FBoxSphereBounds PrimitiveBounds = PrimitiveSceneProxy->GetBounds();
			const FMaterial& Material = MaterialRenderProxy->GetMaterialWithFallback(View.GetFeatureLevel(), MaterialRenderProxy);

			FHVPT_RasterizeBottomLevelFrustumGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RasterizeBottomLevelFrustumGridCS::FParameters>();
			{
				// Scene data
				PassParameters->View = View.ViewUniformBuffer;
				PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);

				// Primitive data
				PassParameters->PrimitiveWorldBoundsMin = FVector3f(PrimitiveBounds.Origin - PrimitiveBounds.BoxExtent);
				PassParameters->PrimitiveWorldBoundsMax = FVector3f(PrimitiveBounds.Origin + PrimitiveBounds.BoxExtent);

				FMatrix InstanceToLocal = HeterogeneousVolumeInterface->GetInstanceToLocal();
				FMatrix LocalToWorld = HeterogeneousVolumeInterface->GetLocalToWorld();
				PassParameters->LocalToWorld = FMatrix44f(InstanceToLocal * LocalToWorld);
				PassParameters->WorldToLocal = FMatrix44f(PassParameters->LocalToWorld.Inverse());

				FMatrix LocalToInstance = InstanceToLocal.Inverse();
				FBoxSphereBounds InstanceBoxSphereBounds = LocalBoxSphereBounds.TransformBy(FTransform(LocalToInstance));
				PassParameters->LocalBoundsOrigin = FVector3f(InstanceBoxSphereBounds.Origin);
				PassParameters->LocalBoundsExtent = FVector3f(InstanceBoxSphereBounds.BoxExtent);
				PassParameters->PrimitiveId = PrimitiveId;

				// Volume data
				PassParameters->TopLevelGridResolution = TopLevelGridResolution;
				PassParameters->VoxelDimensions = TopLevelGridResolution;
				PassParameters->ViewToWorld = FMatrix44f(ViewToWorld);
				PassParameters->TanHalfFOV = CalcTanHalfFOV(View.FOV);
				PassParameters->NearPlaneDepth = NearPlaneDistance;
				PassParameters->FarPlaneDepth = FarPlaneDistance;

				PassParameters->LocalToWorld_Velocity = FMatrix44f(HeterogeneousVolumeInterface->GetLocalToWorld());

				// Sampling data
				PassParameters->bJitter = BuildOptions.bJitter;
				FBlueNoise BlueNoise = GetBlueNoiseGlobalParameters();
				PassParameters->BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

				// Raster tile data
				PassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
				PassParameters->RasterTileBuffer = GraphBuilder.CreateSRV(RasterTileBuffer);

				// Indirect args
				PassParameters->IndirectArgs = RasterizeBottomLevelGridIndirectArgsBuffer;

				// Grid data
				PassParameters->RWBottomLevelGridAllocatorBuffer = GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT);
				PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);

				PassParameters->RWExtinctionGridBuffer = GraphBuilder.CreateUAV(ExtinctionGridBuffer);
				PassParameters->RWEmissionGridBuffer = GraphBuilder.CreateUAV(EmissionGridBuffer);
				PassParameters->RWScatteringGridBuffer = GraphBuilder.CreateUAV(ScatteringGridBuffer);
				PassParameters->RWVelocityGridBuffer = GraphBuilder.CreateUAV(VelocityGridBuffer);
				PassParameters->BottomLevelGridBufferSize = BottomLevelGridBufferSize;
			}

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("FrustumGrid.RasterizeBottomLevelGrid"),
				PassParameters,
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				[PassParameters, Scene, MaterialRenderProxy, &Material, bEnableVelocity](FRDGAsyncTask, FRHIComputeCommandList& RHICmdList)
				{
					FHVPT_RasterizeBottomLevelFrustumGridCS::FPermutationDomain PermutationVector;
					PermutationVector.Set<FHVPT_RasterizeBottomLevelFrustumGridCS::FEnableVelocity>(bEnableVelocity);
					TShaderRef<FHVPT_RasterizeBottomLevelFrustumGridCS> ComputeShader = Material.GetShader<FHVPT_RasterizeBottomLevelFrustumGridCS>(&FLocalVertexFactory::StaticType, PermutationVector, false);

					if (!ComputeShader.IsNull())
					{
						ClearUnusedGraphResources(ComputeShader, PassParameters);

						FMeshDrawShaderBindings ShaderBindings;
						UE::MeshPassUtils::SetupComputeBindings(ComputeShader, Scene, Scene->GetFeatureLevel(), nullptr, *MaterialRenderProxy, Material, ShaderBindings);

						UE::MeshPassUtils::DispatchIndirect(RHICmdList, ComputeShader, ShaderBindings, *PassParameters, PassParameters->IndirectArgs->GetIndirectRHICallBuffer(), 0);
					}
				}
			);
		}
	}

	// Once all volumes are rasterized, we can add the global fog into the grid so that is accumulated during path tracing
	if (HVPT::GetFogCompositingMode() == EFogCompositionMode::PostAndPathTracing)
	{
		const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

		FHVPT_RasterizeFogFrustumGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RasterizeFogFrustumGridCS::FParameters>();
		// Scene data
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);

		PassParameters->ExponentialFogParameters = View.ExponentialFogParameters;
		PassParameters->ExponentialFogParameters2 = View.ExponentialFogParameters2;
		PassParameters->ExponentialFogParameters3 = View.ExponentialFogParameters3;

		PassParameters->GlobalAlbedo = FogInfo.VolumetricFogAlbedo;
		PassParameters->GlobalEmissive = FogInfo.VolumetricFogEmissive;
		PassParameters->GlobalExtinctionScale = FogInfo.VolumetricFogExtinctionScale;

		PassParameters->TopLevelGridResolution = TopLevelGridResolution;
		PassParameters->VoxelDimensions = TopLevelGridResolution;
		PassParameters->ViewToWorld = FMatrix44f(ViewToWorld);
		PassParameters->TanHalfFOV = CalcTanHalfFOV(View.FOV);
		PassParameters->NearPlaneDepth = NearPlaneDistance;
		PassParameters->FarPlaneDepth = FarPlaneDistance;

		// Sampling data
		PassParameters->bJitter = BuildOptions.bJitter;
		FBlueNoise BlueNoise = GetBlueNoiseGlobalParameters();
		PassParameters->BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

		// Raster tile data
		PassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
		PassParameters->RasterTileBuffer = GraphBuilder.CreateSRV(RasterTileBuffer);

		// Indirect args
		PassParameters->IndirectArgs = RasterizeBottomLevelGridIndirectArgsBuffer;

		// Grid data
		PassParameters->RWBottomLevelGridAllocatorBuffer = GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT);
		PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);

		PassParameters->RWExtinctionGridBuffer = GraphBuilder.CreateUAV(ExtinctionGridBuffer);
		PassParameters->RWEmissionGridBuffer = GraphBuilder.CreateUAV(EmissionGridBuffer);
		PassParameters->RWScatteringGridBuffer = GraphBuilder.CreateUAV(ScatteringGridBuffer);
		PassParameters->RWVelocityGridBuffer = GraphBuilder.CreateUAV(VelocityGridBuffer);
		PassParameters->BottomLevelGridBufferSize = BottomLevelGridBufferSize;

		TShaderRef<FHVPT_RasterizeFogFrustumGridCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_RasterizeFogFrustumGridCS>();

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RasterizeFogFrustumGridCS"),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			[PassParameters, ComputeShader](FRDGAsyncTask, FRHIComputeCommandList& RHICmdList)
			{
				ClearUnusedGraphResources(ComputeShader, PassParameters);
				FComputeShaderUtils::DispatchIndirect(RHICmdList, ComputeShader, *PassParameters, PassParameters->IndirectArgs->GetIndirectRHICallBuffer(), 0);
			}
		);
	}
}


void HVPT::Private::CollectHeterogeneousVolumeMeshBatches(
	const FViewInfo& View, TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches
)
{
	HeterogeneousVolumesMeshBatches.Reset();

	for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.HeterogeneousVolumesMeshBatches.Num(); ++MeshBatchIndex)
	{
		const FVolumetricMeshBatch& MeshBatch = View.HeterogeneousVolumesMeshBatches[MeshBatchIndex];
		if (MeshBatch.Proxy->IsShown(&View))
		{
			HeterogeneousVolumesMeshBatches.FindOrAdd(MeshBatch);
		}
	}
}

void HVPT::Private::CalcGlobalBoundsAndMinimumVoxelSize(
	const FViewInfo& View,
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	FBoxSphereBounds::Builder& TopLevelGridBoundsBuilder,
	float& GlobalMinimumVoxelSize
)
{
	GlobalMinimumVoxelSize = HVPT::GetMinimumVoxelSizeOutsideFrustum();

	// Build view bounds
	FVector WorldCameraOrigin = View.ViewMatrices.GetViewOrigin();

	float TanHalfFOV = CalcTanHalfFOV(View.FOV);
	float HalfWidth = View.ViewRect.Width() * 0.5;
	float PixelWidth = TanHalfFOV / HalfWidth;

	for (auto MeshBatchIt = HeterogeneousVolumesMeshBatches.begin(); MeshBatchIt != HeterogeneousVolumesMeshBatches.end(); ++MeshBatchIt)
	{
		const FVolumetricMeshBatch& MeshBatch = *MeshBatchIt;
		const FMeshBatch* Mesh = MeshBatch.Mesh;
		const FPrimitiveSceneProxy* PrimitiveSceneProxy = MeshBatch.Proxy;

		if (!HVPT::ShouldRenderMeshBatchWithHVPT(Mesh, PrimitiveSceneProxy, View.GetFeatureLevel()))
		{
			continue;
		}

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolume = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			const FBoxSphereBounds& PrimitiveBounds = HeterogeneousVolume->GetBounds();
			TopLevelGridBoundsBuilder += PrimitiveBounds;

			bool bIntersectsViewFrustum = View.ViewFrustum.IntersectBox(PrimitiveBounds.Origin, PrimitiveBounds.BoxExtent);
			float VoxelWidth = bIntersectsViewFrustum ? BuildOptions.ShadingRateInFrustum : BuildOptions.ShadingRateOutOfFrustum;
			if (BuildOptions.bUseProjectedPixelSizeForOrthoGrid)
			{
				float Distance = FMath::Max(FVector(PrimitiveBounds.Origin - WorldCameraOrigin).Length() - PrimitiveBounds.SphereRadius, 0.0);
				VoxelWidth *= Distance * PixelWidth;
			}

			VoxelWidth = FMath::Max(VoxelWidth, HeterogeneousVolume->GetMinimumVoxelSize());
			GlobalMinimumVoxelSize = FMath::Min(VoxelWidth, GlobalMinimumVoxelSize);
		}
	}

	// When converting to pixel-space units, clamp per-view minimum voxel-size to in-frustum minimum
	if (TopLevelGridBoundsBuilder.IsValid())
	{
		FBoxSphereBounds PrimitiveBounds(TopLevelGridBoundsBuilder);
		if (BuildOptions.bUseProjectedPixelSizeForOrthoGrid && View.ViewFrustum.IntersectBox(PrimitiveBounds.Origin, PrimitiveBounds.BoxExtent))
		{
			GlobalMinimumVoxelSize = FMath::Max(GlobalMinimumVoxelSize, HVPT::GetMinimumVoxelSizeInsideFrustum());
		}
	}
}

void HVPT::Private::CalculateTopLevelGridResolution(
	const FBoxSphereBounds& TopLevelGridBounds, float GlobalMinimumVoxelSize, FIntVector& TopLevelGridResolution
)
{
	// Bound Top-level grid resolution to cover fully allocated child grids at minimum voxel size
	FIntVector CombinedChildGridResolution = FIntVector(HVPT::GetBottomLevelGridResolution());

	FVector TopLevelGridVoxelSize = FVector(CombinedChildGridResolution) * GlobalMinimumVoxelSize;
	FVector TopLevelGridResolutionAsFloat = (TopLevelGridBounds.BoxExtent * 2.0f) / TopLevelGridVoxelSize;

	TopLevelGridResolution.X = FMath::CeilToInt(TopLevelGridResolutionAsFloat.X);
	TopLevelGridResolution.Y = FMath::CeilToInt(TopLevelGridResolutionAsFloat.Y);
	TopLevelGridResolution.Z = FMath::CeilToInt(TopLevelGridResolutionAsFloat.Z);

	if (CVarHVPTForceCubicTopLevelGrid.GetValueOnRenderThread())
	{
		// Force top level grid to be a cube to allow for morton ordering of top level grid data for improved access patterns
		uint32 CubeResolution = FMath::Clamp(
			FMath::RoundUpToPowerOfTwo(FMath::Max3(TopLevelGridResolution.X, TopLevelGridResolution.Y, TopLevelGridResolution.Z)), 
			1, CVarHVPTCubicTopLevelGridMaxSize.GetValueOnRenderThread());
		TopLevelGridResolution.X = CubeResolution;
		TopLevelGridResolution.Y = CubeResolution;
		TopLevelGridResolution.Z = CubeResolution;
	}
	else
	{
		// Clamp to a moderate limit to also handle indirection grid allocation
		TopLevelGridResolution.X = FMath::Clamp(TopLevelGridResolution.X, 1, 128);
		TopLevelGridResolution.Y = FMath::Clamp(TopLevelGridResolution.Y, 1, 128);
		TopLevelGridResolution.Z = FMath::Clamp(TopLevelGridResolution.Z, 1, 256);
	}
}

void HVPT::Private::CalculateVoxelSize(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	const FBoxSphereBounds& TopLevelGridBounds, 
	FIntVector TopLevelGridResolution, 
	FRDGBufferRef& TopLevelGridBuffer
)
{
	TopLevelGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_TopLevelGridData), TopLevelGridResolution.X * TopLevelGridResolution.Y * TopLevelGridResolution.Z),
		TEXT("HVPT.TopLevelGridBuffer")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TopLevelGridBuffer), 0xFFFFFFF8);

	for (auto MeshBatchIt = HeterogeneousVolumesMeshBatches.begin(); MeshBatchIt != HeterogeneousVolumesMeshBatches.end(); ++MeshBatchIt)
	{
		const FVolumetricMeshBatch& MeshBatch = *MeshBatchIt;
		const FMeshBatch* Mesh = MeshBatch.Mesh;

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolume = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			const FBoxSphereBounds& PrimitiveBounds = HeterogeneousVolume->GetBounds();
			FHVPT_TopLevelGridCalculateVoxelSizeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_TopLevelGridCalculateVoxelSizeCS::FParameters>();
			{
				PassParameters->View = View.ViewUniformBuffer;

				PassParameters->TopLevelGridResolution = TopLevelGridResolution;
				PassParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
				PassParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);

				PassParameters->PrimitiveWorldBoundsMin = FVector3f(PrimitiveBounds.Origin - PrimitiveBounds.BoxExtent);
				PassParameters->PrimitiveWorldBoundsMax = FVector3f(PrimitiveBounds.Origin + PrimitiveBounds.BoxExtent);

				PassParameters->ShadingRateInFrustum = BuildOptions.ShadingRateInFrustum;
				PassParameters->ShadingRateOutOfFrustum = BuildOptions.ShadingRateOutOfFrustum;
				PassParameters->MinVoxelSizeInFrustum = FMath::Max(HeterogeneousVolume->GetMinimumVoxelSize(), HVPT::GetMinimumVoxelSizeInsideFrustum());
				PassParameters->MinVoxelSizeOutOfFrustum = HVPT::GetMinimumVoxelSizeOutsideFrustum();
				PassParameters->bUseProjectedPixelSize = BuildOptions.bUseProjectedPixelSizeForOrthoGrid;

				PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);
			}

			FIntVector GroupCount;
			GroupCount.X = FMath::DivideAndRoundUp(TopLevelGridResolution.X, FHVPT_TopLevelGridCalculateVoxelSizeCS::GetThreadGroupSize3D());
			GroupCount.Y = FMath::DivideAndRoundUp(TopLevelGridResolution.Y, FHVPT_TopLevelGridCalculateVoxelSizeCS::GetThreadGroupSize3D());
			GroupCount.Z = FMath::DivideAndRoundUp(TopLevelGridResolution.Z, FHVPT_TopLevelGridCalculateVoxelSizeCS::GetThreadGroupSize3D());

			TShaderRef<FHVPT_TopLevelGridCalculateVoxelSizeCS> ComputeShader = View.ShaderMap->GetShader<FHVPT_TopLevelGridCalculateVoxelSizeCS>();
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TopLevelGridCalculateVoxelSize"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				ComputeShader,
				PassParameters,
				GroupCount
			);
		}
	}
}

void HVPT::Private::MarkTopLevelGrid(
	FRDGBuilder& GraphBuilder, const FScene* Scene, const FBoxSphereBounds& TopLevelGridBounds, FIntVector TopLevelGridResolution, FRDGBufferRef& TopLevelGridBuffer
)
{
	FHVPT_AllocateBottomLevelGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_AllocateBottomLevelGridCS::FParameters>();
	{
		//PassParameters->View = View.ViewUniformBuffer;
		PassParameters->TopLevelGridResolution = TopLevelGridResolution;
		PassParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
		PassParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);

		PassParameters->MaxVoxelResolution = HVPT::GetBottomLevelGridResolution();

		PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);
	}

	FIntVector GroupCount;
	GroupCount.X = FMath::DivideAndRoundUp(TopLevelGridResolution.X, FHVPT_AllocateBottomLevelGridCS::GetThreadGroupSize3D());
	GroupCount.Y = FMath::DivideAndRoundUp(TopLevelGridResolution.Y, FHVPT_AllocateBottomLevelGridCS::GetThreadGroupSize3D());
	GroupCount.Z = FMath::DivideAndRoundUp(TopLevelGridResolution.Z, FHVPT_AllocateBottomLevelGridCS::GetThreadGroupSize3D());

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(Scene->GetFeatureLevel());

	TShaderRef<FHVPT_AllocateBottomLevelGridCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_AllocateBottomLevelGridCS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("AllocateBottomLevelGrid"),
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		GroupCount
	);
}

void HVPT::Private::RasterizeVolumesIntoOrthoVoxelGrid(
	FRDGBuilder& GraphBuilder, 
	const FScene* Scene, 
	const FViewInfo& View, 
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches, 
	const FHVPT_VoxelGridBuildOptions BuildOptions, 
	FRDGBufferRef RasterTileBuffer, 
	FRDGBufferRef RasterTileAllocatorBuffer, 
	FBoxSphereBounds TopLevelGridBounds, 
	FIntVector TopLevelGridResolution, 
	FRDGBufferRef& TopLevelGridBuffer, 
	FRDGBufferRef& ExtinctionGridBuffer, 
	FRDGBufferRef& EmissionGridBuffer, 
	FRDGBufferRef& ScatteringGridBuffer, 
	FRDGBufferRef& VelocityGridBuffer
)
{
	// Setup indirect dispatch
	FRDGBufferRef RasterizeBottomLevelGridIndirectArgsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
		TEXT("HVPT.RasterizeBottomLevelGridIndirectArgs")
	);

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(Scene->GetFeatureLevel());

	{
		FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS::FParameters* IndirectArgsPassParameters = GraphBuilder.AllocParameters<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS::FParameters>();
		{
			IndirectArgsPassParameters->MaxDispatchThreadGroupsPerDimension = GRHIMaxDispatchThreadGroupsPerDimension;
			IndirectArgsPassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
			IndirectArgsPassParameters->RWRasterizeBottomLevelGridIndirectArgsBuffer = GraphBuilder.CreateUAV(RasterizeBottomLevelGridIndirectArgsBuffer, PF_R32_UINT);
		}

		FIntVector GroupCount(1, 1, 1);
		TShaderRef<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_SetRasterizeBottomLevelGridIndirectArgsCS>();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetRasterizeBottomLevelGridIndirectArgs"),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			ComputeShader,
			IndirectArgsPassParameters,
			GroupCount
		);
	}

	// Volume rasterization
	int32 BottomLevelGridBufferSize = (HVPT::GetMaxBottomLevelMemoryInMegabytesForOrthoGrid() * 1e6) / sizeof(FHVPT_GridData);

	ExtinctionGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.OrthoGrid.ExtinctionGridBuffer")
	);
	EmissionGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.OrthoGrid.EmissionGridBuffer")
	);
	ScatteringGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.OrthoGrid.ScatteringGridBuffer")
	);
	VelocityGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_GridData), BottomLevelGridBufferSize),
		TEXT("HVPT.OrthoGrid.VelocityGridBuffer")
	);

	FRDGBufferRef BottomLevelGridAllocatorBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2),
		TEXT("HVPT.OrthoGrid.BottomLevelGridAllocatorBuffer")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT), 0);

	for (auto MeshBatchIt = HeterogeneousVolumesMeshBatches.begin(); MeshBatchIt != HeterogeneousVolumesMeshBatches.end(); ++MeshBatchIt)
	{
		FVolumetricMeshBatch VolumetricMeshBatch = *MeshBatchIt;
		const FMeshBatch* Mesh = VolumetricMeshBatch.Mesh;
		const FPrimitiveSceneProxy* PrimitiveSceneProxy = VolumetricMeshBatch.Proxy;
		const FMaterialRenderProxy* MaterialRenderProxy = Mesh->MaterialRenderProxy;
		if (!HVPT::ShouldRenderMeshBatchWithHVPT(Mesh, PrimitiveSceneProxy, View.GetFeatureLevel()))
			continue;

		for (int32 VolumeIndex = 0; VolumeIndex < Mesh->Elements.Num(); ++VolumeIndex)
		{
			const IHeterogeneousVolumeInterface* HeterogeneousVolumeInterface = static_cast<const IHeterogeneousVolumeInterface*>(Mesh->Elements[VolumeIndex].UserData);

			bool bEnableVelocity = HVPT::ShouldWriteVelocity();
			if (HVPT::HasExtendedInterface(PrimitiveSceneProxy))
			{
				auto HeterogeneousVolumeExInterface = static_cast<const IHeterogeneousVolumeExInterface*>(HeterogeneousVolumeInterface);
				bEnableVelocity &= HeterogeneousVolumeExInterface->IsPlayingAnimation();
			}

			const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();
			const int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
			const FBoxSphereBounds LocalBoxSphereBounds = PrimitiveSceneProxy->GetLocalBounds();
			const FBoxSphereBounds PrimitiveBounds = PrimitiveSceneProxy->GetBounds();
			const FMaterial& Material = MaterialRenderProxy->GetMaterialWithFallback(View.GetFeatureLevel(), MaterialRenderProxy);

			FHVPT_RasterizeBottomLevelOrthoGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RasterizeBottomLevelOrthoGridCS::FParameters>();
			{
				// Scene data
				PassParameters->View = View.ViewUniformBuffer;
				PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);

				// Primitive data
				PassParameters->PrimitiveWorldBoundsMin = FVector3f(PrimitiveBounds.Origin - PrimitiveBounds.BoxExtent);
				PassParameters->PrimitiveWorldBoundsMax = FVector3f(PrimitiveBounds.Origin + PrimitiveBounds.BoxExtent);

				FMatrix InstanceToLocal = HeterogeneousVolumeInterface->GetInstanceToLocal();
				FMatrix LocalToWorld = HeterogeneousVolumeInterface->GetLocalToWorld();
				PassParameters->LocalToWorld = FMatrix44f(InstanceToLocal * LocalToWorld);
				PassParameters->WorldToLocal = FMatrix44f(PassParameters->LocalToWorld.Inverse());

				FMatrix LocalToInstance = InstanceToLocal.Inverse();
				FBoxSphereBounds InstanceBoxSphereBounds = LocalBoxSphereBounds.TransformBy(LocalToInstance);
				PassParameters->LocalBoundsOrigin = FVector3f(InstanceBoxSphereBounds.Origin);
				PassParameters->LocalBoundsExtent = FVector3f(InstanceBoxSphereBounds.BoxExtent);
				PassParameters->PrimitiveId = PrimitiveId;

				// Volume data
				PassParameters->TopLevelGridResolution = TopLevelGridResolution;
				PassParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
				PassParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);

				PassParameters->LocalToWorld_Velocity = FMatrix44f(HeterogeneousVolumeInterface->GetLocalToWorld());

				// Sampling data
				FBlueNoise BlueNoise = GetBlueNoiseGlobalParameters();
				PassParameters->BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

				// Unify with "object" definition??
				PassParameters->PrimitiveWorldBoundsMin = FVector3f(PrimitiveBounds.Origin - PrimitiveBounds.BoxExtent);
				PassParameters->PrimitiveWorldBoundsMax = FVector3f(PrimitiveBounds.Origin + PrimitiveBounds.BoxExtent);

				PassParameters->BottomLevelGridBufferSize = BottomLevelGridBufferSize;

				// Raster tile data
				PassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
				PassParameters->RasterTileBuffer = GraphBuilder.CreateSRV(RasterTileBuffer);

				// Indirect args
				PassParameters->IndirectArgs = RasterizeBottomLevelGridIndirectArgsBuffer;

				// Sampling mode
				PassParameters->bJitter = BuildOptions.bJitter;

				// Grid data
				PassParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(TopLevelGridBuffer);
				PassParameters->RWBottomLevelGridAllocatorBuffer = GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT);
				PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);
				PassParameters->RWExtinctionGridBuffer = GraphBuilder.CreateUAV(ExtinctionGridBuffer);
				PassParameters->RWEmissionGridBuffer = GraphBuilder.CreateUAV(EmissionGridBuffer);
				PassParameters->RWScatteringGridBuffer = GraphBuilder.CreateUAV(ScatteringGridBuffer);
				PassParameters->RWVelocityGridBuffer = GraphBuilder.CreateUAV(VelocityGridBuffer);
			}

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RasterizeBottomLevelGrid"),
				PassParameters,
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				[PassParameters, Scene, MaterialRenderProxy, &Material, bEnableVelocity](FRDGAsyncTask, FRHIComputeCommandList& RHICmdList)
				{
					FHVPT_RasterizeBottomLevelOrthoGridCS::FPermutationDomain PermutationVector;
					PermutationVector.Set<FHVPT_RasterizeBottomLevelOrthoGridCS::FEnableVelocity>(bEnableVelocity);
					TShaderRef<FHVPT_RasterizeBottomLevelOrthoGridCS> ComputeShader = Material.GetShader<FHVPT_RasterizeBottomLevelOrthoGridCS>(&FLocalVertexFactory::StaticType, PermutationVector, false);

					if (!ComputeShader.IsNull())
					{
						ClearUnusedGraphResources(ComputeShader, PassParameters);

						FMeshDrawShaderBindings ShaderBindings;
						UE::MeshPassUtils::SetupComputeBindings(ComputeShader, Scene, Scene->GetFeatureLevel(), nullptr, *MaterialRenderProxy, Material, ShaderBindings);

						UE::MeshPassUtils::DispatchIndirect(RHICmdList, ComputeShader, ShaderBindings, *PassParameters, PassParameters->IndirectArgs->GetIndirectRHICallBuffer(), 0);
					}
				}
			);
		}
	}

	// Once all volumes are rasterized, we can add the global fog into the grid so that is accumulated during path tracing
	if (HVPT::GetFogCompositingMode() == EFogCompositionMode::PostAndPathTracing)
	{
		const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];

		FHVPT_RasterizeFogOrthoGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RasterizeFogOrthoGridCS::FParameters>();
		// Scene data
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->Scene = View.GetSceneUniforms().GetBuffer(GraphBuilder);

		PassParameters->ExponentialFogParameters = View.ExponentialFogParameters;
		PassParameters->ExponentialFogParameters2 = View.ExponentialFogParameters2;
		PassParameters->ExponentialFogParameters3 = View.ExponentialFogParameters3;

		PassParameters->GlobalAlbedo = FogInfo.VolumetricFogAlbedo;
		PassParameters->GlobalEmissive = FogInfo.VolumetricFogEmissive;
		PassParameters->GlobalExtinctionScale = FogInfo.VolumetricFogExtinctionScale;

		// Volume data
		PassParameters->TopLevelGridResolution = TopLevelGridResolution;
		PassParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
		PassParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);

		// Sampling data
		FBlueNoise BlueNoise = GetBlueNoiseGlobalParameters();
		PassParameters->BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

		PassParameters->BottomLevelGridBufferSize = BottomLevelGridBufferSize;

		// Raster tile data
		PassParameters->RasterTileAllocatorBuffer = GraphBuilder.CreateSRV(RasterTileAllocatorBuffer, PF_R32_UINT);
		PassParameters->RasterTileBuffer = GraphBuilder.CreateSRV(RasterTileBuffer);

		// Indirect args
		PassParameters->IndirectArgs = RasterizeBottomLevelGridIndirectArgsBuffer;

		// Sampling mode
		PassParameters->bJitter = BuildOptions.bJitter;

		// Grid data
		PassParameters->RWBottomLevelGridAllocatorBuffer = GraphBuilder.CreateUAV(BottomLevelGridAllocatorBuffer, PF_R32_UINT);
		PassParameters->RWTopLevelGridBuffer = GraphBuilder.CreateUAV(TopLevelGridBuffer);
		PassParameters->RWExtinctionGridBuffer = GraphBuilder.CreateUAV(ExtinctionGridBuffer);
		PassParameters->RWEmissionGridBuffer = GraphBuilder.CreateUAV(EmissionGridBuffer);
		PassParameters->RWScatteringGridBuffer = GraphBuilder.CreateUAV(ScatteringGridBuffer);
		PassParameters->RWVelocityGridBuffer = GraphBuilder.CreateUAV(VelocityGridBuffer);

		TShaderRef<FHVPT_RasterizeFogOrthoGridCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_RasterizeFogOrthoGridCS>();

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RasterizeFogOrthoGridCS"),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			[PassParameters, ComputeShader](FRDGAsyncTask, FRHIComputeCommandList& RHICmdList)
			{
				ClearUnusedGraphResources(ComputeShader, PassParameters);
				FComputeShaderUtils::DispatchIndirect(RHICmdList, ComputeShader, *PassParameters, PassParameters->IndirectArgs->GetIndirectRHICallBuffer(), 0);
			}
		);
	}
}

void HVPT::Private::BuildMajorantVoxelGrid(
	FRDGBuilder& GraphBuilder, 
	const FScene* Scene, 
	const FIntVector& TopLevelGridResolution, 
	FRDGBufferRef TopLevelGridBuffer, 
	FRDGBufferRef ExtinctionGridBuffer, 
	FRDGBufferRef& MajorantVoxelGridBuffer
)
{
	uint32 MajorantVoxelGridBufferSize = TopLevelGridResolution.X * TopLevelGridResolution.Y * TopLevelGridResolution.Z;
	MajorantVoxelGridBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_MajorantGridData), MajorantVoxelGridBufferSize),
		TEXT("HVPT.MajorantVoxelGridBuffer")
	);

	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(Scene->GetFeatureLevel());

	{
		FHVPT_BuildMajorantVoxelGridCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_BuildMajorantVoxelGridCS::FParameters>();
		{
			PassParameters->TopLevelGridResolution = TopLevelGridResolution;
			PassParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(TopLevelGridBuffer);
			PassParameters->ExtinctionGridBuffer = GraphBuilder.CreateSRV(ExtinctionGridBuffer);
			PassParameters->RWMajorantVoxelGridBuffer = GraphBuilder.CreateUAV(MajorantVoxelGridBuffer);
		}

		TShaderRef<FHVPT_BuildMajorantVoxelGridCS> ComputeShader = GlobalShaderMap->GetShader<FHVPT_BuildMajorantVoxelGridCS>();

		FIntVector GroupCount = FIntVector(
			FMath::DivideAndRoundUp(TopLevelGridResolution.X, FHVPT_BuildMajorantVoxelGridCS::GetThreadGroupSize3D()),
			FMath::DivideAndRoundUp(TopLevelGridResolution.Y, FHVPT_BuildMajorantVoxelGridCS::GetThreadGroupSize3D()),
			FMath::DivideAndRoundUp(TopLevelGridResolution.Z, FHVPT_BuildMajorantVoxelGridCS::GetThreadGroupSize3D())
		);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BuildMajorantVoxelGridCS"),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			ComputeShader,
			PassParameters,
			GroupCount
		);
	}
}
