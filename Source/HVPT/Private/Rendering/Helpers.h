#pragma once

#include "RHIShaderPlatform.h"
#include "RenderGraphFwd.h"
#include "ShaderParameterMacros.h"

class FRHICommandList;

class FRHIUniformBuffer;
class FViewInfo;
struct FScopedUniformBufferStaticBindings;
class FShaderBindingLayout;

struct FSceneTextures;
class FSceneTextureParameters;
class FSceneTextureUniformParameters;
class FExponentialHeightFogSceneInfo;

BEGIN_SHADER_PARAMETER_STRUCT(FHVPT_PathTracingFogParameters, )
	SHADER_PARAMETER(FVector2f, FogDensity)
	SHADER_PARAMETER(FVector2f, FogHeight)
	SHADER_PARAMETER(FVector2f, FogFalloff)
	SHADER_PARAMETER(FLinearColor, FogAlbedo)
	SHADER_PARAMETER(float, FogPhaseG)
	SHADER_PARAMETER(FVector2f, FogCenter)
	SHADER_PARAMETER(float, FogMinZ)
	SHADER_PARAMETER(float, FogMaxZ)
	SHADER_PARAMETER(float, FogRadius)
	SHADER_PARAMETER(float, FogFalloffClamp)
END_SHADER_PARAMETER_STRUCT()

namespace HVPT::Private
{
// Utilities from UE renderer module that are not made public but required by the plugin

FSceneTextureParameters GetSceneTextureParameters(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures);

const FShaderBindingLayout* GetShaderBindingLayout(EShaderPlatform ShaderPlatform);

TOptional<FScopedUniformBufferStaticBindings> BindStaticUniformBufferBindings(
	const FViewInfo& View,
	FRHIUniformBuffer* SceneUniformBuffer,
	FRHIUniformBuffer* NaniteRayTracingUniformBuffer,
	FRHICommandList& RHICmdList
);

FHVPT_PathTracingFogParameters PrepareFogParameters(const FViewInfo& View, const FExponentialHeightFogSceneInfo& FogInfo);

// GPU-driven radix sort (number of elements to sort is supplied by previous GPU work)
// Requires two buffers to operate (ping-pong)
// Can optionally sort arrays of values along with the keys
// BufferIndex specifies the buffer containing unsorted data initially
// Counter offset is the index (in num uints - not num bytes) into the buffer containing the counter to use for creating dispatch indirect args
// Returns index of buffer containing sorted result
// Implemented in RadixSort.cpp
uint32 SortBufferIndirect(
	FRDGBuilder& GraphBuilder, 
	TArrayView<FRDGBufferRef> InKeyBuffers,
	/* Optional */ TArrayView<FRDGBufferRef> InValueBuffers,
	int32 BufferIndex, 
	FRDGBufferRef Counter, 
	uint32 CounterOffset, 
	uint32 KeyMask, 
	ERHIFeatureLevel::Type FeatureLevel
);

// Overload with finer-grained control, for example in a situation with sub-allocating out of buffers
// Can specify the region of the buffer to be sorted manually with the SRV/UAV
uint32 SortBufferIndirect(
	FRDGBuilder& GraphBuilder,
	TArrayView<FRDGBufferSRVRef> InKeySRVs,
	TArrayView<FRDGBufferUAVRef> InKeyUAVs,
	/* Optional */ TArrayView<FRDGBufferSRVRef> InValueSRVs,
	/* Optional */ TArrayView<FRDGBufferUAVRef> InValueUAVs,
	int32 BufferIndex,
	FRDGBufferRef Counter,
	uint32 CounterOffset,
	uint32 KeyMask,
	ERHIFeatureLevel::Type FeatureLevel
);

}
