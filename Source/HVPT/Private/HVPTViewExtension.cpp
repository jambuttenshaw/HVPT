#include "HVPTViewExtension.h"

#include "HVPT.h"
#include "HVPTViewState.h"

#include "Rendering/VoxelGrid.h"

#include "DeferredShadingRenderer.h"
#include "HVPTDefinitions.h"

#define LOCTEXT_NAMESPACE "HVPTModule"

DECLARE_GPU_STAT_NAMED(HVPTStat, TEXT("HVPT"));


namespace HVPT {

TCustomShowFlag<> ShowHVPTDebug(TEXT("HVPTDebug"), false, SFG_Developer, LOCTEXT("ShowFlagDisplayName", "HVPT Debug"));

static bool IsDebugShowFlagEnabled(const FSceneView& InView)
{
	int32 ShowFlagIndex = FEngineShowFlags::FindIndexByName(TEXT("HVPTDebug"));
	return ShowFlagIndex >= 0 && InView.Family->EngineShowFlags.GetSingleFlag(static_cast<uint32>(ShowFlagIndex));
}

}


FHVPTViewExtension::FHVPTViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	// Subscribe to GI plugin events - this allows us to inject our raytracing raygen shaders
#if RHI_RAYTRACING
	FGlobalIlluminationPluginDelegates::FPrepareRayTracing& PrepareRTDelegate = FGlobalIlluminationPluginDelegates::PrepareRayTracing();
	PrepareRTDelegateHandle = PrepareRTDelegate.AddLambda([this](const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
		{
			auto ViewState = this->GetOrCreateViewStateForView(View);
			if (!ViewState)
				return;

			if (HVPT::UseHVPT_RenderThread())
			{
				if (HVPT::UseReSTIR())
				{
					HVPT::PrepareRaytracingShadersReSTIR(View, *ViewState, OutRayGenShaders);
				}
				else
				{
					HVPT::PrepareRaytracingShaders(View, *ViewState, OutRayGenShaders);
				}
			}
		});

	FGlobalIlluminationPluginDelegates::FAnyRayTracingPassEnabled& AnyRTPassEnabledDelegate = FGlobalIlluminationPluginDelegates::AnyRayTracingPassEnabled();
	AnyRTPassEnabledDelegateHandle = AnyRTPassEnabledDelegate.AddStatic([](bool& anyEnabled)
		{
			anyEnabled |= HVPT::UseHVPT_RenderThread();
		});
#endif
}

FHVPTViewExtension::~FHVPTViewExtension()
{
#if RHI_RAYTRACING
	// Unsubscribe delegates
	FGlobalIlluminationPluginDelegates::FPrepareRayTracing& PrepareRTDelegate = FGlobalIlluminationPluginDelegates::PrepareRayTracing();
	PrepareRTDelegate.Remove(PrepareRTDelegateHandle);

	FGlobalIlluminationPluginDelegates::FAnyRayTracingPassEnabled& AnyRTPassEnabledDelegate = FGlobalIlluminationPluginDelegates::AnyRayTracingPassEnabled();
	AnyRTPassEnabledDelegate.Remove(AnyRTPassEnabledDelegateHandle);
#endif
}

void FHVPTViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
#if RHI_RAYTRACING
	if (!InView.bIsViewInfo || InView.Family->EngineShowFlags.PathTracing)
		return;
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	if (ViewInfo.bIsReflectionCapture)
		return;

	auto ViewState = GetOrCreateViewStateForView(ViewInfo);
	if (!ViewState)
		return;

	if (HVPT::IsDebugShowFlagEnabled(InView))
	{
		ViewState->DebugTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(ViewInfo.ViewRect.Size(), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("HVPT.DebugTexture"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ViewState->DebugTexture), 0.0f);

		// Set debug view mode
		ViewState->DebugFlags = HVPT::GetDebugViewMode();
		// Populate flags
		ViewState->DebugFlags |= HVPT_DEBUG_FLAG_ENABLE;
	}
	else
	{
		ViewState->DebugFlags = 0;
	}
#endif

}

void FHVPTViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextureParameters
)
{
#if RHI_RAYTRACING
	if (!InView.bIsViewInfo || InView.Family->EngineShowFlags.PathTracing)
		return;
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	if (!HVPT::ShouldRenderHVPTForView(ViewInfo))
		return;
	auto SceneTextures = ViewInfo.GetSceneTextures();

	RDG_EVENT_SCOPE(GraphBuilder, "HVPT.ClearVelocity");

	// Clear velocity buffer if required
	// Velocity must have been used AT LEAST once BEFORE post-processing pass,
	// otherwise UE will not pass a velocity texture to the post-processing stack.

	// This is tricky for us as we want to write to velocity in the pre-pass.
	// By clearing velocity after the base pass (if the base pass has not written velocity)
	// UE will definitely give us a velocity texture in the post-process pass.

	if (HVPT::ShouldWriteVelocity() && !HasBeenProduced(SceneTextures.Velocity))
	{
		AddClearRenderTargetPass(GraphBuilder, SceneTextures.Velocity);
	}
#endif
}


void FHVPTViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
#if RHI_RAYTRACING
	if (!InView.bIsViewInfo || InView.Family->EngineShowFlags.PathTracing)
		return;
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	if (!HVPT::ShouldRenderHVPTForView(ViewInfo))
		return;

	auto ViewState = GetOrCreateViewStateForView(ViewInfo);
	auto Scene = (ViewInfo.Family->Scene) ? ViewInfo.Family->Scene->GetRenderScene() : nullptr;
	if (!(ViewState && Scene))
		return;
	const FSceneTextures& SceneTextures = ViewInfo.GetSceneTextures();

	RDG_EVENT_SCOPE_STAT(GraphBuilder, HVPTStat, "HVPT");
	RDG_GPU_STAT_SCOPE(GraphBuilder, HVPTStat);
	SCOPED_NAMED_EVENT(HVPT, FColor::Purple);

	// Register history into RDG
	if (ViewState->TemporalAccumulationRT)
	{
		ViewState->TemporalAccumulationTexture = GraphBuilder.RegisterExternalTexture(ViewState->TemporalAccumulationRT);
	}

	// Build voxel grid if required
	{
		TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> OrthoGridUniformBuffer = HVPT::GetOrthoVoxelGridUniformBuffer(GraphBuilder, *ViewState);
		TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> FrustumGridUniformBuffer = HVPT::GetFrustumVoxelGridUniformBuffer(GraphBuilder, *ViewState);

		bool bIsGridEmpty = !(OrthoGridUniformBuffer && FrustumGridUniformBuffer);
		if (!bIsGridEmpty)
		{
			auto& Parameters = OrthoGridUniformBuffer->GetParameters();
			bIsGridEmpty = Parameters->TopLevelGridWorldBoundsMin == Parameters->TopLevelGridWorldBoundsMax;
		}

		if (HVPT::GetRebuildEveryFrame() || bIsGridEmpty)
		{
			FHVPT_VoxelGridBuildOptions BuildOptions;
			BuildOptions.bJitter = HVPT::GetShouldJitter() && !HVPT::GetFreezeTemporalSeed();

			HVPT::BuildOrthoVoxelGrid(GraphBuilder, Scene, ViewInfo, BuildOptions, OrthoGridUniformBuffer);
			HVPT::BuildFrustumVoxelGrid(GraphBuilder, Scene, ViewInfo, BuildOptions, FrustumGridUniformBuffer);

			ViewState->OrthoGridUniformBuffer = OrthoGridUniformBuffer;
			ViewState->FrustumGridUniformBuffer = FrustumGridUniformBuffer;
		}
	}

	// Create depth buffer copy
	ViewState->DepthBufferCopy = GraphBuilder.CreateTexture(SceneTextures.Depth.Target->Desc, TEXT("HVPT.DepthCopy"));
	AddCopyTexturePass(GraphBuilder, SceneTextures.Depth.Target, ViewState->DepthBufferCopy);

	// Create texture to hold transmittance
	ViewState->FeatureTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(ViewInfo.ViewRect.Size(), PF_G16R16F, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("HVPT.FeatureTexture"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ViewState->FeatureTexture), { 1.0f, 0.0f });

	// Run pre-pass to calculate transmittance and GBuffer properties
	HVPT::RenderPrePass(
		GraphBuilder,
		ViewInfo,
		SceneTextures,
		*ViewState
	);

	// Create radiance texture
	{
		FRDGTextureDesc Desc = ViewInfo.GetSceneTextures().Color.Target->Desc;
		Desc.Format = PF_FloatRGB;
		Desc.Flags &= ~(TexCreate_FastVRAM);
		ViewState->RadianceTexture = GraphBuilder.CreateTexture(Desc, TEXT("HVPT.Radiance"));
	}
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ViewState->RadianceTexture), FLinearColor::Black);

	// Perform radiance pass
	if (!HVPT::GetDisableRadiance())
	{
		if (HVPT::UseReSTIR())
		{
			HVPT::RenderWithReSTIRPathTracing(
				GraphBuilder,
				*Scene,
				ViewInfo,
				SceneTextures,
				*ViewState
			);
		}
		else
		{
			HVPT::RenderWithPathTracing(
				GraphBuilder,
				*Scene,
				ViewInfo,
				SceneTextures,
				*ViewState
			);
		}
	}

	if (HVPT::ShouldAccumulate())
	{
		FRDGTextureDesc RadianceTextureDesc = ViewState->RadianceTexture->Desc;
		if (ViewState->TemporalAccumulationTexture 
			&& RadianceTextureDesc.Extent != ViewState->TemporalAccumulationTexture->Desc.Extent)
		{
			ViewState->TemporalAccumulationTexture = nullptr;
		}

		if (!ViewState->TemporalAccumulationTexture)
		{
			// Create temporal accumulation texture
			ViewState->TemporalAccumulationTexture = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(RadianceTextureDesc.Extent, PF_A32B32G32R32F, FClearValueBinding::Black, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV),
				TEXT("HVPT.TemporalAccumulation"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ViewState->TemporalAccumulationTexture), 0.0f);
		}

		HVPT::Accumulate(
			GraphBuilder,
			ViewInfo,
			*ViewState
		);
		ViewState->AccumulatedSampleCount++;
	}
	else
	{
		if (ViewState->AccumulatedSampleCount > 0)
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ViewState->TemporalAccumulationTexture), 0.0f);
		}
		ViewState->AccumulatedSampleCount = 0;
	}

	// Finally perform composition with the scene
	HVPT::Composite(
		GraphBuilder,
		*Scene,
		ViewInfo,
		SceneTextures,
		*ViewState
	);

