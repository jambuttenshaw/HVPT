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

}
