// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "MaterialShared.h"
#include "MeshBatch.h"

DECLARE_LOG_CATEGORY_EXTERN(LogHVPT, Log, All);


/**	HETEROGENEOUS VOLUMES PATH TRACING (HVPT)
 *	This plugin implements a custom path tracing pipeline to render heterogeneous volumes.
 *
 *	CVar Setup:
 *	To use this plugin, at least the following 3 CVars are required:
 *	 
 *		r.Translucency.HeterogeneousVolumes=2 
 *		r.HeterogeneousVolumes.StepSize=1000000
 *		
 *		r.HVPT=True
 *
 */


class FViewInfo;

namespace HVPT
{
	// Where not specified, these getters should be called on the RENDER THREAD ONLY
	
	HVPT_API bool UseHVPT_GameThread();
	HVPT_API bool UseHVPT_RenderThread();

	HVPT_API bool ShouldWriteVelocity();
	HVPT_API bool ShouldWriteGBuffer();
	HVPT_API bool ShouldWriteNormals();
	HVPT_API bool ShouldAccumulate();
	HVPT_API bool UseSurfaceContributions();
	HVPT_API int32 GetMaxBounces();
	HVPT_API int32 GetMaxRaymarchSteps();

	// ReSTIR Pipeline
	HVPT_API bool UseReSTIR();
	HVPT_API int32 GetNumInitialCandidates();
	HVPT_API bool GetUseShadowTermForCandidateGeneration();
	HVPT_API bool GetTemporalReuseEnabled();
	HVPT_API bool GetTemporalReuseMISEnabled();
	HVPT_API float GetTemporalReuseHistoryThreshold();
	HVPT_API bool GetTemporalReprojectionEnabled();
	HVPT_API bool GetSpatialReuseEnabled();
	HVPT_API int32 GetNumSpatialReuseSamples();
	HVPT_API float GetSpatialReuseRadius();
	HVPT_API bool GetSpatialReuseMISEnabled();

	// GBuffer writing controls
	HVPT_API int32 GetTransmittanceMode();
	HVPT_API bool GetStochasticGBufferWrites();
	HVPT_API float GetSharpness();

	// Voxel grid build
	HVPT_API bool GetRebuildEveryFrame();
	HVPT_API bool GetShouldJitter();
	HVPT_API float GetMinimumVoxelSizeInsideFrustum();
	HVPT_API float GetMinimumVoxelSizeOutsideFrustum();
	HVPT_API int32 GetBottomLevelGridResolution();
	HVPT_API float GetInsideFrustumShadingRate();
	HVPT_API float GetOutsideFrustumShadingRate();

	enum class EFogCompositionMode
	{
		Disabled = 0,
		PostOnly,
		PostAndPathTracing
	};
	HVPT_API EFogCompositionMode GetFogCompositingMode();
	HVPT_API float GetFogCompositingOpticalDepthThreshold();

	HVPT_API bool EnableFrustumGrid();
	HVPT_API float GetNearPlaneDistanceForFrustumGrid();
	HVPT_API float GetFarPlaneDistanceForFrustumGrid();
	HVPT_API int32 GetDepthSliceCountForFrustumGrid();
	HVPT_API int32 GetMaxBottomLevelMemoryInMegabytesForFrustumGrid();

	HVPT_API bool EnableOrthoGrid();
	HVPT_API int32 GetMaxBottomLevelMemoryInMegabytesForOrthoGrid();

	// Debug tools
	HVPT_API bool GetFreezeTemporalSeed();
	HVPT_API bool GetFreezeFrame();
	HVPT_API bool GetDisableRadiance();
	HVPT_API bool GetVisualizeVelocity();
	HVPT_API uint32 GetDebugViewMode();

	// Query support
	HVPT_API bool DoesPlatformSupportHVPT(EShaderPlatform Platform);
	HVPT_API bool DoesMaterialShaderSupportHVPT(const FMaterialShaderParameters& Parameters);
	HVPT_API bool DoesMaterialShaderSupportHVPT(const FMaterial& Material);

	HVPT_API bool ShouldRenderHVPTForView(const FViewInfo& View);
	HVPT_API bool ShouldRenderMeshBatchWithHVPT(const FMeshBatch* Mesh, const FPrimitiveSceneProxy* Proxy, ERHIFeatureLevel::Type FeatureLevel);

	HVPT_API bool ShouldUseSER();

	// Extended heterogeneous volume interface

	// Use templates below externally
	void RegisterProxyWithExtendedInterfaceHash(size_t Hash);
	void UnregisterProxyWithExtendedInterfaceHash(size_t Hash);

	template<typename Proxy>
	void RegisterProxyWithExtendedInterface()
	{
		HVPT::RegisterProxyWithExtendedInterfaceHash(Proxy::GetStaticTypeHash());
	}
	template<typename Proxy>
	void UnregisterProxyWithExtendedInterface()
	{
		HVPT::UnregisterProxyWithExtendedInterfaceHash(Proxy::GetStaticTypeHash());
	}

	HVPT_API bool HasExtendedInterface(const FPrimitiveSceneProxy* Proxy);
}


class FHVPTModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
