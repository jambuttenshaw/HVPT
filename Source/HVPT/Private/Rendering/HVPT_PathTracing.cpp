#include <SceneRendering.h>

#include "HVPTViewExtension.h"
#include "HVPT.h"

#include "GlobalShader.h"
#include "HVPTViewState.h"
#include "RayTracingPayloadType.h"
#include "RayTracing/RayTracingScene.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SceneTextureParameters.h"
#include "PathTracing.h"
#include "ScenePrivate.h"
#include "RendererUtils.h"
#include "RayTracingShaderBindingLayout.h"
#include "SceneCore.h"

#include "VoxelGrid.h"

#include "Helpers.h"
#include "PathTracingLightGrid.h"


#if RHI_RAYTRACING

class FHVPT_RenderWithPathTracingRGS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RenderWithPathTracingRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FHVPT_RenderWithPathTracingRGS, FGlobalShader);

	class FSurfaceContributions : SHADER_PERMUTATION_BOOL("USE_SURFACE_CONTRIBUTIONS");
	class FApplyVolumetricFog : SHADER_PERMUTATION_BOOL("APPLY_VOLUMETRIC_FOG");
	using FPermutationDomain = TShaderPermutationDomain<FSurfaceContributions, FApplyVolumetricFog>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepthTexture_Copy)

		SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)

		SHADER_PARAMETER_STRUCT_INCLUDE(FHVPT_PathTracingFogParameters, FogParameters)

		// Taken from path tracer implementation, to be able to use path tracer light sampling logic
		// Scene lights
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER(uint32, SceneVisibleLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingSkylight, SkylightParameters)

		// Heterogeneous volumes adaptive voxel grid
		// All volumes in the scene are rasterized into these 2 grids
		// therefore we effectively only have to trace against a single object
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTOrthoGridUniformBufferParameters, HVPT_OrthoGrid)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTFrustumGridUniformBufferParameters, HVPT_FrustumGrid)

		// Random init parameters
		SHADER_PARAMETER(uint32, TemporalSeed)

		// Tracing Parameters
		SHADER_PARAMETER(uint32, MaxBounces)
		SHADER_PARAMETER(uint32, MaxRaymarchSteps)

		SHADER_PARAMETER(float, OpaqueThreshold)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadianceTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform)
			&& DoesPlatformSupportHeterogeneousVolumes(Parameters.Platform);
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

IMPLEMENT_GLOBAL_SHADER(FHVPT_RenderWithPathTracingRGS, "/Plugin/HVPT/Private/PathTracing.usf", "HVPT_RenderWithPathTracingRGS", SF_RayGen);


void HVPT::PrepareRaytracingShaders(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	auto ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

	FHVPT_RenderWithPathTracingRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHVPT_RenderWithPathTracingRGS::FSurfaceContributions>(HVPT::UseSurfaceContributions());
	PermutationVector.Set<FHVPT_RenderWithPathTracingRGS::FApplyVolumetricFog>(HVPT::GetFogCompositingMode() == EFogCompositionMode::PostAndPathTracing);
	auto RayGenShader = ShaderMap->GetShader<FHVPT_RenderWithPathTracingRGS>(PermutationVector);
	OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
}

void HVPT::RenderWithPathTracing(
	FRDGBuilder& GraphBuilder, const FScene& Scene, const FViewInfo& ViewInfo, const FSceneTextures& SceneTextures, FHVPTViewState& State
)
{
	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: Path Tracing");

	FHVPT_RenderWithPathTracingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RenderWithPathTracingRGS::FParameters>();

	PassParameters->View = ViewInfo.ViewUniformBuffer;
	PassParameters->SceneTextures = HVPT::Private::GetSceneTextureParameters(GraphBuilder, SceneTextures);
	PassParameters->SceneDepthTexture_Copy = GraphBuilder.CreateSRV(State.DepthBufferCopy);
	PassParameters->TLAS = Scene.RayTracingScene.GetLayerView(ERayTracingSceneLayer::Base);

	PassParameters->FogParameters = HVPT::Private::PrepareFogParameters(ViewInfo, Scene.ExponentialFogs[0]);

	::HVPT::Private::SetPathTracingLightParameters(
		GraphBuilder,
		ViewInfo,
		false, // bUseAtmosphere
		&PassParameters->SkylightParameters,
		&PassParameters->LightGridParameters,
		&PassParameters->SceneVisibleLightCount,
		&PassParameters->SceneLightCount,
		&PassParameters->SceneLights
	);

	PassParameters->HVPT_OrthoGrid = State.OrthoGridUniformBuffer;
	PassParameters->HVPT_FrustumGrid = State.FrustumGridUniformBuffer;

	uint32 FrameIndex = ViewInfo.ViewState ? ViewInfo.ViewState->FrameIndex : 0;
	PassParameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : FrameIndex;

	// Note: Adding 1 to max bounces to make consistent with ReSTIR pipeline
	// ReSTIR counts bounces as scattering events - this pipeline counts bounces as number of path vertices excluding camera vertex
	PassParameters->MaxBounces = HVPT::GetMaxBounces() + 1;
	PassParameters->MaxRaymarchSteps = HVPT::GetMaxRaymarchSteps();

	PassParameters->RWRadianceTexture = GraphBuilder.CreateUAV(State.RadianceTexture);

	FIntPoint DispatchSize = ViewInfo.ViewRect.Size();

	FHVPT_RenderWithPathTracingRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHVPT_RenderWithPathTracingRGS::FSurfaceContributions>(HVPT::UseSurfaceContributions());
	PermutationVector.Set<FHVPT_RenderWithPathTracingRGS::FApplyVolumetricFog>(HVPT::GetFogCompositingMode() == EFogCompositionMode::PostAndPathTracing);
	TShaderMapRef<FHVPT_RenderWithPathTracingRGS> RayGenShader(ViewInfo.ShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HVPT_RenderWithPathTracing"),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, SceneUniformBuffer = ViewInfo.GetSceneUniforms().GetBufferRHI(GraphBuilder), RayGenShader, DispatchSize, &ViewInfo]
		(FRHICommandList& RHICmdList)
		{
			FRHIBatchedShaderParameters& GlobalResources = RHICmdList.GetScratchShaderParameters();
			SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);
			TOptional<FScopedUniformBufferStaticBindings> StaticUniformBufferScope =
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				HVPT::Private::BindStaticUniformBufferBindings(ViewInfo, SceneUniformBuffer, Nanite::GetPublicGlobalRayTracingUniformBuffer()->GetRHI(), RHICmdList);
#else
				RayTracing::BindStaticUniformBufferBindings(ViewInfo, SceneUniformBuffer, RHICmdList);
#endif

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
		});
}

#endif
