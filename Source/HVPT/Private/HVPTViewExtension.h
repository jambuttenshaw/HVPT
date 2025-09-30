#pragma once

#include "SceneViewExtension.h"

class FSceneViewState;
struct FHVPTViewState;

class FSceneTextureParameters;

class FHVPTViewExtension : public FSceneViewExtensionBase
{
public:
	FHVPTViewExtension(const FAutoRegister& AutoRegister);
	~FHVPTViewExtension();

	//~ Begin ISceneViewExtension interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;

	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures
	) override;

	virtual void PrePostProcessPass_RenderThread(
		FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs
	) override;

	virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
	//~ End ISceneViewExtension interface

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	FHVPTViewState* GetOrCreateViewStateForView(const FViewInfo& ViewInfo);

private:

	// Composites debug texture after scene render, to visualize data from HVPT rendering.
	// The debug texture is available to write to from any pass for general purpose visualization and quick debugging.
	

private:
	FDelegateHandle PrepareRTDelegateHandle;
	FDelegateHandle AnyRTPassEnabledDelegateHandle;

	TMap<FSceneViewState*, TUniquePtr<FHVPTViewState>> ViewStates;
};


namespace HVPT
{
#if RHI_RAYTRACING

void PrepareRaytracingShaders(const FViewInfo& View, const FHVPTViewState& State, TArray<FRHIRayTracingShader*>& OutRayGenShaders);
void PrepareRaytracingShadersReSTIR(const FViewInfo& View, const FHVPTViewState& State, TArray<FRHIRayTracingShader*>& OutRayGenShaders);

void RenderPrePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	const FSceneTextures& SceneTextures,
	FHVPTViewState& State
);

void RenderWithPathTracing(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& ViewInfo,
	const FSceneTextures& SceneTextures,
	FHVPTViewState& State
);

void RenderWithReSTIRPathTracing(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& ViewInfo,
	const FSceneTextures& SceneTextures,
	FHVPTViewState& State
);

void Composite(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& ViewInfo,
	const FSceneTextures& SceneTextures,
	FHVPTViewState& State
);

void Accumulate(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	FHVPTViewState& State
);

#endif
}
