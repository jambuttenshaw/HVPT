#include "HVPTViewExtension.h"

#include "RenderGraphBuilder.h"
#include "SceneCore.h"
#include "ScenePrivate.h"
#include "SceneTextureParameters.h"

#include "HVPT.h"
#include "HVPTDefinitions.h"
#include "HVPTViewState.h"

#include "PathTracing.h"
#include "PathTracingLightGrid.h"

#include "RayTracingShaderBindingLayout.h"
#include "Helpers.h"

// Max bounces supported by ReSTIR pipeline
constexpr uint32 kReSTIRMaxBounces = 8;
constexpr uint32 kReSTIRMaxSpatialSamples = 8;


static TAutoConsoleVariable<bool> CVarHVPTReSTIRUseDispatchIndirect(
	TEXT("r.HVPT.ReSTIR.UseDispatchIndirect"),
	false,
	TEXT("Uses dispatch indirect to only dispatch threads that will definitely process volume media."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRDeferEvaluateCandidateF(
	TEXT("r.HVPT.ReSTIR.DeferEvaluateCandidateF"),
	true,
	TEXT("Defers evaluating candidate path contribution to a separate path to help reduce thread divergence."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRDeferSurfaceBounces(
	TEXT("r.HVPT.ReSTIR.DeferSurfaceBounces"),
	true,
	TEXT("Defers tracing rays into scene for surface candidates until a separate pass to help reduce thread divergence."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTReSTIRDeferredBounceBufferSize(
	TEXT("r.HVPT.ReSTIR.DeferredBounceBufferSize"),
	1'000'000,
	TEXT("Amount of space allocated for deferred bounces, in number of bounces. Any required bounces that do not fit in buffer will be traced immediately and not deferred."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRMultiPassSpatialReuse(
	TEXT("r.HVPT.ReSTIR.SpatialReuse.MultiPass"),
	true,
	TEXT("Whether to use multi-pass pipeline for spatial reuse."),
	ECVF_RenderThreadSafe
);

static bool DeferSurfaceHits()
{
	return (HVPT::GetMaxBounces() > 1) && HVPT::UseSurfaceContributions() && CVarHVPTReSTIRDeferSurfaceBounces.GetValueOnRenderThread();
}


#if RHI_RAYTRACING


// Extracted into its own struct to be able to construct it once and re-use for each pass
// Taken from path tracer implementation, to be able to use path tracer light sampling logic
BEGIN_SHADER_PARAMETER_STRUCT(FPathTracingLightParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
	SHADER_PARAMETER(uint32, SceneLightCount)
	SHADER_PARAMETER(uint32, SceneVisibleLightCount)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)
	SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)
END_SHADER_PARAMETER_STRUCT()


BEGIN_SHADER_PARAMETER_STRUCT(FReSTIRCommonParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
	SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
	SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)

	SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightParameters, LightParameters)
	SHADER_PARAMETER_STRUCT_INCLUDE(FHVPT_PathTracingFogParameters, FogParameters)

	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTOrthoGridUniformBufferParameters, OrthoGridUniformBuffer)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTFrustumGridUniformBufferParameters, FrustumGridUniformBuffer)

	SHADER_PARAMETER(uint32, TemporalSeed)

	SHADER_PARAMETER(uint32, NumBounces)

	// For indirect dispatch
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReservoirIndices)
	RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

	// Debug tools
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWDebugTexture)
	SHADER_PARAMETER(uint32, DebugFlags) // To represent debug modes etc
END_SHADER_PARAMETER_STRUCT()


class FReSTIRDispatchRaysDispatcherCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRDispatchRaysDispatcherCS);
	SHADER_USE_PARAMETER_STRUCT(FReSTIRDispatchRaysDispatcherCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWAllocatorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWReservoirIndices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static uint32 GetThreadGroupSize2D() { return 8; }
	static uint32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRDispatchRaysDispatcherCS, "/Plugin/HVPT/Private/ReSTIR/Dispatcher.usf", "ReSTIRDispatchRaysDispatcherCS", SF_Compute)


class FReSTIRBaseRGS : public FGlobalShader
{
public:

	class FMultipleBounces : SHADER_PERMUTATION_BOOL("MULTIPLE_BOUNCES");
	class FUseSurfaceContributions : SHADER_PERMUTATION_BOOL("USE_SURFACE_CONTRIBUTIONS");
	//class FApplyVolumetricFog : SHADER_PERMUTATION_BOOL("APPLY_VOLUMETRIC_FOG");
	class FUseSER : SHADER_PERMUTATION_BOOL("USE_SER");
	class FUseDispatchIndirect : SHADER_PERMUTATION_BOOL("USE_DISPATCH_INDIRECT");
	class FDebugOutputEnabled : SHADER_PERMUTATION_BOOL("DEBUG_OUTPUT_ENABLED");
	using FPermutationDomain = TShaderPermutationDomain<FMultipleBounces,
														FUseSurfaceContributions,
														//FApplyVolumetricFog,
														FUseSER,
														FUseDispatchIndirect,
														FDebugOutputEnabled>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform)
			&& HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("MAX_EXTRA_BOUNCES"), kReSTIRMaxBounces - 1);
		OutEnvironment.SetDefine(TEXT("MAX_SPATIAL_SAMPLES"), kReSTIRMaxSpatialSamples);

		OutEnvironment.CompilerFlags.Add(CFLAG_AllowRealTypes);
	}

	static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
	{
		return ERayTracingPayloadType::RayTracingMaterial;
	}

	static const FShaderBindingLayout* GetShaderBindingLayout(const FShaderPermutationParameters& Parameters)
	{
		return HVPT::Private::GetShaderBindingLayout(Parameters.Platform);
	}
};


class FReSTIRCandidateGenerationRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRCandidateGenerationRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRCandidateGenerationRGS, FReSTIRBaseRGS);

	class FDeferEvaluateF : SHADER_PERMUTATION_BOOL("DEFER_EVALUATE_F");
	class FDeferSurfaceHits : SHADER_PERMUTATION_BOOL("DEFER_SURFACE_HITS");
	using FPermutationDomain = TShaderPermutationDomain<FReSTIRBaseRGS::FMultipleBounces,
														FReSTIRBaseRGS::FUseSurfaceContributions,
														FReSTIRBaseRGS::FUseSER,
														FReSTIRBaseRGS::FUseDispatchIndirect,
														FReSTIRBaseRGS::FDebugOutputEnabled,
														FDeferEvaluateF,
														FDeferSurfaceHits>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepthTexture_Copy)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER(uint32, NumInitialCandidates)
		SHADER_PARAMETER(uint32, bUseShadowTermForCandidateGeneration)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWExtraBounces)

		// Deferred surface bounces
		SHADER_PARAMETER(uint32, SurfaceBounceAllocatorSize)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWDeferredSurfaceBounceAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_DeferredSurfaceBounce>, RWDeferredSurfaceBounces)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWDeferredSurfaceExtraBounces)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRCandidateGenerationRGS, "/Plugin/HVPT/Private/ReSTIR/CandidateGeneration.usf", "ReSTIRCandidateGenerationRGS", SF_RayGen)

