#include "HVPTViewExtension.h"

#include "RenderGraphBuilder.h"
#include "SceneRendering.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SceneTextureParameters.h"
#include "GlobalShader.h"
#include "ShaderCompilerCore.h"
#include "ScenePrivate.h"

#include "Helpers.h"
#include "HVPTViewState.h"
#include "SceneCore.h"


class FHVPT_CompositeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_CompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_CompositeCS, FGlobalShader);

	class FApplyFog : SHADER_PERMUTATION_BOOL("APPLY_VOLUMETRIC_FOG");
	using FPermutationDomain = TShaderPermutationDomain<FApplyFog>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER(int, ApplyVolumetricFog)
		SHADER_PARAMETER(float, VolumetricFogStartDistance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, IntegratedLightScattering)
		SHADER_PARAMETER_SAMPLER(SamplerState, IntegratedLightScatteringSampler)

		// Volume data
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float3>, RadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, FeatureTexture)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWColorTexture)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters
	)
	{
		// Apply conditional project settings for Heterogeneous volumes?
		return DoesPlatformSupportHeterogeneousVolumes(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment
	)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());

		// This shader takes a very long time to compile with FXC, so we pre-compile it with DXC first and then forward the optimized HLSL to FXC.
		OutEnvironment.CompilerFlags.Add(CFLAG_PrecompileWithDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);

		OutEnvironment.SetDefine(TEXT("GET_PRIMITIVE_DATA_OVERRIDE"), 1);
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_CompositeCS, "/Plugin/HVPT/Private/Composite.usf", "HVPT_CompositeCS", SF_Compute);


void HVPT::Composite(
	FRDGBuilder& GraphBuilder, const FScene& Scene, const FViewInfo& ViewInfo, const FSceneTextures& SceneTextures, FHVPTViewState& State
)
{
	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: Composite");

	const FExponentialHeightFogSceneInfo& FogInfo = Scene.ExponentialFogs[0];
	bool bEnableVolumetricFog = FogInfo.bEnableVolumetricFog
							&& ViewInfo.VolumetricFogResources.IntegratedLightScatteringTexture
							&& HVPT::GetFogCompositingMode() != EFogCompositionMode::Disabled;

	FHVPT_CompositeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_CompositeCS::FParameters>();
	PassParameters->View = ViewInfo.ViewUniformBuffer;
	PassParameters->SceneTextures = HVPT::Private::GetSceneTextureParameters(GraphBuilder, SceneTextures);

	PassParameters->ApplyVolumetricFog = FogInfo.bEnableVolumetricFog;
	PassParameters->VolumetricFogStartDistance = FogInfo.VolumetricFogStartDistance;
	PassParameters->IntegratedLightScattering = GraphBuilder.CreateSRV(
		bEnableVolumetricFog ? ViewInfo.VolumetricFogResources.IntegratedLightScatteringTexture : GSystemTextures.GetVolumetricBlackDummy(GraphBuilder)
	);
	PassParameters->IntegratedLightScatteringSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	PassParameters->RadianceTexture = GraphBuilder.CreateSRV(State.RadianceTexture);
	PassParameters->FeatureTexture = GraphBuilder.CreateSRV(State.FeatureTexture);

	PassParameters->RWColorTexture = GraphBuilder.CreateUAV(SceneTextures.Color.Target);

	FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(ViewInfo.ViewRect.Size(), FHVPT_CompositeCS::GetThreadGroupSize2D());

	FHVPT_CompositeCS::FPermutationDomain Permutation;
	Permutation.Set<FHVPT_CompositeCS::FApplyFog>(bEnableVolumetricFog);
	TShaderRef<FHVPT_CompositeCS> ComputeShader = ViewInfo.ShaderMap->GetShader<FHVPT_CompositeCS>(Permutation);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HVPT_Composite"),
		ComputeShader,
		PassParameters,
		GroupCount
	);
}
