#pragma once

#include "RenderGraphFwd.h"
#include "Rendering/VoxelGrid.h"


// Collection of all resources required by a view to render Heterogeneous Volumes with the path tracing pipeline
struct FHVPTViewState
{
	// RDG resources used within the frame
	FRDGTextureRef RadianceTexture = nullptr;
	FRDGTextureRef FeatureTexture = nullptr; // uint32 texture - low 16 bits transmittance, high 16 bits depth of first scattering event

	FRDGTextureRef TemporalAccumulationTexture = nullptr;

	FRDGTextureRef DepthBufferCopy = nullptr;

	TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> OrthoGridUniformBuffer = nullptr;
	TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> FrustumGridUniformBuffer = nullptr;

	// Cached resources used between frames

	TRefCountPtr<IPooledRenderTarget> TemporalAccumulationRT = nullptr;

	TRefCountPtr<FRDGPooledBuffer> ReSTIRReservoirCache = nullptr;
	TRefCountPtr<FRDGPooledBuffer> ReSTIRExtraBounceCache = nullptr;

	FHVPTOrthoGridParameterCache OrthoGridParameterCache;
	FHVPTFrustumGridParameterCache FrustumGridParameterCache;

	uint32 AccumulatedSampleCount = 0;
};
