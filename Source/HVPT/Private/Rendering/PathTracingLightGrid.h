#pragma once

#include "ShaderParameterStruct.h"


// GPU Lightmass plugin also declares this type inside one of its translation units
// Be careful where this is included to avoid type redefinitions!!
BEGIN_SHADER_PARAMETER_STRUCT(FPathTracingLightGrid, )
	SHADER_PARAMETER(uint32, SceneInfiniteLightCount)
	SHADER_PARAMETER(FVector3f, SceneLightsTranslatedBoundMin)
	SHADER_PARAMETER(FVector3f, SceneLightsTranslatedBoundMax)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LightGrid)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, LightGridData)
	SHADER_PARAMETER(unsigned, LightGridResolution)
	SHADER_PARAMETER(unsigned, LightGridMaxCount)
	SHADER_PARAMETER(int, LightGridAxis)
END_SHADER_PARAMETER_STRUCT()

class FPathTracingSkylight;


namespace HVPT::Private
{
	// Path tracers are built from UE's path tracer - which uses the path tracing light grid
	// That code is private inside engine, so needed to copy it into plugin
	void SetPathTracingLightParameters(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const bool bUseAtmosphere,
		// output args
		FPathTracingSkylight* SkylightParameters,
		FPathTracingLightGrid* LightGridParameters,
		uint32* SceneVisibleLightCount,
		uint32* SceneLightCount,
		FRDGBufferSRVRef* SceneLights
	);
}
