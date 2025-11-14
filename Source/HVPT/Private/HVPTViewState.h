#pragma once

#include "RenderGraphFwd.h"
#include "Rendering/VoxelGrid.h"


// Collection of all resources required by a view to render Heterogeneous Volumes with the path tracing pipeline
struct FHVPTViewState
{
	// RDG resources used within the frame
	FRDGTextureRef RadianceTexture = nullptr;
	FRDGTextureRef FeatureTexture = nullptr;

	FRDGTextureRef TemporalFeatureTexture = nullptr;
	FRDGTextureRef TemporalAccumulationTexture_Hi = nullptr;
	FRDGTextureRef TemporalAccumulationTexture_Lo = nullptr;

	FRDGTextureRef DepthBufferCopy = nullptr;

	TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> OrthoGridUniformBuffer = nullptr;
	TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> FrustumGridUniformBuffer = nullptr;

	FRDGTextureRef DebugTexture = nullptr; // General purpose texture for debug visualization
	uint32 DebugFlags = 0;

	// Cached resources used between frames

	TRefCountPtr<IPooledRenderTarget> TemporalAccumulationRT_Hi = nullptr;
	TRefCountPtr<IPooledRenderTarget> TemporalAccumulationRT_Lo = nullptr;

	TRefCountPtr<FRDGPooledBuffer> ReSTIRReservoirCache = nullptr;
	TRefCountPtr<FRDGPooledBuffer> ReSTIRExtraBounceCache = nullptr;

	FHVPTOrthoGridParameterCache OrthoGridParameterCache;
	FHVPTFrustumGridParameterCache FrustumGridParameterCache;

	uint32 AccumulatedSampleCount = 0;

	TRefCountPtr<IPooledRenderTarget> RadianceRT = nullptr;
	TRefCountPtr<IPooledRenderTarget> FeatureRT = nullptr;
};