class FReSTIRCandidateEvaluateSurfaceBouncesRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRCandidateEvaluateSurfaceBouncesRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRCandidateEvaluateSurfaceBouncesRGS, FReSTIRBaseRGS);

	using FPermutationDomain = TShaderPermutationDomain<FReSTIRBaseRGS::FUseSER,
														FReSTIRBaseRGS::FDebugOutputEnabled>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER(uint32, bUseShadowTermForCandidateGeneration)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_DeferredSurfaceBounce>, DeferredSurfaceBounces)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, DeferredSurfaceExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWExtraBounces)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FReSTIRBaseRGS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine("MULTIPLE_BOUNCES", true);
		OutEnvironment.SetDefine("USE_SURFACE_CONTRIBUTIONS", true);
		OutEnvironment.SetDefine("DEFER_SURFACE_HITS", true);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRCandidateEvaluateSurfaceBouncesRGS, "/Plugin/HVPT/Private/ReSTIR/CandidateGeneration.usf", "ReSTIRCandidateEvaluateSurfaceBouncesRGS", SF_RayGen)

class FReSTIRCandidateEvaluateFRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRCandidateEvaluateFRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRCandidateEvaluateFRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, ExtraBounces)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRCandidateEvaluateFRGS, "/Plugin/HVPT/Private/ReSTIR/CandidateGeneration.usf", "ReSTIRCandidateEvaluateFRGS", SF_RayGen)

class FReSTIRTemporalReuseRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRTemporalReuseRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRTemporalReuseRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, TemporalFeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, PreviousReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, PreviousExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWCurrentExtraBounces)

		SHADER_PARAMETER(float, TemporalHistoryThreshold)
		SHADER_PARAMETER(uint32, bEnableTemporalReprojection)
		SHADER_PARAMETER(uint32, bTalbotMIS)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRTemporalReuseRGS, "/Plugin/HVPT/Private/ReSTIR/TemporalReuse.usf", "ReSTIRTemporalReuseRGS", SF_RayGen)

class FReSTIRSpatialReuseRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRSpatialReuseRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRSpatialReuseRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, InReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, InExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWOutReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWOutExtraBounces)

		SHADER_PARAMETER(uint32, NumSpatialSamples)
		SHADER_PARAMETER(float, SpatialReuseRadius)
		SHADER_PARAMETER(uint32, bTalbotMIS)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRSpatialReuseRGS, "/Plugin/HVPT/Private/ReSTIR/SpatialReuse.usf", "ReSTIRSpatialReuseRGS", SF_RayGen)

BEGIN_SHADER_PARAMETER_STRUCT(FReSTIRMultiPassSpatialReuseCommonParameters, )
	SHADER_PARAMETER(uint32, NumSpatialSamples)
	SHADER_PARAMETER(float, SpatialReuseRadius)

	SHADER_PARAMETER(FIntPoint, TileStart)
	SHADER_PARAMETER(FIntPoint, TileSize)
	SHADER_PARAMETER(FIntPoint, BufferedTileSize)

	SHADER_PARAMETER(int32, TileBufferZoneWidth)

	SHADER_PARAMETER(int32, DomainsPerReservoir)		// Total domains per reservoir
	SHADER_PARAMETER(int32, SqrtDomainsPerReservoir)	// The domains form a square within a reservoir. This is the side length of that square
END_SHADER_PARAMETER_STRUCT()

class FReSTIRSpatialReuse_ChooseNeighboursCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRSpatialReuse_ChooseNeighboursCS);
	SHADER_USE_PARAMETER_STRUCT(FReSTIRSpatialReuse_ChooseNeighboursCS, FGlobalShader);

	class FDebugOutputEnabled : SHADER_PERMUTATION_BOOL("DEBUG_OUTPUT_ENABLED");
	using FPermutationDomain = TShaderPermutationDomain<FDebugOutputEnabled>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRMultiPassSpatialReuseCommonParameters, SpatialReuseCommon)

		SHADER_PARAMETER(uint32, TemporalSeed)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, InReservoirs)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint16_t>, RWNeighbourIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint16_t>, RWEvaluationResults)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWEvaluationIndirectionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer<uint>, RWIndirectionAllocator)

		// Debug tools
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWDebugTexture)
		SHADER_PARAMETER(uint32, DebugFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());

		OutEnvironment.SetDefine(TEXT("MAX_EXTRA_BOUNCES"), kReSTIRMaxBounces - 1);
		OutEnvironment.SetDefine(TEXT("MAX_SPATIAL_SAMPLES"), kReSTIRMaxSpatialSamples);

		OutEnvironment.CompilerFlags.Add(CFLAG_AllowRealTypes);
		OutEnvironment.CompilerFlags.Add(CFLAG_InlineRayTracing);
	}

	static uint32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRSpatialReuse_ChooseNeighboursCS, "/Plugin/HVPT/Private/ReSTIR/SpatialReuse_MultiPass.usf", "ReSTIRSpatialReuse_ChooseNeighboursCS", SF_Compute)

class FReSTIRSpatialReuse_EvaluateRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRSpatialReuse_EvaluateRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRSpatialReuse_EvaluateRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRMultiPassSpatialReuseCommonParameters, SpatialReuseCommon)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, InReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, InExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EvaluationIndirectionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint16_t>, RWEvaluationResults)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRSpatialReuse_EvaluateRGS, "/Plugin/HVPT/Private/ReSTIR/SpatialReuse_MultiPass.usf", "ReSTIRSpatialReuse_EvaluateRGS", SF_RayGen)

