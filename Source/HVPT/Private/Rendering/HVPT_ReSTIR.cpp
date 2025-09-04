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
END_SHADER_PARAMETER_STRUCT()


class FReSTIRBaseRGS : public FGlobalShader
{
public:

	class FMultipleBounces : SHADER_PERMUTATION_BOOL("MULTIPLE_BOUNCES");
	class FUseSurfaceContributions : SHADER_PERMUTATION_BOOL("USE_SURFACE_CONTRIBUTIONS");
	class FApplyVolumetricFog : SHADER_PERMUTATION_BOOL("APPLY_VOLUMETRIC_FOG");
	using FPermutationDomain = TShaderPermutationDomain<FMultipleBounces, FUseSurfaceContributions, FApplyVolumetricFog>;

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

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepthTexture_Copy)

		SHADER_PARAMETER(uint32, NumInitialCandidates)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWExtraBounces)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRCandidateGenerationRGS, "/Plugin/HVPT/Private/ReSTIR/CandidateGeneration.usf", "ReSTIRCandidateGenerationRGS", SF_RayGen)

class FReSTIRTemporalReuseRGS : public FReSTIRBaseRGS
{
public:
	DECLARE_GLOBAL_SHADER(FReSTIRTemporalReuseRGS);
	SHADER_USE_ROOT_PARAMETER_STRUCT(FReSTIRTemporalReuseRGS, FReSTIRBaseRGS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FReSTIRCommonParameters, Common)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, PreviousReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, PreviousExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWCurrentReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWCurrentExtraBounces)
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

		SHADER_PARAMETER(uint32, NumSpatialSamples)
		SHADER_PARAMETER(float, SpatialReuseRadius)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Reservoir>, InReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_Bounce>, InExtraBounces)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Reservoir>, RWOutReservoirs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FHVPT_Bounce>, RWOutExtraBounces)

	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FReSTIRSpatialReuseRGS, "/Plugin/HVPT/Private/ReSTIR/SpatialReuse.usf", "ReSTIRSpatialReuseRGS", SF_RayGen)

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


void HVPT::PrepareRaytracingShadersReSTIR(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	auto ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

	auto AddShader = [&]<typename T>()
	{
		typename T::FPermutationDomain Permutation;
		Permutation.Set<typename T::FMultipleBounces>(HVPT::GetMaxBounces() > 1);
		Permutation.Set<typename T::FUseSurfaceContributions>(HVPT::UseSurfaceContributions());
		Permutation.Set<typename T::FApplyVolumetricFog>(HVPT::GetFogCompositingMode() == HVPT::EFogCompositionMode::PostAndPathTracing);
		OutRayGenShaders.Add(ShaderMap->GetShader<T>(Permutation).GetRayTracingShader());
	};

	// AddShader<T>() does not compile
	AddShader.template operator()<FReSTIRCandidateGenerationRGS>();
	AddShader.template operator()<FReSTIRTemporalReuseRGS>();
	AddShader.template operator()<FReSTIRSpatialReuseRGS>();
	AddShader.template operator()<FReSTIRFinalShadingRGS>();
}


template <typename Shader>
void AddRaytracingPass(
	FRDGBuilder& GraphBuilder,
	FRDGEventName&& EventName,
	const FViewInfo& View,
	typename Shader::FParameters* PassParameters
)
{
	typename Shader::FPermutationDomain Permutation;
	AddRaytracingPass<Shader>(GraphBuilder, std::move(EventName), View, PassParameters, Permutation);
}

