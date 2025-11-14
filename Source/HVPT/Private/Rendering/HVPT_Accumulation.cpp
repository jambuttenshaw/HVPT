#include "HVPTViewExtension.h"

#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"
#include "ScenePrivate.h"

#include "HVPT.h"
#include "HVPTViewState.h"

static TAutoConsoleVariable<int32> CVarHVPTAccumulatePauseAfter(
	TEXT("r.HVPT.Accumulate.PauseAfter"),
	-1,
	TEXT("The number of samples to stop accumulating after."),
	ECVF_RenderThreadSafe
);


class FHVPT_AccumulateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_AccumulateCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_AccumulateCS, FGlobalShader)

	class FPauseAccumulation : SHADER_PERMUTATION_BOOL("PAUSE_ACCUMULATION");
	using FPermutationDomain = TShaderPermutationDomain<FPauseAccumulation>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(int, NumAccumulatedSamples)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWTemporalAccumulationTexture_Hi)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWTemporalAccumulationTexture_Lo)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWFeatureTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return HVPT::DoesPlatformSupportHVPT(Parameters.Platform)
			&& IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment
	)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static uint32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FHVPT_AccumulateCS, "/Plugin/HVPT/Private/Accumulation.usf", "HVPT_AccumulateCS", SF_Compute);


void HVPT::Accumulate(
	FRDGBuilder& GraphBuilder, const FViewInfo& ViewInfo, FHVPTViewState& State
)
{
	int32 PauseAfter = CVarHVPTAccumulatePauseAfter.GetValueOnRenderThread();
	bool bPause = PauseAfter > 0 && State.AccumulatedSampleCount > static_cast<uint32>(PauseAfter);

	FHVPT_AccumulateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_AccumulateCS::FParameters>();

	PassParameters->View = ViewInfo.ViewUniformBuffer;

	PassParameters->NumAccumulatedSamples = bPause ? PauseAfter : State.AccumulatedSampleCount;

	PassParameters->RWTemporalAccumulationTexture_Hi = GraphBuilder.CreateUAV(State.TemporalAccumulationTexture_Hi);
	PassParameters->RWTemporalAccumulationTexture_Lo = GraphBuilder.CreateUAV(State.TemporalAccumulationTexture_Lo);
	PassParameters->RWRadianceTexture = GraphBuilder.CreateUAV(State.RadianceTexture);
	PassParameters->RWFeatureTexture = GraphBuilder.CreateUAV(State.FeatureTexture);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ViewInfo.FeatureLevel);

	FHVPT_AccumulateCS::FPermutationDomain Permutation;
	Permutation.Set<FHVPT_AccumulateCS::FPauseAccumulation>(bPause);
	TShaderMapRef<FHVPT_AccumulateCS> ComputeShader(ShaderMap, Permutation);

	FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(ViewInfo.ViewRect.Size(), FHVPT_AccumulateCS::GetThreadGroupSize2D());

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HVPT_AccumulateCS"),
		ERDGPassFlags::Compute,
		ComputeShader,
		PassParameters,
		GroupCount
	);
}