class FReSTIRSpatialReuse_GatherAndReuseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRSpatialReuse_GatherAndReuseCS);
	SHADER_USE_PARAMETER_STRUCT(FReSTIRSpatialReuse_GatherAndReuseCS, FGlobalShader);

	class FMultipleBounces : SHADER_PERMUTATION_BOOL("MULTIPLE_BOUNCES");
	class FDebugOutputEnabled : SHADER_PERMUTATION_BOOL("DEBUG_OUTPUT_ENABLED");
	using FPermutationDomain = TShaderPermutationDomain<FMultipleBounces, FDebugOutputEnabled>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRMultiPassSpatialReuseCommonParameters, SpatialReuseCommon)

		SHADER_PARAMETER(uint32, TemporalSeed)

		SHADER_PARAMETER(uint32, NumBounces)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, InReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, InExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint16_t>, NeighbourIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint16_t>, EvaluationResults)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWOutReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWOutExtraBounces)

		// Debug tools
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWDebugTexture)
		SHADER_PARAMETER(uint32, DebugFlags)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());

		OutEnvironment.SetDefine(TEXT("MAX_EXTRA_BOUNCES"), kReSTIRMaxBounces - 1);
		OutEnvironment.SetDefine(TEXT("MAX_SPATIAL_SAMPLES"), kReSTIRMaxSpatialSamples);

		OutEnvironment.CompilerFlags.Add(CFLAG_AllowRealTypes);
		OutEnvironment.CompilerFlags.Add(CFLAG_InlineRayTracing);
	}

	static uint32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRSpatialReuse_GatherAndReuseCS, "/Plugin/HVPT/Private/ReSTIR/SpatialReuse_MultiPass.usf", "ReSTIRSpatialReuse_GatherAndReuseCS", SF_Compute)

class FReSTIRFinalShadingRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRFinalShadingRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRFinalShadingRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		// Input reservoirs
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, CurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, ExtraBounces)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadianceTexture)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRFinalShadingRGS, "/Plugin/HVPT/Private/ReSTIR/FinalShading.usf", "ReSTIRFinalShadingRGS", SF_RayGen)


class FReSTIRDebugVisualizationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRDebugVisualizationCS);
	SHADER_USE_PARAMETER_STRUCT(FReSTIRDebugVisualizationCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, Reservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, ExtraBounces)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static uint32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRDebugVisualizationCS, "/Plugin/HVPT/Private/ReSTIR/ReservoirVisualization.usf", "ReSTIRDebugVisualizationCS", SF_Compute);


template<typename Shader>
typename Shader::FPermutationDomain CreatePermutation(const FHVPTViewState& State)
{
	typename Shader::FPermutationDomain Permutation = typename Shader::FPermutationDomain{};
	Permutation.Set<typename Shader::FMultipleBounces>(HVPT::GetMaxBounces() > 1);
	Permutation.Set<typename Shader::FUseSurfaceContributions>(HVPT::UseSurfaceContributions());
	//Permutation.Set<typename Shader::FApplyVolumetricFog>(HVPT::GetFogCompositingMode() == HVPT::EFogCompositionMode::PostAndPathTracing);
	Permutation.Set<typename Shader::FUseSER>(HVPT::ShouldUseSER());
	Permutation.Set<typename Shader::FUseDispatchIndirect>(CVarHVPTReSTIRUseDispatchIndirect.GetValueOnRenderThread());
	Permutation.Set<typename Shader::FDebugOutputEnabled>(State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE);
	return Permutation;
}

void HVPT::PrepareRaytracingShadersReSTIR(const FViewInfo& View, const FHVPTViewState& State, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	auto ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

	auto AddShader = [&]<typename T>(typename T::FPermutationDomain Permutation)
	{
		OutRayGenShaders.Add(ShaderMap->GetShader<T>(Permutation).GetRayTracingShader());
	};

	// AddShader<T>() does not compile
	{
		FReSTIRCandidateGenerationRGS::FPermutationDomain Permutation = CreatePermutation<FReSTIRCandidateGenerationRGS>(State);
		Permutation.Set<FReSTIRCandidateGenerationRGS::FDeferEvaluateF>(CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread());
		Permutation.Set<FReSTIRCandidateGenerationRGS::FDeferSurfaceHits>(DeferSurfaceHits());
		AddShader.template operator()<FReSTIRCandidateGenerationRGS>(Permutation);
	}
	if (DeferSurfaceHits())
	{
		FReSTIRCandidateEvaluateSurfaceBouncesRGS::FPermutationDomain Permutation;
		Permutation.Set<FReSTIRCandidateEvaluateSurfaceBouncesRGS::FUseSER>(HVPT::ShouldUseSER());
		Permutation.Set<FReSTIRCandidateEvaluateSurfaceBouncesRGS::FDebugOutputEnabled>(State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE);
		OutRayGenShaders.Add(ShaderMap->GetShader<FReSTIRCandidateEvaluateSurfaceBouncesRGS>(Permutation).GetRayTracingShader());
	}
	if (CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread())
		AddShader.template operator()<FReSTIRCandidateEvaluateFRGS>(CreatePermutation<FReSTIRCandidateEvaluateFRGS>(State));
	AddShader.template operator()<FReSTIRTemporalReuseRGS>(CreatePermutation<FReSTIRTemporalReuseRGS>(State));
	if (CVarHVPTReSTIRMultiPassSpatialReuse.GetValueOnRenderThread())
		AddShader.template operator()<FReSTIRSpatialReuse_EvaluateRGS>(CreatePermutation<FReSTIRSpatialReuseRGS>(State));
	else
		AddShader.template operator()<FReSTIRSpatialReuseRGS>(CreatePermutation<FReSTIRSpatialReuseRGS>(State));
	AddShader.template operator()<FReSTIRFinalShadingRGS>(CreatePermutation<FReSTIRFinalShadingRGS>(State));
}


template <typename Shader>
void AddRaytracingPass(
	FRDGBuilder& GraphBuilder,
	FRDGEventName&& EventName,
	const FViewInfo& View,
	const FHVPTViewState& State,
	typename Shader::FParameters* PassParameters,
	FRDGBufferRef ArgumentBuffer,
	uint32 ArgumentOffset = 0
)
{
	typename Shader::FPermutationDomain Permutation = CreatePermutation<Shader>(State);
	AddRaytracingPass<Shader>(GraphBuilder, std::move(EventName), View, State, PassParameters, Permutation, ArgumentBuffer, ArgumentOffset);
}