template <typename Shader>
void AddRaytracingPass(
	FRDGBuilder& GraphBuilder,
	FRDGEventName&& EventName,
	const FViewInfo& ViewInfo,
	typename Shader::FParameters* PassParameters,
	typename Shader::FPermutationDomain& Permutation
)
{
	auto DispatchSize = ViewInfo.ViewRect.Size();
	FRHIUniformBuffer* SceneUniformBuffer = ViewInfo.GetSceneUniforms().GetBufferRHI(GraphBuilder);

	// Set common permutation vector dimensions here
	Permutation.Set<typename Shader::FMultipleBounces>(HVPT::GetMaxBounces() > 1);
	Permutation.Set<typename Shader::FUseSurfaceContributions>(HVPT::UseSurfaceContributions());
	Permutation.Set<typename Shader::FApplyVolumetricFog>(HVPT::GetFogCompositingMode() == HVPT::EFogCompositionMode::PostAndPathTracing);
	TShaderMapRef<Shader> RayGenShader(ViewInfo.ShaderMap, Permutation);

	GraphBuilder.AddPass(
		std::move(EventName),
		PassParameters,
		ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		[PassParameters, SceneUniformBuffer, RayGenShader, DispatchSize, &ViewInfo](FRHICommandList& RHICmdList)
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


void HVPT::RenderWithReSTIRPathTracing(
	FRDGBuilder& GraphBuilder, const FScene& Scene, const FViewInfo& ViewInfo, const FSceneTextures& SceneTextures, FHVPTViewState& State
)
{
	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR");

	// Create resources
	const auto& Extent = ViewInfo.ViewRect.Size();

	auto ReservoirDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_Reservoir), Extent.X * Extent.Y);
	// When max bounces is 1 then extra bounce buffer is not needed - just creates buffer with 1 element
	auto ExtraBounceDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FHVPT_Bounce),
		FMath::Max(Extent.X * Extent.Y * (HVPT::GetMaxBounces() - 1), 1));

	FRDGBufferRef ReservoirsA = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("HeterogeneousVolumes.ReservoirsA"));
	FRDGBufferRef ExtraBouncesA = GraphBuilder.CreateBuffer(ExtraBounceDesc, TEXT("HeterogeneousVolumes.ExtraBouncesA"));

	FRDGBufferRef ReservoirsB;
	FRDGBufferRef ExtraBouncesB;
	bool bValidHistory = (State.ReSTIRReservoirCache.IsValid() && State.ReSTIRReservoirCache->Desc == ReservoirDesc)
					  && (State.ReSTIRExtraBounceCache.IsValid() && State.ReSTIRExtraBounceCache->Desc == ExtraBounceDesc);
	if (bValidHistory)
	{
		ReservoirsB = GraphBuilder.RegisterExternalBuffer(State.ReSTIRReservoirCache);
		ExtraBouncesB = GraphBuilder.RegisterExternalBuffer(State.ReSTIRExtraBounceCache);
	}
	else
	{
		// No valid history - create empty buffers
		ReservoirsB = GraphBuilder.CreateBuffer(ReservoirDesc, TEXT("HeterogeneousVolumes.ReservoirsB"));
		ExtraBouncesB = GraphBuilder.CreateBuffer(ExtraBounceDesc, TEXT("HeterogeneousVolumes.ExtraBouncesB"));
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

	// Unfortunately due to what appears to appear an Unreal bug (crashes during D3D12 pipeline state creation with E_INVALIDARG)
	// using a uniform buffer to hold common parameters does not work
	auto PopulateCommonParameters = [&](FReSTIRCommonParameters* Parameters, uint32 TemporalSeedOffset)
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
			Parameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : 4 * FrameIndex + TemporalSeedOffset;

			Parameters->NumBounces = FMath::Clamp(HVPT::GetMaxBounces(), 1, kReSTIRMaxBounces);
		};


	// Candidate Generation
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Candidate Generation)");

		FReSTIRCandidateGenerationRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRCandidateGenerationRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common, 0);

		PassParameters->SceneDepthTexture_Copy = GraphBuilder.CreateSRV(State.DepthBufferCopy);

		PassParameters->NumInitialCandidates = HVPT::GetNumInitialCandidates();

		PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
		PassParameters->RWExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesA);

		AddRaytracingPass<FReSTIRCandidateGenerationRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRCandidateGeneration"),
			ViewInfo, 
			PassParameters
		);
	}

	// Temporal Reuse (only when history is valid)
	if (bValidHistory && HVPT::GetTemporalReuseEnabled())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Temporal Reuse)");

		FReSTIRTemporalReuseRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRTemporalReuseRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common, 1);

		PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

		PassParameters->PreviousReservoirs = GraphBuilder.CreateSRV(ReservoirsB);
		PassParameters->PreviousExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesB);

		PassParameters->RWCurrentReservoirs = GraphBuilder.CreateUAV(ReservoirsA);
		PassParameters->RWCurrentExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesA);

		AddRaytracingPass<FReSTIRTemporalReuseRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRTemporalReuse"),
			ViewInfo,
			PassParameters
		);
	}

	// Spatial Reuse
	if (HVPT::GetSpatialReuseEnabled())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Spatial Reuse)");

		FReSTIRSpatialReuseRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRSpatialReuseRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common, 2);

		PassParameters->NumSpatialSamples = FMath::Clamp(HVPT::GetNumSpatialReuseSamples(), 0, kReSTIRMaxSpatialSamples);
		PassParameters->SpatialReuseRadius = HVPT::GetSpatialReuseRadius();

		PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

		PassParameters->InReservoirs = GraphBuilder.CreateSRV(ReservoirsA);
		PassParameters->InExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesA);

		PassParameters->RWOutReservoirs = GraphBuilder.CreateUAV(ReservoirsB);
		PassParameters->RWOutExtraBounces = GraphBuilder.CreateUAV(ExtraBouncesB);

		AddRaytracingPass<FReSTIRSpatialReuseRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRSpatialReuse"),
			ViewInfo, 
			PassParameters
		);
	}
	else
	{
		// Swap buffers since spatial reuse is not active
		ReservoirsB = ReservoirsA;
		ExtraBouncesB = ExtraBouncesA;
	}

	// Final Shading
	{
		RDG_EVENT_SCOPE(GraphBuilder, "HVPT: ReSTIR (Final Shading)");

		FReSTIRFinalShadingRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReSTIRFinalShadingRGS::FParameters>();
		PopulateCommonParameters(&PassParameters->Common, 3);

		PassParameters->CurrentReservoirs = GraphBuilder.CreateSRV(ReservoirsB);
		PassParameters->ExtraBounces = GraphBuilder.CreateSRV(ExtraBouncesB);

		PassParameters->RWRadianceTexture = GraphBuilder.CreateUAV(State.RadianceTexture);

		AddRaytracingPass<FReSTIRFinalShadingRGS>(
			GraphBuilder,
			RDG_EVENT_NAME("ReSTIRFinalShading"),
			ViewInfo, 
			PassParameters
		);
	}

	// Extract history to be used in next frame
	GraphBuilder.QueueBufferExtraction(ReservoirsB, &State.ReSTIRReservoirCache);
	GraphBuilder.QueueBufferExtraction(ExtraBouncesB, &State.ReSTIRExtraBounceCache);
}

#endif
