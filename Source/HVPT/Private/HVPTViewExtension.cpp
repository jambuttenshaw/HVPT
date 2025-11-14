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
			FRDGTextureDesc::Create2D(ViewInfo.ViewRect.Size(), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV),
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
	if (ViewState->FeatureRT)
	{
		ViewState->TemporalFeatureTexture = GraphBuilder.RegisterExternalTexture(ViewState->FeatureRT);
	}
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
	if (HVPT::GetFreezeFrame() && ViewState->TemporalFeatureTexture)
	{
		ViewState->FeatureTexture = ViewState->TemporalFeatureTexture;
	}
	else
	{
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
	}

	// Create radiance texture
	if (HVPT::GetFreezeFrame() && ViewState->RadianceRT)
	{
		ViewState->RadianceTexture = GraphBuilder.RegisterExternalTexture(ViewState->RadianceRT);
	}
	else
	{
		FRDGTextureDesc Desc = ViewInfo.GetSceneTextures().Color.Target->Desc;
		Desc.Format = PF_FloatRGB;
		Desc.Flags &= ~(TexCreate_FastVRAM);
		ViewState->RadianceTexture = GraphBuilder.CreateTexture(Desc, TEXT("HVPT.Radiance"));

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
	}

	if (HVPT::ShouldAccumulate())
	{
		const auto& PrevView = ViewInfo.PrevViewInfo;
		bool bViewChanged = ViewInfo.ViewRect != PrevView.ViewRect || !ViewInfo.ViewMatrices.GetViewProjectionMatrix().Equals(PrevView.ViewMatrices.GetViewProjectionMatrix(), 0.1);

		FRDGTextureDesc RadianceTextureDesc = ViewState->RadianceTexture->Desc;
		if (bViewChanged || 
			(ViewState->TemporalAccumulationTexture && RadianceTextureDesc.Extent != ViewState->TemporalAccumulationTexture->Desc.Extent))
		{
			ViewState->TemporalAccumulationTexture = nullptr;
			ViewState->AccumulatedSampleCount = 0;
		}

		if (!ViewState->TemporalAccumulationTexture)
		{
			// Create temporal accumulation texture
			ViewState->TemporalAccumulationTexture = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(RadianceTextureDesc.Extent, PF_A32B32G32R32F, FClearValueBinding::Black, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV),
				TEXT("HVPT.TemporalAccumulation"));
		}

		if (ViewState->AccumulatedSampleCount == 0)
		{
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

		HVPT::DrawDebugOverlay(
			GraphBuilder,
			ViewInfo,
			*ViewState
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
	if (ViewState->FeatureTexture)
		GraphBuilder.QueueTextureExtraction(ViewState->FeatureTexture, &ViewState->FeatureRT);
	else
		ViewState->FeatureRT = nullptr;
	if (ViewState->TemporalAccumulationTexture)
		GraphBuilder.QueueTextureExtraction(ViewState->TemporalAccumulationTexture, &ViewState->TemporalAccumulationRT);
	else
		ViewState->TemporalAccumulationRT = nullptr;
	if (HVPT::GetFreezeFrame() && ViewState->RadianceTexture)
		GraphBuilder.QueueTextureExtraction(ViewState->RadianceTexture, &ViewState->RadianceRT);
	else
		ViewState->RadianceRT = nullptr;

	// Clear will-be-dangling (once RDG has executed) pointers (this view state should not be accessed across frames)
	ViewState->TemporalAccumulationTexture = nullptr;
	ViewState->RadianceTexture = nullptr;
	ViewState->FeatureTexture = nullptr;
	ViewState->TemporalFeatureTexture = nullptr;
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


void HVPT::DrawDebugOverlay(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	FHVPTViewState& State)
{
	FScreenPassRenderTarget Output(State.DebugTexture, ViewInfo.ViewRect, ERenderTargetLoadAction::ELoad);
	AddDrawCanvasPass(GraphBuilder, RDG_EVENT_NAME("HVPTDebugOverlay"), ViewInfo, Output,
		[&ViewInfo](FCanvas& Canvas)
		{
			float X = 20;
			float Y = 20;
			constexpr float YStep = 14;

			FString Line;

			Line = FString::Printf(TEXT("HVPT Debug"));
			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::White);

			Line = FString::Printf(TEXT("Use r.HVPT.DebugViewMode to select mode."));
			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Gray);

			Y += YStep;

			uint32 ViewMode = HVPT::GetDebugViewMode();

			// Not including custom - that is a special case
			static const FString DebugViewModes[] = {
				TEXT("Num Bounces"),
				TEXT("Path Type"),
				TEXT("Light ID"),
				TEXT("Temporal Reuse (TR)"),
				TEXT("Spatial Reuse (SR)"),
				TEXT("Fireflies"),
				TEXT("Reprojection"),
				TEXT("Multi-Pass SR Overalloc"),
			};

			{
				Line = FString::Printf(TEXT("-1: Custom"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), (HVPT_DEBUG_VIEW_MODE_CUSTOM == ViewMode) ? FLinearColor::Yellow : FLinearColor::Gray);
			}
			for (uint32 i = 0; i < std::size(DebugViewModes); i++)
			{
				Line = FString::Printf(TEXT(" %d: %s"), i, *DebugViewModes[i]);
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), (i == ViewMode) ? FLinearColor::Yellow : FLinearColor::Gray);
			}
			if (ViewMode != HVPT_DEBUG_VIEW_MODE_CUSTOM && ViewMode >= std::size(DebugViewModes))
			{
				Line = FString::Printf(TEXT("Unknown view mode '%d'"), ViewMode);
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Red);
			}

			// Add view-mode specific context
			Y += 2.0f * YStep;

			switch (ViewMode)
			{
			case HVPT_DEBUG_VIEW_MODE_PATH_TYPE:
			{
				Line = FString::Printf(TEXT("Path Types:"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::White);
				Line = FString::Printf(TEXT("Absorption"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Red);
				Line = FString::Printf(TEXT("Scattering"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Green);
				Line = FString::Printf(TEXT("Surface"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Blue);
			} break;
			case HVPT_DEBUG_VIEW_MODE_TEMPORAL_REUSE:
			{
				bool bEnabled = HVPT::GetTemporalReuseEnabled();
				Line = FString::Printf(TEXT("Temporal Reuse=%s"), bEnabled ? TEXT("True") : TEXT("False"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), bEnabled ? FLinearColor::Green : FLinearColor::Red);
				Line = FString::Printf(TEXT("Key:"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Gray);
				Line = FString::Printf(TEXT("Canonical Sample"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Yellow);
				Line = FString::Printf(TEXT("Temporal Sample"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), {0.0f, 1.0f, 1.0f});
			} break;
			case HVPT_DEBUG_VIEW_MODE_SPATIAL_REUSE:
			{
				bool bEnabled = HVPT::GetSpatialReuseEnabled();
				Line = FString::Printf(TEXT("Spatial Reuse=%s"), bEnabled ? TEXT("True") : TEXT("False"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), bEnabled ? FLinearColor::Green : FLinearColor::Red);
				Line = FString::Printf(TEXT("Key:"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Gray);
				Line = FString::Printf(TEXT("Canonical Sample"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor::Yellow);
				Line = FString::Printf(TEXT("Spatial Sample"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), { 0.0f, 1.0f, 1.0f });
			} break;
			case HVPT_DEBUG_VIEW_MODE_REPROJECTION:
			{
				bool bEnabled = HVPT::GetTemporalReuseEnabled();
				Line = FString::Printf(TEXT("Temporal Reuse=%s"), bEnabled ? TEXT("True") : TEXT("False"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), bEnabled ? FLinearColor::Green : FLinearColor::Red);

				bEnabled = HVPT::GetTemporalReprojectionEnabled();
				Line = FString::Printf(TEXT("Temporal Reprojection=%s"), bEnabled ? TEXT("True") : TEXT("False"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), bEnabled ? FLinearColor::Green : FLinearColor::Red);
			} break;
			case HVPT_DEBUG_VIEW_MODE_MULTI_PASS_OVERALLOCATION:
			{
				bool bEnabled = HVPT::GetSpatialReuseEnabled() && HVPT::GetMultiPassSpatialReuseEnabled();
				Line = FString::Printf(TEXT("Multi-Pass Spatial Reuse=%s"), bEnabled ? TEXT("True") : TEXT("False"));
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), bEnabled ? FLinearColor::Green : FLinearColor::Red);
			}
			default:
				break;
			}
		});
}

#undef LOCTEXT_NAMESPACE