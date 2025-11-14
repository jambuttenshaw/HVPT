// Copyright Epic Games, Inc. All Rights Reserved.

#include "HVPT.h"

#include "HeterogeneousVolumeExSceneProxy.h"
#include "PrimitiveSceneProxy.h"
#include "RenderUtils.h"
#include "SceneRendering.h"
#include "ShaderCore.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialRenderProxy.h"
#include "Misc/Paths.h"


#define LOCTEXT_NAMESPACE "FHVPTModule"

DEFINE_LOG_CATEGORY(LogHVPT);


// HVPT CVars
static TAutoConsoleVariable<bool> CVarHVPT(
	TEXT("r.HVPT"),
	false,
	TEXT("Enables the path tracing pipeline for Heterogeneous Volumes (Default = false)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTVelocity(
	TEXT("r.HVPT.Velocity"),
	true,
	TEXT("Enables writing volume velocity to the velocity buffer (Default = true)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTGBuffer(
	TEXT("r.HVPT.WriteGBuffer"),
	true,
	TEXT("Enables writing to the GBuffer (Default = true)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTNormals(
	TEXT("r.HVPT.WriteNormals"),
	true,
	TEXT("Enables writing volume density gradient to the GBuffer (Default = true)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTAccumulate(
	TEXT("r.HVPT.Accumulate"),
	false,
	TEXT("Enables accumulation of path tracing samples to test converged lighting."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTSurfaceContributions(
	TEXT("r.HVPT.SurfaceContributions"),
	true,
	TEXT("Enables direct and indirect illumination from surfaces onto volumes."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTSamplesPerPixel(
	TEXT("r.HVPT.SamplesPerPixel"),
	1,
	TEXT("Number of samples per pixel (not for ReSTIR pipeline)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTMaxBounces(
	TEXT("r.HVPT.MaxBounces"),
	4,
	TEXT("Maximum number of bounces per ray (default = 4)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTMaxRaymarchSteps(
	TEXT("r.HVPT.MaxRaymarchSteps"),
	256,
	TEXT("Maximum number of tracking steps through volume before terminating to avoid TDR."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTReSTIR(
	TEXT("r.HVPT.ReSTIR"),
	false,
	TEXT("Enables the ReSTIR path tracing pipeline for Heterogeneous Volumes (Default = false)."
		"r.HVPT must be enabled first."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTReSTIRInitialCandidates(
	TEXT("r.HVPT.ReSTIR.NumInitialCandidates"),
	1,
	TEXT("Number of candidate paths generated (Default = 1)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRShadowTermForCandidateGeneration(
	TEXT("r.HVPT.ReSTIR.ShadowTermForCandidateGeneration"),
	true,
	TEXT("Include shadow term for generating candidate samples (Default = true)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRTemporalReuse(
	TEXT("r.HVPT.ReSTIR.TemporalReuse"),
	true,
	TEXT("Enable temporal reuse in ReSTIR pipeline (Default = true)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRTemporalReuseMIS(
	TEXT("r.HVPT.ReSTIR.TemporalReuse.MIS"),
	true,
	TEXT("Enable Talbot MIS in temporal reuse in ReSTIR pipeline (Default = true)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTReSTIRTemporalReuseHistoryThreshold(
	TEXT("r.HVPT.ReSTIR.TemporalReuse.HistoryThreshold"),
	2.0f,
	TEXT("Threshold on weight for temporal samples to prevent infinite reuse."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRTemporalReprojection(
	TEXT("r.HVPT.ReSTIR.TemporalReprojection"),
	true,
	TEXT("Enable temporal reprojection when selecting a sample from previous frames reservoirs for resampling."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTReSTIRSpatialReuseSamples(
	TEXT("r.HVPT.ReSTIR.SpatialReuse.NumSamples"),
	0,
	TEXT("Number of neighbouring pixels considered for spatial reuse. Setting to 0 disables spatial reuse. (Default = 4)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTReSTIRSpatialReuseRadius(
	TEXT("r.HVPT.ReSTIR.SpatialReuse.Radius"),
	10.0f,
	TEXT("Radius around each pixel where spatial reuse is considered."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRSpatialReuseMIS(
	TEXT("r.HVPT.ReSTIR.SpatialReuse.MIS"),
	true,
	TEXT("Enable Talbot MIS in spatial reuse in ReSTIR pipeline (Default = true)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTReSTIRMultiPassSpatialReuse(
	TEXT("r.HVPT.ReSTIR.SpatialReuse.MultiPass"),
	true,
	TEXT("Whether to use multi-pass pipeline for spatial reuse."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<int32> CVarHVPTTransmittanceMode(
	TEXT("r.HVPT.TransmittanceMode"),
	1,
	TEXT("Set method used to calculate transmittance in the transmittance pass."
		"0: Ray marching"
		"1: Ray marching + stochastic translucency (default)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTStochasticGBuffer(
	TEXT("r.HVPT.WriteGBuffer.Stochastic"),
	true,
	TEXT("Enables stochastic writing to the GBuffer (Default = true)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTSharpness(
	TEXT("r.HVPT.WriteGBuffer.Sharpness"),
	0.1f,
	TEXT("Generally makes the volume appear more or less 'sharp' after denoising with DLSS-RR. In range [0,1]."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTVoxelGridRebuildEveryFrame(
	TEXT("r.HVPT.RebuildEveryFrame"),
	true,
	TEXT("Enable rebuilding of voxel grid every frame regardless of world state."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTVoxelGridJitter(
	TEXT("r.HVPT.Jitter"),
	true,
	TEXT("Enables temporal jitter when building the voxel grid."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTMinimumVoxelSizeInsideFrustum(
	TEXT("r.HVPT.MinimumVoxelSizeInsideFrustum"),
	0.1f,
	TEXT("The minimum voxel size (Default = 100.0)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTMinimumVoxelSizeOutsideFrustum(
	TEXT("r.HVPT.MinimumVoxelSizeOutsideFrustum"),
	100.0f,
	TEXT("The minimum voxel size (Default = 100.0)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTBottomLevelGridResolution(
	TEXT("r.HVPT.BottomLevelGridResolution"),
	4,
	TEXT("Determines intra-tile bottom-level grid resolution (Default = 4)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTShadingRateInsideFrustum(
	TEXT("r.HVPT.ShadingRateInsideFrustum"),
	2.0f,
	TEXT("Shading rate for voxels inside view frustum (Default = 2)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTShadingRateOutsideFrustum(
	TEXT("r.HVPT.ShadingRateOutsideFrustum"),
	4.0f,
	TEXT("Shading rate for voxels outside view frustum (Default = 4)."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<int32> CVarHVPTFogCompositingMode(
	TEXT("r.HVPT.FogCompositingMode"),
	2,
	TEXT("Method for accumulating volumetric fog with heterogeneous volumes."
		"0: Disabled."
		"1: Composition as a post-process only."
		"2: Post-process composition with also rasterizing global fog into voxel grid (default)."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTFogCompositingOpticalDepthThreshold(
	TEXT("r.HVPT.FogCompositingOpticalDepthThreshold"),
	1.0f,
	TEXT("The optical depth threshold at which volumetric fog will be composited onto the heterogeneous volumes."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTFrustumGrid(
	TEXT("r.HVPT.FrustumGrid"),
	true,
	TEXT("Enables frustum grid."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTFrustumGridNearPlaneDistance(
	TEXT("r.HVPT.FrustumGrid.NearPlaneDistance"),
	1.0f,
	TEXT("Sets near-plane distance for the frustum grid (Default = 1.0)\n"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarHVPTFrustumGridFarPlaneDistance(
	TEXT("r.HVPT.FrustumGrid.FarPlaneDistance"),
	-1.0f,
	TEXT("Sets far-plane distance for the frustum grid (Default = -1.0)\n"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTFrustumGridDepthSliceCount(
	TEXT("r.HVPT.FrustumGrid.DepthSliceCount"),
	512,
	TEXT("The number of depth slices (Default = 512)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTFrustumGridMaxMemory(
	TEXT("r.HVPT.FrustumGrid.MaxBottomLevelMemoryMegabytes"),
	128,
	TEXT("Maximum allowed size of bottom level grid in megabytes (Default = 128)"),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTOrthoGrid(
	TEXT("r.HVPT.OrthoGrid"),
	true,
	TEXT("Enables ortho grid."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTOrthoGridMaxMemory(
	TEXT("r.HVPT.OrthoGrid.MaxBottomLevelMemoryMegabytes"),
	128,
	TEXT("Maximum allowed size of bottom level grid in megabytes (Default = 128)"),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTUseSER(
	TEXT("r.HVPT.SER"),
	false,
	TEXT("Enables SER if available on the current platform."),
	ECVF_RenderThreadSafe
);


static TAutoConsoleVariable<bool> CVarHVPTFreezeTemporalSeed(
	TEXT("r.HVPT.FreezeTemporalSeed"),
	false,
	TEXT("Debug tool: Freezes temporal seed to help debugging path tracing."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTFreezeFrame(
	TEXT("r.HVPT.FreezeFrame"),
	false,
	TEXT("Debug tool: Re-uses last frames image, no new computation is performed."
		"Will not correctly freeze denoised image but is useful for debugging pre-denoised image."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTDisableRadiance(
	TEXT("r.HVPT.DisableRadiance"),
	false,
	TEXT("Debug tool: Only executes pre-pass and composition pass, and not the radiance pass."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<bool> CVarHVPTVisualizeVelocity(
	TEXT("r.HVPT.Velocity.Visualize"),
	false,
	TEXT("Debug tool: Renders world-space velocity into the base color view of the volume for debugging."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarHVPTDebugViewMode(
	TEXT("r.HVPT.DebugViewMode"),
	0,
	TEXT("Debug tool: Changes the view mode shown by the HVPT Debug show flag."
		"For a list of view modes and their IDs, see HVPT/Shaders/Shared/HVPTDefinitions.h"),
	ECVF_RenderThreadSafe
);


// HVPT getters
namespace HVPT
{
	bool UseHVPT_GameThread()
	{
		return IsRayTracingEnabled() && CVarHVPT.GetValueOnGameThread();
	}

	bool UseHVPT_RenderThread()
	{
		return IsRayTracingEnabled() && CVarHVPT.GetValueOnRenderThread();
	}

	bool ShouldWriteVelocity()
	{
		return CVarHVPTVelocity.GetValueOnRenderThread();
	}

	bool ShouldWriteGBuffer()
	{
		return CVarHVPTGBuffer.GetValueOnRenderThread();
	}

	bool ShouldWriteNormals()
	{
		return CVarHVPTNormals.GetValueOnRenderThread();
	}

	bool ShouldAccumulate()
	{
		return CVarHVPTAccumulate.GetValueOnRenderThread();
	}

	bool UseSurfaceContributions()
	{
		return CVarHVPTSurfaceContributions.GetValueOnRenderThread();
	}

	int32 GetSamplesPerPixel()
	{
		return FMath::Max(CVarHVPTSamplesPerPixel.GetValueOnRenderThread(), 1);
	}

	int32 GetMaxBounces()
	{
		return FMath::Max(CVarHVPTMaxBounces.GetValueOnRenderThread(), 1);
	}

	int32 GetMaxRaymarchSteps()
	{
		return FMath::Max(CVarHVPTMaxRaymarchSteps.GetValueOnRenderThread(), 1);
	}


	bool UseReSTIR()
	{
		return CVarHVPTReSTIR.GetValueOnRenderThread();
	}

	bool GetTemporalReuseEnabled()
	{
		return CVarHVPTReSTIRTemporalReuse.GetValueOnRenderThread();	
	}

	bool GetTemporalReuseMISEnabled()
	{
		return CVarHVPTReSTIRTemporalReuseMIS.GetValueOnRenderThread();
	}

	float GetTemporalReuseHistoryThreshold()
	{
		return CVarHVPTReSTIRTemporalReuseHistoryThreshold.GetValueOnRenderThread();
	}

	bool GetTemporalReprojectionEnabled()
	{
		return CVarHVPTReSTIRTemporalReprojection.GetValueOnRenderThread();
	}

	bool GetSpatialReuseEnabled()
	{
		return GetNumSpatialReuseSamples() > 0;
	}

	bool GetMultiPassSpatialReuseEnabled()
	{
		return CVarHVPTReSTIRMultiPassSpatialReuse.GetValueOnRenderThread();
	}

	int32 GetNumInitialCandidates()
	{
		return FMath::Max(CVarHVPTReSTIRInitialCandidates.GetValueOnRenderThread(), 1);
	}

	bool GetUseShadowTermForCandidateGeneration()
	{
		return CVarHVPTReSTIRShadowTermForCandidateGeneration.GetValueOnRenderThread();
	}

	int32 GetNumSpatialReuseSamples()
	{
		return FMath::Max(CVarHVPTReSTIRSpatialReuseSamples.GetValueOnRenderThread(), 0);
	}

	float GetSpatialReuseRadius()
	{
		return FMath::Max(CVarHVPTReSTIRSpatialReuseRadius.GetValueOnRenderThread(), 0.0f);
	}

	bool GetSpatialReuseMISEnabled()
	{
		return CVarHVPTReSTIRSpatialReuseMIS.GetValueOnRenderThread();	
	}


	int32 GetTransmittanceMode()
	{
		return FMath::Clamp(CVarHVPTTransmittanceMode.GetValueOnRenderThread(), 0, 1);
	}

	bool GetStochasticGBufferWrites()
	{
		return CVarHVPTStochasticGBuffer.GetValueOnRenderThread();
	}

	float GetSharpness()
	{
		return FMath::Clamp(CVarHVPTSharpness.GetValueOnRenderThread(), 0.0f, 1.0f);
	}


	bool GetRebuildEveryFrame()
	{
		return CVarHVPTVoxelGridRebuildEveryFrame.GetValueOnRenderThread();
	}

	bool GetShouldJitter()
	{
		return CVarHVPTVoxelGridJitter.GetValueOnRenderThread();
	}

	float GetMinimumVoxelSizeInsideFrustum()
	{
		return FMath::Max(CVarHVPTMinimumVoxelSizeInsideFrustum.GetValueOnRenderThread(), 0.01f);
	}

	float GetMinimumVoxelSizeOutsideFrustum()
	{
		return FMath::Max(CVarHVPTMinimumVoxelSizeOutsideFrustum.GetValueOnRenderThread(), 0.01f);
	}

	int32 GetBottomLevelGridResolution()
	{
		return FMath::Max(CVarHVPTBottomLevelGridResolution.GetValueOnRenderThread(), 1);
	}

	float GetInsideFrustumShadingRate()
	{
		return FMath::Max(CVarHVPTShadingRateInsideFrustum.GetValueOnRenderThread(), 0.1f);
	}

	float GetOutsideFrustumShadingRate()
	{
		return FMath::Max(CVarHVPTShadingRateOutsideFrustum.GetValueOnRenderThread(), 0.1f);
	}


	EFogCompositionMode GetFogCompositingMode()
	{
		int32 ClampedValue = FMath::Clamp(CVarHVPTFogCompositingMode.GetValueOnRenderThread(), 0, 2);
		return static_cast<EFogCompositionMode>(ClampedValue);
	}

	float GetFogCompositingOpticalDepthThreshold()
	{
		return CVarHVPTFogCompositingOpticalDepthThreshold.GetValueOnRenderThread();
	}


	bool EnableFrustumGrid()
	{
		return CVarHVPTFrustumGrid.GetValueOnRenderThread();
	}

	float GetNearPlaneDistanceForFrustumGrid()
	{
		return CVarHVPTFrustumGridNearPlaneDistance.GetValueOnRenderThread();
	}

	float GetFarPlaneDistanceForFrustumGrid()
	{
		return CVarHVPTFrustumGridFarPlaneDistance.GetValueOnRenderThread();
	}

	int32 GetDepthSliceCountForFrustumGrid()
	{
		return FMath::Max(CVarHVPTFrustumGridDepthSliceCount.GetValueOnRenderThread(), 1);
	}

	int32 GetMaxBottomLevelMemoryInMegabytesForFrustumGrid()
	{
		return FMath::Max(CVarHVPTFrustumGridMaxMemory.GetValueOnRenderThread(), 1);
	}


	bool EnableOrthoGrid()
	{
		return CVarHVPTOrthoGrid.GetValueOnRenderThread();
	}

	int32 GetMaxBottomLevelMemoryInMegabytesForOrthoGrid()
	{
		return FMath::Max(CVarHVPTOrthoGridMaxMemory.GetValueOnRenderThread(), 1);
	}


	bool GetFreezeTemporalSeed()
	{
		return CVarHVPTFreezeTemporalSeed.GetValueOnRenderThread();
	}

	bool GetFreezeFrame()
	{
		return CVarHVPTFreezeFrame.GetValueOnRenderThread();
	}

	bool GetDisableRadiance()
	{
		return CVarHVPTDisableRadiance.GetValueOnRenderThread();
	}

	bool GetVisualizeVelocity()
	{
		return CVarHVPTVisualizeVelocity.GetValueOnRenderThread();
	}

	uint32 GetDebugViewMode()
	{
		int32 ViewMode = CVarHVPTDebugViewMode.GetValueOnRenderThread();
		if (ViewMode < 0 || ViewMode > 255)
		{
			ViewMode = 255;
		}
		return static_cast<uint32>(ViewMode);
	}


	bool DoesPlatformSupportHVPT(EShaderPlatform Platform)
	{
		return DoesPlatformSupportHeterogeneousVolumes(Platform)
			&& IsRayTracingEnabled(Platform);
	}

	bool DoesMaterialShaderSupportHVPT(const FMaterialShaderParameters& Parameters)
	{
		return (Parameters.MaterialDomain == MD_Volume)
			&& Parameters.bIsUsedWithHeterogeneousVolumes;
	}

	bool DoesMaterialShaderSupportHVPT(const FMaterial& Material)
	{
		return (Material.GetMaterialDomain() == MD_Volume)
			&& Material.IsUsedWithHeterogeneousVolumes();
	}

	bool ShouldRenderHVPTForView(const FViewInfo& View)
	{
		return !View.HeterogeneousVolumesMeshBatches.IsEmpty()
			&& View.Family
			&& !View.bIsReflectionCapture;
	}

	bool ShouldRenderMeshBatchWithHVPT(const FMeshBatch* Mesh, const FPrimitiveSceneProxy* Proxy, ERHIFeatureLevel::Type FeatureLevel)
	{
		check(Mesh);
		check(Proxy);
		check(Mesh->MaterialRenderProxy);

		const FMaterialRenderProxy* MaterialRenderProxy = Mesh->MaterialRenderProxy;
		const FMaterial& Material = MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, MaterialRenderProxy);
		return HVPT::UseHVPT_RenderThread()
			&& Proxy->IsHeterogeneousVolume()
			&& DoesMaterialShaderSupportHVPT(Material);
	}


	bool ShouldUseSER()
	{
		return GRHIGlobals.SupportsShaderExecutionReordering && CVarHVPTUseSER.GetValueOnRenderThread();
	}


	static TSet<size_t> GExtendedInterfaceHashes;

	void RegisterProxyWithExtendedInterfaceHash(size_t Hash)
	{
		GExtendedInterfaceHashes.Add(Hash);
	}

	void UnregisterProxyWithExtendedInterfaceHash(size_t Hash)
	{
		check(GExtendedInterfaceHashes.Contains(Hash));
		GExtendedInterfaceHashes.Remove(Hash);	
	}

	bool HasExtendedInterface(const FPrimitiveSceneProxy* Proxy)
	{
		return GExtendedInterfaceHashes.Contains(Proxy->GetTypeHash());
	}

}


void FHVPTModule::StartupModule()
{
	FString PluginDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("HVPT"));
	FString ShaderDirectory = FPaths::Combine(PluginDir, TEXT("Shaders"));
	AddShaderSourceDirectoryMapping("/Plugin/HVPT", ShaderDirectory);
	AddShaderSourceSharedVirtualDirectory(TEXT("/Plugin/HVPT/Shared/"));

	HVPT::RegisterProxyWithExtendedInterface<FHeterogeneousVolumeExSceneProxy>();
}

void FHVPTModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FHVPTModule, HVPT)