#endif
}

void FHVPTViewExtension::PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
#if RHI_RAYTRACING
	if (!InView.bIsViewInfo || InView.Family->EngineShowFlags.PathTracing)
		return;
	FViewInfo& ViewInfo = static_cast<FViewInfo&>(InView);
	if (!HVPT::ShouldRenderHVPTForView(ViewInfo))
		return;

	auto ViewState = GetOrCreateViewStateForView(ViewInfo);
	if (!ViewState)
		return;

	if (HVPT::IsDebugShowFlagEnabled(InView) && HasBeenProduced(ViewState->DebugTexture))
	{
		FRDGTextureRef RenderTarget = RegisterExternalTexture(
			GraphBuilder, ViewInfo.Family->RenderTarget->GetRenderTargetTexture(), TEXT("HVPT.DebugRenderTarget")
		);

		AddDrawTexturePass(
			GraphBuilder,
			FScreenPassViewInfo{ ViewInfo },
			ViewState->DebugTexture,
			RenderTarget,
			FIntPoint::ZeroValue, ViewState->DebugTexture->Desc.Extent,	// Input
			FIntPoint::ZeroValue, RenderTarget->Desc.Extent				// Output
		);
	}

	// Extract resources used between frames
	if (ViewState->OrthoGridUniformBuffer && ViewState->FrustumGridUniformBuffer)
	{
		HVPT::ExtractOrthoVoxelGridUniformBuffer(GraphBuilder, ViewState->OrthoGridUniformBuffer, ViewState->OrthoGridParameterCache);
		HVPT::ExtractFrustumVoxelGridUniformBuffer(GraphBuilder, ViewState->FrustumGridUniformBuffer, ViewState->FrustumGridParameterCache);
	}
	if (ViewState->TemporalAccumulationTexture)
	{
		GraphBuilder.QueueTextureExtraction(ViewState->TemporalAccumulationTexture, &ViewState->TemporalAccumulationRT);
	}

	// Clear dangling (once RDG has executed) pointers
	ViewState->TemporalAccumulationTexture = nullptr;
	ViewState->RadianceTexture = nullptr;
	ViewState->FeatureTexture = nullptr;
	ViewState->DepthBufferCopy = nullptr;
	ViewState->FrustumGridUniformBuffer = nullptr;
	ViewState->OrthoGridUniformBuffer = nullptr;
	ViewState->DebugTexture = nullptr;

#endif
}

bool FHVPTViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
#if RHI_RAYTRACING
	return HVPT::UseHVPT_GameThread();
#else
	return false;
#endif
}

FHVPTViewState* FHVPTViewExtension::GetOrCreateViewStateForView(const FViewInfo& ViewInfo)
{
	if (auto ViewState = ViewInfo.ViewState)
	{
		if (ViewStates.Contains(ViewState))
		{
			return ViewStates[ViewState].Get();
		}

		return ViewStates.Add(ViewState, MakeUnique<FHVPTViewState>()).Get();
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE