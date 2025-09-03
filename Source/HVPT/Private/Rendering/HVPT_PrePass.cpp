#include "HVPTViewExtension.h"

#include "RenderGraphBuilder.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

#include "HVPTViewState.h"
#include "Helpers.h"


class FHVPT_PrePassPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_PrePassPS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_PrePassPS, FGlobalShader);

	class FWriteGBuffer : SHADER_PERMUTATION_BOOL("WRITE_GBUFFER");
	class FDensityGradientAsNormal : SHADER_PERMUTATION_BOOL("DENSITY_GRADIENT_NORMAL");
	class FTransmittanceMode : SHADER_PERMUTATION_INT("TRANSMITTANCE_MODE", 2);
	class FStochasticGBufferWrites : SHADER_PERMUTATION_BOOL("STOCHASTIC_GBUFFER_WRITES");
	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	class FDebugVisualizeVelocity : SHADER_PERMUTATION_BOOL("DEBUG_VISUALIZE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteGBuffer,
		FDensityGradientAsNormal,
		FTransmittanceMode,
		FStochasticGBufferWrites,
		FWriteVelocity,
		FDebugVisualizeVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepthTexture_Copy)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTOrthoGridUniformBufferParameters, HVPT_OrthoGrid)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHVPTFrustumGridUniformBufferParameters, HVPT_FrustumGrid)

		// Random init parameters
		SHADER_PARAMETER(uint32, TemporalSeed)

		SHADER_PARAMETER(float, Sharpness)

		SHADER_PARAMETER(float, FogComposition_OpticalDepth)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<half2>, RWFeatureTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWVelocityTexture)

		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewPort)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, OutputViewPort)

		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment
	)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// We need to set these so that the shader generation utils will set up the GBuffer writing defines
		// TODO: This causes all sorts of problems if also using substrate
		// TODO: because the render target layout is different
		OutEnvironment.SetDefine(TEXT("IS_BASE_PASS"), true);
		OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_SOLID"), true);
		OutEnvironment.SetDefine(TEXT("MATERIAL_SHADINGMODEL_UNLIT"), true);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_PrePassPS, "/Plugin/HVPT/Private/PrePass.usf", "HVPT_PrePassPS", SF_Pixel);


void HVPT::RenderPrePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	const FSceneTextures& SceneTextures,
	FHVPTViewState& State
)
{
	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: Pre-Pass");

	FHVPT_PrePassPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_PrePassPS::FParameters>();

	PassParameters->View = ViewInfo.ViewUniformBuffer;
	PassParameters->SceneDepthTexture_Copy = GraphBuilder.CreateSRV(State.DepthBufferCopy);

	PassParameters->HVPT_OrthoGrid = State.OrthoGridUniformBuffer;
	PassParameters->HVPT_FrustumGrid= State.FrustumGridUniformBuffer;

	uint32 FrameIndex = ViewInfo.ViewState ? ViewInfo.ViewState->FrameIndex : 0;
	PassParameters->TemporalSeed = HVPT::GetFreezeTemporalSeed() ? 0 : FrameIndex;

	PassParameters->Sharpness = HVPT::GetSharpness();

	PassParameters->FogComposition_OpticalDepth = HVPT::GetFogCompositingOpticalDepthThreshold();

	PassParameters->RWFeatureTexture = GraphBuilder.CreateUAV(State.FeatureTexture, ERDGUnorderedAccessViewFlags::None, PF_G16R16F);
	PassParameters->RWVelocityTexture = GraphBuilder.CreateUAV(SceneTextures.Velocity);

	// Get GBuffer
	TStaticArray<FTextureRenderTargetBinding, MaxSimultaneousRenderTargets> RenderTargetTextures;
	uint32 RenderTargetTextureCount = SceneTextures.GetGBufferRenderTargets(RenderTargetTextures);
	TArrayView<FTextureRenderTargetBinding> RenderTargetTexturesView(RenderTargetTextures.GetData(), RenderTargetTextureCount);
	PassParameters->RenderTargets = GetRenderTargetBindings(ERenderTargetLoadAction::ELoad, RenderTargetTexturesView);

	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop
	);

	FHVPT_PrePassPS::FPermutationDomain Permutation;
	Permutation.Set<FHVPT_PrePassPS::FWriteGBuffer>(HVPT::ShouldWriteGBuffer());
	Permutation.Set<FHVPT_PrePassPS::FDensityGradientAsNormal>(HVPT::ShouldWriteNormals());
	Permutation.Set<FHVPT_PrePassPS::FTransmittanceMode>(HVPT::GetTransmittanceMode());
	Permutation.Set<FHVPT_PrePassPS::FStochasticGBufferWrites>(HVPT::GetStochasticGBufferWrites());
	Permutation.Set<FHVPT_PrePassPS::FWriteVelocity>(HVPT::ShouldWriteVelocity());
	Permutation.Set<FHVPT_PrePassPS::FDebugVisualizeVelocity>(HVPT::GetVisualizeVelocity());

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ViewInfo.FeatureLevel);
	TShaderMapRef<FScreenPassVS> VertexShader(ShaderMap);
	TShaderMapRef<FHVPT_PrePassPS> PixelShader(ShaderMap, Permutation);

	FScreenPassTextureViewport ViewPort(ViewInfo.ViewRect.Size());

	PassParameters->InputViewPort = GetScreenPassTextureViewportParameters(ViewPort);
	PassParameters->OutputViewPort = GetScreenPassTextureViewportParameters(ViewPort);

	using FCompositionDepthStencilState = TStaticDepthStencilState<true, CF_Greater>;

	AddDrawScreenPass(
		GraphBuilder,
		RDG_EVENT_NAME("HVPT_PrePassPS"),
		ViewInfo,
		ViewPort,
		ViewPort,
		VertexShader,
		PixelShader,
		FCompositionDepthStencilState::GetRHI(),
		PassParameters);
}