template <typename Shader>
void AddRaytracingPass(
	FRDGBuilder& GraphBuilder,
	FRDGEventName&& EventName,
	const FViewInfo& ViewInfo,
	const FHVPTViewState& State,
	typename Shader::FParameters* PassParameters,
	typename Shader::FPermutationDomain& Permutation,
	FRDGBufferRef ArgumentBuffer,
	uint32 ArgumentOffset = 0
)
{
	bool bDispatchIndirect = ArgumentBuffer != nullptr;
	FRHIUniformBuffer* SceneUniformBuffer = ViewInfo.GetSceneUniforms().GetBufferRHI(GraphBuilder);

	TShaderMapRef<Shader> RayGenShader(ViewInfo.ShaderMap, Permutation);

	GraphBuilder.AddPass(
		std::move(EventName),
		PassParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[PassParameters, SceneUniformBuffer, RayGenShader, ArgumentBuffer, ArgumentOffset, bDispatchIndirect, &ViewInfo](FRHICommandList& RHICmdList)
		{
			if (ArgumentBuffer)
				ArgumentBuffer->MarkResourceAsUsed();

			FRHIBatchedShaderParameters& GlobalResources = RHICmdList.GetScratchShaderParameters();
			SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
			TOptional<FScopedUniformBufferStaticBindings> StaticUniformBufferScope =
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				HVPT::Private::BindStaticUniformBufferBindings(ViewInfo, SceneUniformBuffer, Nanite::GetPublicGlobalRayTracingUniformBuffer()->GetRHI(), RHICmdList);
#else
				RayTracing::BindStaticUniformBufferBindings(ViewInfo, SceneUniformBuffer, RHICmdList);
#endif

			if (bDispatchIndirect)
			{
				RHICmdList.RayTraceDispatchIndirect(
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					ViewInfo.MaterialRayTracingData.PipelineState,
#else
					ViewInfo.RayTracingMaterialPipeline,
#endif
					RayGenShader.GetRayTracingShader(),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					ViewInfo.MaterialRayTracingData.ShaderBindingTable,
#else
					ViewInfo.RayTracingSBT,
#endif
					GlobalResources,
					ArgumentBuffer->GetRHI(),
					ArgumentOffset
				);
			}
			else
			{
				auto DispatchSize = ViewInfo.ViewRect.Size();
				RHICmdList.RayTraceDispatch(
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					ViewInfo.MaterialRayTracingData.PipelineState,
#else
					ViewInfo.RayTracingMaterialPipeline,
#endif
					RayGenShader.GetRayTracingShader(),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					ViewInfo.MaterialRayTracingData.ShaderBindingTable,
#else
					ViewInfo.RayTracingSBT,
#endif
					GlobalResources,
					DispatchSize.X,
					DispatchSize.Y
				);
			}
		});
}


void HVPT::RenderWithReSTIRPathTracing(
	FRDGBuilder& GraphBuilder, const FScene& Scene, const FViewInfo& ViewInfo, const FSceneTextures& SceneTextures, FHVPTViewState& State
)
{
	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR");

	// Create resources
	const auto& Extent = ViewInfo.ViewRect.Size();
	const uint32 NumReservoirs = Extent.X * Extent.Y;

	bool bHasTemporalFeatureTexture = State.TemporalFeatureTexture != nullptr;

	auto ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_Reservoir), NumReservoirs);
	// When max bounces is 1 then extra bounce buffer is not needed - just creates buffer with 1 element
	auto ExtraBounceDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_Bounce),
		FMath::Max(NumReservoirs * static_cast<uint32>(HVPT::GetMaxBounces() - 1), 1u));

	FRDGBufferRef ReservoirsA = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("HVPT.ReservoirsA"));
	FRDGBufferRef ExtraBouncesA = GraphBuilder.CreateBuffer(ExtraBounceDesc, TEXT("HVPT.ExtraBouncesA"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ReservoirsA), 0);
	AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(ExtraBouncesA), 0.0f);

	FRDGBufferRef ReservoirsB = nullptr;
	FRDGBufferRef ExtraBouncesB = nullptr;
	bool bValidHistory = (State.ReSTIRReservoirCache.IsValid() && State.ReSTIRReservoirCache->Desc == ReservoirDesc)
					  && ((HVPT::GetMaxBounces() == 1) || State.ReSTIRExtraBounceCache.IsValid() && State.ReSTIRExtraBounceCache->Desc == ExtraBounceDesc);
	if (bValidHistory)
	{
		ReservoirsB = GraphBuilder.RegisterExternalBuffer(State.ReSTIRReservoirCache);
		if (HVPT::GetMaxBounces() > 1)
			ExtraBouncesB = GraphBuilder.RegisterExternalBuffer(State.ReSTIRExtraBounceCache);
	}
	else
	{
		// No valid history - create empty buffers
		ReservoirsB = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("HVPT.ReservoirsB"));
		ExtraBouncesB = GraphBuilder.CreateBuffer(ExtraBounceDesc, TEXT("HVPT.ExtraBouncesB"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ReservoirsB), 0);
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(ExtraBouncesB), 0.0f);
	}

	// Creating light parameters can be done once and reused between passes
	FPathTracingLightParameters LightParameters;
	HVPT::Private::SetPathTracingLightParameters(
		GraphBuilder,
		ViewInfo,
		false, // bUseAtmosphere
		&LightParameters.SkylightParameters,
		&LightParameters.LightGridParameters,
		&LightParameters.SceneVisibleLightCount,
		&LightParameters.SceneLightCount,
		&LightParameters.SceneLights
	);
	FHVPT_PathTracingFogParameters FogParameters = HVPT::Private::PrepareFogParameters(ViewInfo, Scene.ExponentialFogs[0]);

	
	FRDGBufferRef DispatchRaysIndirectArgumentBuffer = nullptr;
	FRDGBufferRef ReservoirIndicesBuffer = nullptr;
	if (CVarHVPTReSTIRUseDispatchIndirect.GetValueOnRenderThread())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Dispatcher)");

		DispatchRaysIndirectArgumentBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("HVPT.ReSTIR.DispatchRaysIndirectArgs"));
		ReservoirIndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumReservoirs), TEXT("HVPT.ReSTIR.ReservoirIndices"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DispatchRaysIndirectArgumentBuffer), 0);
		// Execute dispatcher
		{
			FReSTIRDispatchRaysDispatcherCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRDispatchRaysDispatcherCS::FParameters>();
			PassParameters->View = ViewInfo.ViewUniformBuffer;
			PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);
			PassParameters->RWAllocatorBuffer = GraphBuilder.CreateUAV(DispatchRaysIndirectArgumentBuffer);
			PassParameters->RWReservoirIndices = GraphBuilder.CreateUAV(ReservoirIndicesBuffer);
			TShaderMapRef<FReSTIRDispatchRaysDispatcherCS> ComputeShader(ViewInfo.ShaderMap);

			const auto GroupCount = FComputeShaderUtils::GetGroupCount(Extent, FReSTIRDispatchRaysDispatcherCS::GetThreadGroupSize2D());
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ReSTIRDispatcher"),
				ERDGPassFlags::Compute,
				ComputeShader,
				PassParameters,
				GroupCount
			);
		}
	}

	uint32 MaxNumPasses = 4;
	MaxNumPasses += CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread() ? 1 : 0;
	MaxNumPasses += (State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE) ? 1 : 0;
	MaxNumPasses += CVarHVPTReSTIRMultiPassSpatialReuse.GetValueOnRenderThread() ? 24 : 0; // TODO: Actually calculate how many passes based off number of tiles
	uint32 TemporalSeedOffset = 0;
	auto PopulateCommonParameters = [&](FReSTIRCommonParameters* Parameters)
		{
			Parameters->View = ViewInfo.ViewUniformBuffer;
			Parameters->SceneTextures = HVPT::Private::GetSceneTextureParameters(GraphBuilder, SceneTextures);
			Parameters->Scene = ViewInfo.GetSceneUniforms().GetBuffer(GraphBuilder);
			Parameters->TLAS = Scene.RayTracingScene.GetLayerView(ERayTracingSceneLayer::Base);

			Parameters->LightParameters = LightParameters;
			Parameters->FogParameters = FogParameters;

			Parameters->OrthoGridUniformBuffer = State.OrthoGridUniformBuffer;
			Parameters->FrustumGridUniformBuffer = State.FrustumGridUniformBuffer;

			uint32 FrameIndex = ViewInfo.ViewState ? ViewInfo.ViewState->FrameIndex : 0;
			Parameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : MaxNumPasses * FrameIndex + TemporalSeedOffset++;

			Parameters->NumBounces = FMath::Clamp(HVPT::GetMaxBounces(), 1, kReSTIRMaxBounces);

			if (CVarHVPTReSTIRUseDispatchIndirect.GetValueOnRenderThread())
			{
				Parameters->ReservoirIndices = GraphBuilder.CreateSRV(ReservoirIndicesBuffer);
				Parameters->IndirectArgs = DispatchRaysIndirectArgumentBuffer;
			}
			if (State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE)
			{
				Parameters->RWDebugTexture = GraphBuilder.CreateUAV(State.DebugTexture);
				Parameters->DebugFlags = State.DebugFlags;
			}
		};

	// Candidate Generation
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Candidate Generation)");

		uint32 NumCandidates = HVPT::GetNumInitialCandidates();

		// For indirect surface bounces
		bool bDeferSurfaceHits = DeferSurfaceHits();
		// Buffer allocation is split evenly among candidates
		// Each candidate allocates into a different region of the buffer
		// This is to allow deferred candidates to be evaluated over multiple passes to prevent race conditions on the reservoir buffer
		uint32 MaxDeferredSurfaceBounces = CVarHVPTReSTIRDeferredBounceBufferSize.GetValueOnRenderThread();
		uint32 MaxDeferredSurfaceBouncesPerCandidate = MaxDeferredSurfaceBounces / NumCandidates;

		if (bDeferSurfaceHits && !CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread())
		{
			UE_LOG(LogHVPT, Error, TEXT("When using r.HVPT.ReSTIR.DeferSurfaceBounces, you must also enable r.HVPT.ReSTIR.DeferEvaluateCandidateF"));
		}

		FRDGBufferRef DeferredSurfaceBounceAllocator = nullptr;
		FRDGBufferRef DeferredSurfaceBounces = nullptr;
		FRDGBufferRef DeferredSurfaceExtraBounces = nullptr;
		if (bDeferSurfaceHits)
		{
			DeferredSurfaceBounceAllocator = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumCandidates), TEXT("HVPT.ReSTIR.DeferredSurfaceBounceAllocator"));
			DeferredSurfaceBounces = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_DeferredSurfaceBounce), MaxDeferredSurfaceBounces), TEXT("HVPT.ReSTIR.DeferredSurfaceBounces"));
			DeferredSurfaceExtraBounces = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_Bounce), MaxDeferredSurfaceBounces * HVPT::GetMaxBounces()), TEXT("HVPT.ReSTIR.DeferredSurfaceExtraBounces"));

			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DeferredSurfaceBounceAllocator), 0);
		}

		{
			FReSTIRCandidateGenerationRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRCandidateGenerationRGS::FParameters>();
			PopulateCommonParameters(&PassParameters->Common);

			PassParameters->SceneDepthTexture_Copy = GraphBuilder.CreateSRV(State.DepthBufferCopy);
			PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

			PassParameters->NumInitialCandidates = NumCandidates;
			PassParameters->bUseShadowTermForCandidateGeneration = HVPT::GetUseShadowTermForCandidateGeneration();

			PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
			if (HVPT::GetMaxBounces() > 1)
				PassParameters->RWExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesA);

			if (bDeferSurfaceHits)
			{
				PassParameters->SurfaceBounceAllocatorSize = MaxDeferredSurfaceBouncesPerCandidate;
				PassParameters->RWDeferredSurfaceBounceAllocator = GraphBuilder.CreateUAV(DeferredSurfaceBounceAllocator);
				PassParameters->RWDeferredSurfaceBounces = GraphBuilder.CreateUAV(DeferredSurfaceBounces);
				PassParameters->RWDeferredSurfaceExtraBounces = GraphBuilder.CreateUAV(DeferredSurfaceExtraBounces);
			}

			FReSTIRCandidateGenerationRGS::FPermutationDomain Permutation = CreatePermutation<FReSTIRCandidateGenerationRGS>(State);
			Permutation.Set<FReSTIRCandidateGenerationRGS::FDeferEvaluateF>(CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread());
			Permutation.Set<FReSTIRCandidateGenerationRGS::FDeferSurfaceHits>(DeferSurfaceHits());
			AddRaytracingPass<FReSTIRCandidateGenerationRGS>(
				GraphBuilder,
				RDG_EVENT_NAME("ReSTIRCandidateGeneration"),
				ViewInfo, 
				State,
				PassParameters,
				Permutation,
				DispatchRaysIndirectArgumentBuffer
			);
		}
		if (bDeferSurfaceHits)
		{
			// Multiple candidates for the same pixel may want to defer surface bounce evaluation.
			// Evaluating the surface bounce requires both reading and writing to the reservoir buffer.
			// Multiple passes are required to prevent race conditions on the reservoir buffer.
			for (uint32 DeferredSurfaceBouncePass = 0; DeferredSurfaceBouncePass < NumCandidates; DeferredSurfaceBouncePass++)
			{
				// Setup indirect arguments
				FRDGBufferRef DispatchDeferredSurfaceBounceIndirectArguments = FComputeShaderUtils::AddIndirectArgsSetupCsPass1D(
					GraphBuilder, ViewInfo.FeatureLevel, DeferredSurfaceBounceAllocator, TEXT("HVPT.ReSTIR.DeferredSurfaceBouncesIndirectArguments"), 1, DeferredSurfaceBouncePass);

				FReSTIRCandidateEvaluateSurfaceBouncesRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRCandidateEvaluateSurfaceBouncesRGS::FParameters>();
				PopulateCommonParameters(&PassParameters->Common);
				PassParameters->Common.IndirectArgs = DispatchDeferredSurfaceBounceIndirectArguments;

				PassParameters->bUseShadowTermForCandidateGeneration = HVPT::GetUseShadowTermForCandidateGeneration();

				PassParameters->DeferredSurfaceBounces = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc{ DeferredSurfaceBounces,
					DeferredSurfaceBouncePass * MaxDeferredSurfaceBouncesPerCandidate * static_cast<uint32>(sizeof(FHVPT_DeferredSurfaceBounce)),
					MaxDeferredSurfaceBouncesPerCandidate });
				PassParameters->DeferredSurfaceExtraBounces = GraphBuilder.CreateSRV(
					FRDGBufferSRVDesc{ DeferredSurfaceExtraBounces,
					DeferredSurfaceBouncePass * MaxDeferredSurfaceBouncesPerCandidate * static_cast<uint32>(sizeof(FHVPT_Bounce)) * HVPT::GetMaxBounces(),
					MaxDeferredSurfaceBouncesPerCandidate * HVPT::GetMaxBounces() });

				PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
				PassParameters->RWExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesA);

				FReSTIRCandidateEvaluateSurfaceBouncesRGS::FPermutationDomain Permutation;
				Permutation.Set<FReSTIRCandidateEvaluateSurfaceBouncesRGS::FUseSER>(HVPT::ShouldUseSER());
				Permutation.Set<FReSTIRCandidateEvaluateSurfaceBouncesRGS::FDebugOutputEnabled>(State.DebugFlags& HVPT_DEBUG_FLAG_ENABLE);
				AddRaytracingPass<FReSTIRCandidateEvaluateSurfaceBouncesRGS>(
					GraphBuilder,
					RDG_EVENT_NAME("ReSTIRCandidateEvaluateSurfaceBounces(n=%d)", DeferredSurfaceBouncePass),
					ViewInfo,
					State,
					PassParameters,
					Permutation,
					DispatchDeferredSurfaceBounceIndirectArguments
				);
			}
		}
		if (CVarHVPTReSTIRDeferEvaluateCandidateF.GetValueOnRenderThread())
		{
			FReSTIRCandidateEvaluateFRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRCandidateEvaluateFRGS::FParameters>();
			PopulateCommonParameters(&PassParameters->Common);

			PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
			if (HVPT::GetMaxBounces() > 1)
				PassParameters->ExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesA);

			AddRaytracingPass<FReSTIRCandidateEvaluateFRGS>(
				GraphBuilder,
				RDG_EVENT_NAME("ReSTIRCandidateEvaluateF"),
				ViewInfo,
				State,
				PassParameters,
				DispatchRaysIndirectArgumentBuffer
			);
		}
	}

	// Temporal Reuse (only when history is valid)
	if (bValidHistory && HVPT::GetTemporalReuseEnabled())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Temporal Reuse)");

		FReSTIRTemporalReuseRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRTemporalReuseRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common);

		PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);
		PassParameters->TemporalFeatureTexture = GraphBuilder.CreateSRV(bHasTemporalFeatureTexture ? State.TemporalFeatureTexture : GSystemTextures.GetBlackDummy(GraphBuilder));

		PassParameters->PreviousReservoirs = GraphBuilder.CreateSRV(ReservoirsB);
		if (HVPT::GetMaxBounces() > 1)
			PassParameters->PreviousExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesB);

		PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
		if (HVPT::GetMaxBounces() > 1)
			PassParameters->RWCurrentExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesA);

		PassParameters->TemporalHistoryThreshold = HVPT::GetTemporalReuseHistoryThreshold();
		PassParameters->bEnableTemporalReprojection = HVPT::GetTemporalReprojectionEnabled() && bHasTemporalFeatureTexture;
		PassParameters->bTalbotMIS = HVPT::GetTemporalReuseMISEnabled();

		AddRaytracingPass<FReSTIRTemporalReuseRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRTemporalReuse"),
			ViewInfo,
			State,
			PassParameters,
			DispatchRaysIndirectArgumentBuffer
		);
	}

	// Spatial Reuse
	if (HVPT::GetSpatialReuseEnabled())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Spatial Reuse)");

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ReservoirsB), 0);
		if (HVPT::GetMaxBounces() > 1)
			AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(ExtraBouncesB), 0.0f);

		if (!CVarHVPTReSTIRMultiPassSpatialReuse.GetValueOnRenderThread())
		{
			FReSTIRSpatialReuseRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRSpatialReuseRGS::FParameters>();
			PopulateCommonParameters(&PassParameters->Common);

			PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

			PassParameters->InReservoirs = GraphBuilder.CreateSRV(ReservoirsA);
			if (HVPT::GetMaxBounces() > 1)
				PassParameters->InExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesA);

			PassParameters->RWOutReservoirs = GraphBuilder.CreateUAV(ReservoirsB);
			if (HVPT::GetMaxBounces() > 1)
				PassParameters->RWOutExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesB);

			PassParameters->NumSpatialSamples = FMath::Clamp(HVPT::GetNumSpatialReuseSamples(), 0, kReSTIRMaxSpatialSamples);
			PassParameters->SpatialReuseRadius = HVPT::GetSpatialReuseRadius();
			PassParameters->bTalbotMIS = HVPT::GetSpatialReuseMISEnabled();

			AddRaytracingPass<FReSTIRSpatialReuseRGS>(
				GraphBuilder,
				RDG_EVENT_NAME("ReSTIRSpatialReuse"),
				ViewInfo,
				State,
				PassParameters,
				DispatchRaysIndirectArgumentBuffer
			);
		}
		else
		{
			// Spatial reuse in multiple passes to reduce thread divergence and share work among threads
			// To make the memory requirements for transient buffers feasible, the number of spatial samples and spatial reuse radius have to be limited
			float SpatialReuseRadius = FMath::Clamp(HVPT::GetSpatialReuseRadius(), 1.0f, 10.0f);
			int32 SpatialReuseRadiusI = FMath::CeilToInt(SpatialReuseRadius);
			uint32 NumSpatialSamples = FMath::Clamp(HVPT::GetNumSpatialReuseSamples(), 1, 8);

			// Calculate number of tiles / size of tiles based on view size
			const auto& ViewExtent = ViewInfo.ViewRect.Size();
			// The view rect will be processed in 512x512 tiles. These tiles will be surrounded by a (up to) 10px buffer zone around each edge.
			// Threads will be dispatched for the 512x512 tile, but they may require evaluating domains outside of this rect, so the transient buffers must be big enough for this
			constexpr int32 TileW = 512;
			constexpr int32 ReservoirsPerTile = TileW * TileW;

			const int32 TileW_Buffered = TileW + 2 * SpatialReuseRadiusI;
			const int32 SqrtDomainsPerReservoir = (2 * SpatialReuseRadiusI + 1);
			const int32 DomainsPerReservoir = SqrtDomainsPerReservoir * SqrtDomainsPerReservoir;
			const int32 MaxEvaluationsPerTile = (TileW_Buffered * TileW_Buffered) * DomainsPerReservoir;

			const int32 NumTilesX = FMath::DivideAndRoundUp(ViewExtent.X, TileW);
			const int32 NumTilesY = FMath::DivideAndRoundUp(ViewExtent.Y, TileW);

			// Allocate transient buffers required for intermediate results
			FRDGBufferRef NeighbourIndicesBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint16), ReservoirsPerTile * NumSpatialSamples), TEXT("NeighbourIndices"));
			FRDGBufferRef EvaluationResultsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxEvaluationsPerTile), TEXT("EvaluationResults"));
			FRDGBufferRef EvaluationIndirectionBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxEvaluationsPerTile), TEXT("EvaluationIndirection"));
			FRDGBufferRef IndirectionBufferAllocator = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("IndirectionAllocator"));

			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ViewInfo.FeatureLevel);

			// To make memory usage requirements reasonable (and to allow indices into buffers to fit in few enough bits),
			// spatial reuse is performed in tiles.
			for (int32 Y = 0; Y < NumTilesY; Y++)
			for (int32 X = 0; X < NumTilesX; X++)
			{
				RDG_EVENT_SCOPE(GraphBuilder, "Tiled Spatial Reuse (X=%d,Y=%d)", X, Y);

				// The actual tile width may be less than TileW (if the tile is off of the screen)
				// Calculate the actual tile size
				FIntPoint TileStart{ X * TileW, Y * TileW };
				FIntPoint TileEnd{ FMath::Min((X + 1) * TileW, ViewExtent.X), FMath::Min((Y + 1) * TileW, ViewExtent.Y) };
				FIntPoint TileSize = TileEnd - TileStart;

				auto PopulateSpatialReuseCommonParameters = [&](FReSTIRMultiPassSpatialReuseCommonParameters& Parameters)
				{
					Parameters.NumSpatialSamples = NumSpatialSamples;
					Parameters.SpatialReuseRadius = SpatialReuseRadius;

					Parameters.TileStart = TileStart;
					Parameters.TileSize = TileSize;
					Parameters.BufferedTileSize = TileSize + FIntPoint{ 2 * SpatialReuseRadiusI, 2 * SpatialReuseRadiusI };

					Parameters.TileBufferZoneWidth = SpatialReuseRadiusI;

					Parameters.DomainsPerReservoir = DomainsPerReservoir;
					Parameters.SqrtDomainsPerReservoir = SqrtDomainsPerReservoir;
				};

				// Clear resources that need cleared (allocator + results buffer)
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(EvaluationResultsBuffer), HVPT_SPATIAL_REUSE_UNALLOCATED);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(IndirectionBufferAllocator), 0);

				// Step 1: Choose neighbours for each reservoir.
				// One item will be added to EvaluationIndirectionBuffer for every path evaluation that is required to calculate MIS weights.
				{
					FReSTIRSpatialReuse_ChooseNeighboursCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRSpatialReuse_ChooseNeighboursCS::FParameters>();
					PassParameters->View = ViewInfo.ViewUniformBuffer;
					PopulateSpatialReuseCommonParameters(PassParameters->SpatialReuseCommon);

					uint32 FrameIndex = ViewInfo.ViewState ? ViewInfo.ViewState->FrameIndex : 0;
					PassParameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : MaxNumPasses * FrameIndex + TemporalSeedOffset++;

					PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

					PassParameters->InReservoirs = GraphBuilder.CreateSRV(ReservoirsA);

					PassParameters->RWNeighbourIndices = GraphBuilder.CreateUAV(NeighbourIndicesBuffer);
					PassParameters->RWEvaluationResults = GraphBuilder.CreateUAV(EvaluationResultsBuffer);
					PassParameters->RWEvaluationIndirectionBuffer = GraphBuilder.CreateUAV(EvaluationIndirectionBuffer);
					PassParameters->RWIndirectionAllocator = GraphBuilder.CreateUAV(IndirectionBufferAllocator);

					if (State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE)
					{
						PassParameters->RWDebugTexture = GraphBuilder.CreateUAV(State.DebugTexture);
						PassParameters->DebugFlags = State.DebugFlags;
					}

					FReSTIRSpatialReuse_ChooseNeighboursCS::FPermutationDomain Permutation;
					Permutation.Set<FReSTIRSpatialReuse_ChooseNeighboursCS::FDebugOutputEnabled>(State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE);
					TShaderMapRef<FReSTIRSpatialReuse_ChooseNeighboursCS> ComputeShader(ShaderMap, Permutation);
					FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(static_cast<FIntPoint>(TileSize), FReSTIRSpatialReuse_ChooseNeighboursCS::GetThreadGroupSize2D());
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("ChooseNeighbours"),
						ERDGPassFlags::Compute,
						ComputeShader,
						PassParameters,
						GroupCount
					);
				}

				// Step 2: Sorting and compaction on EvaluationIndirectionBuffer which enables nearby threads to evaluate paths of similar lengths
				{
					
				}
				
				// Step 3: Evaluating paths. Each path that is needed to be evaluated in the indirection buffer will be executed and the result stored in EvaluationResultsBuffer
				{
					// Setup indirect arguments
					FRDGBufferRef IndirectArguments = FComputeShaderUtils::AddIndirectArgsSetupCsPass1D(
						GraphBuilder, ViewInfo.FeatureLevel, IndirectionBufferAllocator, TEXT("HVPT.ReSTIR.DeferredSurfaceBouncesIndirectArguments"), 1);

					FReSTIRSpatialReuse_EvaluateRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRSpatialReuse_EvaluateRGS::FParameters>();
					PopulateCommonParameters(&PassParameters->Common);
					PassParameters->Common.IndirectArgs = IndirectArguments;

					PopulateSpatialReuseCommonParameters(PassParameters->SpatialReuseCommon);

					PassParameters->InReservoirs = GraphBuilder.CreateSRV(ReservoirsA);
					if (HVPT::GetMaxBounces() > 1)
						PassParameters->InExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesA);

					PassParameters->EvaluationIndirectionBuffer = GraphBuilder.CreateSRV(EvaluationIndirectionBuffer);
					PassParameters->RWEvaluationResults = GraphBuilder.CreateUAV(EvaluationResultsBuffer);

					AddRaytracingPass<FReSTIRSpatialReuse_EvaluateRGS>(
						GraphBuilder,
						RDG_EVENT_NAME("EvaluatePHat"),
						ViewInfo,
						State,
						PassParameters,
						IndirectArguments
					);
				}
				
				// Step 4: Spatial Reuse. Now that all the paths have been evaluated that are needed, we can gather the results and use them to select a sample for each pixel's reservoir
				{
					FReSTIRSpatialReuse_GatherAndReuseCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRSpatialReuse_GatherAndReuseCS::FParameters>();
					PassParameters->View = ViewInfo.ViewUniformBuffer;

					PopulateSpatialReuseCommonParameters(PassParameters->SpatialReuseCommon);

					uint32 FrameIndex = ViewInfo.ViewState ? ViewInfo.ViewState->FrameIndex : 0;
					PassParameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : MaxNumPasses * FrameIndex + TemporalSeedOffset++;

					PassParameters->NumBounces = FMath::Clamp(HVPT::GetMaxBounces(), 1, kReSTIRMaxBounces);

					PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

					PassParameters->InReservoirs = GraphBuilder.CreateSRV(ReservoirsA);
					if (HVPT::GetMaxBounces() > 1)
						PassParameters->InExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesA);

					PassParameters->NeighbourIndices = GraphBuilder.CreateSRV(NeighbourIndicesBuffer);
					PassParameters->EvaluationResults = GraphBuilder.CreateSRV(EvaluationResultsBuffer);

					PassParameters->RWOutReservoirs = GraphBuilder.CreateUAV(ReservoirsB);
					if (HVPT::GetMaxBounces() > 1)
						PassParameters->RWOutExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesB);

					if (State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE)
					{
						PassParameters->RWDebugTexture = GraphBuilder.CreateUAV(State.DebugTexture);
						PassParameters->DebugFlags = State.DebugFlags;
					}

					FReSTIRSpatialReuse_GatherAndReuseCS::FPermutationDomain Permutation;
					Permutation.Set<FReSTIRSpatialReuse_GatherAndReuseCS::FMultipleBounces>(HVPT::GetMaxBounces() > 1);
					Permutation.Set<FReSTIRSpatialReuse_GatherAndReuseCS::FDebugOutputEnabled>(State.DebugFlags& HVPT_DEBUG_FLAG_ENABLE);
					TShaderMapRef<FReSTIRSpatialReuse_GatherAndReuseCS> ComputeShader(ShaderMap, Permutation);
					FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(static_cast<FIntPoint>(TileSize), FReSTIRSpatialReuse_GatherAndReuseCS::GetThreadGroupSize2D());
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("GatherAndReuse"),
						ERDGPassFlags::Compute,
						ComputeShader,
						PassParameters,
						GroupCount
					);
				}
			}
		}
	}
	else
	{
		// Copying when spatial reuse is disabled is not required, but makes debugging much easier as aliasing resources
		// (when ReservoirsA and ReservoirsB point to the same resource) gets confusing quick.
		// Plus, the cost of copying these two buffers is very tiny compared to the cost of volumetric path tracing
		// In practice, spatial reuse should generally be enabled anyway.
		uint64 NumBytes = static_cast<uint64>(ReservoirsB->Desc.BytesPerElement * ReservoirsB->Desc.NumElements);
		AddCopyBufferPass(GraphBuilder, ReservoirsB, 0, ReservoirsA, 0, NumBytes);

		NumBytes = static_cast<uint64>(ExtraBouncesB->Desc.BytesPerElement * ExtraBouncesB->Desc.NumElements);
		AddCopyBufferPass(GraphBuilder, ExtraBouncesB, 0, ExtraBouncesA, 0, NumBytes);
	}

	// Final Shading
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Final Shading)");

		FReSTIRFinalShadingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRFinalShadingRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common);

		PassParameters->CurrentReservoirs = GraphBuilder.CreateSRV(ReservoirsB);
		if (HVPT::GetMaxBounces() > 1)
			PassParameters->ExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesB);

		PassParameters->RWRadianceTexture = GraphBuilder.CreateUAV(State.RadianceTexture);

		AddRaytracingPass<FReSTIRFinalShadingRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRFinalShading"),
			ViewInfo, 
			State,
			PassParameters,
			DispatchRaysIndirectArgumentBuffer
		);
	}

	// Run debug pass
	if (State.DebugFlags & HVPT_DEBUG_FLAG_ENABLE)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Debug Visualization)");

		FReSTIRDebugVisualizationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRDebugVisualizationCS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common);

		PassParameters->Reservoirs = GraphBuilder.CreateSRV(ReservoirsB);
		if (HVPT::GetMaxBounces() > 1)
			PassParameters->ExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesB);

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ViewInfo.FeatureLevel);
		TShaderMapRef<FReSTIRDebugVisualizationCS> ComputeShader(ShaderMap);

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(ViewInfo.ViewRect.Size(), FReSTIRDebugVisualizationCS::GetThreadGroupSize2D());

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRDebugVisualization"),
			ERDGPassFlags::Compute,
			ComputeShader,
			PassParameters,
			GroupCount
		);
	}

	// Extract history to be used in next frame
	GraphBuilder.QueueBufferExtraction(ReservoirsB, &State.ReSTIRReservoirCache);
	if (HasBeenProduced(ExtraBouncesB))
		GraphBuilder.QueueBufferExtraction(ExtraBouncesB, &State.ReSTIRExtraBounceCache);
}

#endif
