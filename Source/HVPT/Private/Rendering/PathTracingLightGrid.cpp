#include "PathTracingLightGrid.h"
#include "EnvironmentComponentsFlags.h"
#include "GenerateMips.h"
#include "Helpers.h"

#include "PathTracing.h"
#include "PathTracingDefinitions.h"
#include "RayTracingTypes.h"
#include "RayTracing/RayTracingLighting.h"
#include "RenderGraphBuilder.h"
#include "ScenePrivate.h"
#include "ReflectionEnvironment.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "SceneProxies/SkyLightSceneProxy.h"
#endif


// All contents of this file have been copied from various parts of the engine, because they are private and inaccessible to plugins
// Some modifications were required to avoid linker errors but the functionality is nearly identical


#if RHI_RAYTRACING

class FHVPT_PathTracingSkylightPrepareCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_PathTracingSkylightPrepareCS)
	SHADER_USE_PARAMETER_STRUCT(FHVPT_PathTracingSkylightPrepareCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		// NOTE: skylight code is shared with RT passes
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		//OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), FComputeShaderUtils::kGolden2DGroupSize);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, SkyLightCubemap0)
		SHADER_PARAMETER_TEXTURE(TextureCube, SkyLightCubemap1)
		SHADER_PARAMETER_SAMPLER(SamplerState, SkyLightCubemapSampler0)
		SHADER_PARAMETER_SAMPLER(SamplerState, SkyLightCubemapSampler1)
		SHADER_PARAMETER(float, SkylightBlendFactor)
		SHADER_PARAMETER(float, SkylightInvResolution)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, SkylightTextureOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, SkylightTexturePdf)
		SHADER_PARAMETER(FVector3f, SkyColor)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_SHADER_TYPE(, FHVPT_PathTracingSkylightPrepareCS, TEXT("/Plugin/HVPT/Private/PathTracingLightGrid.usf"), TEXT("PathTracingSkylightPrepareCS"), SF_Compute);


class FHVPT_PathTracingBuildLightGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHVPT_PathTracingBuildLightGridCS)
	SHADER_USE_PARAMETER_STRUCT(FHVPT_PathTracingBuildLightGridCS, FGlobalShader)

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Required intersection utils didn't exist in 5.5, so we will define them ourselves
		// In more recent engine versions switch back to built in functions
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#else
		OutEnvironment.SetDefine(TEXT("DEFINE_INTERSECTION_UTILS"), true);
#endif

		OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), FComputeShaderUtils::kGolden2DGroupSize);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPathTracingLight>, SceneLights)
		SHADER_PARAMETER(uint32, SceneLightCount)
		SHADER_PARAMETER_STRUCT_INCLUDE(FPathTracingLightGrid, LightGridParameters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint>, RWLightGrid)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWLightGridData)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_SHADER_TYPE(, FHVPT_PathTracingBuildLightGridCS, TEXT("/Plugin/HVPT/Private/PathTracingLightGrid.usf"), TEXT("PathTracingBuildLightGridCS"), SF_Compute);


RDG_REGISTER_BLACKBOARD_STRUCT(FPathTracingSkylight)


namespace HVPT::Private {

uint32 PackRG16(float In0, float In1)
{
	return uint32(FFloat16(In0).Encoded) | (uint32(FFloat16(In1).Encoded) << 16);
}

FBox3f GetPointLightBounds(const FVector3f& Center, float Radius)
{
	const FVector3f R(Radius, Radius, Radius);
	return FBox3f(Center - R, Center + R);
}

FBox3f GetSpotLightBounds(const FVector3f& Center, const FVector3f& Normal, float Radius, float CosOuter)
{
	// box around ray from light center to tip of the cone
	const FVector3f Tip = Center + Normal * Radius;
	FVector3f Lo = FVector3f::Min(Center, Tip);
	FVector3f Hi = FVector3f::Max(Center, Tip);

	const float SinOuter = FMath::Sqrt(1.0f - CosOuter * CosOuter);

	// expand by disc around the farthest part of the cone
	const FVector3f Disc = FVector3f(
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.X * Normal.X, 0.0f, 1.0f)),
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.Y * Normal.Y, 0.0f, 1.0f)),
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.Z * Normal.Z, 0.0f, 1.0f))
	);
	Lo = FVector3f::Min(Lo, Center + Radius * (Normal * CosOuter - Disc * SinOuter));
	Hi = FVector3f::Max(Hi, Center + Radius * (Normal * CosOuter + Disc * SinOuter));

	// Check if any of the coordinate axis points lie inside the cone and include them if they do
	// This is the only case which is not captured by the AABB above
	const FVector3f E = FVector3f(
		FMath::Abs(Normal.X) > CosOuter ? Center.X + copysignf(Radius, Normal.X) : Center.X,
		FMath::Abs(Normal.Y) > CosOuter ? Center.Y + copysignf(Radius, Normal.Y) : Center.Y,
		FMath::Abs(Normal.Z) > CosOuter ? Center.Z + copysignf(Radius, Normal.Z) : Center.Z
	);
	Lo = FVector3f::Min(Lo, E);
	Hi = FVector3f::Max(Hi, E);
	return FBox3f(Lo, Hi);
}

FBox3f GetRectLightBounds(const FVector3f& Center, const FVector3f& Normal, const FVector3f& Tangent, float HalfWidth, float HalfHeight, float Radius, float BarnCos, float BarnLen)
{
	const FVector3f Corner = FVector3f(
		copysignf(Radius, Normal.X),
		copysignf(Radius, Normal.Y),
		copysignf(Radius, Normal.Z)
	);
	const FVector3f Disc = FVector3f(
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.X * Normal.X, 0.0f, 1.0f)),
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.Y * Normal.Y, 0.0f, 1.0f)),
		FMath::Sqrt(FMath::Clamp(1.0f - Normal.Z * Normal.Z, 0.0f, 1.0f))
	);

	// rect bbox is the bbox of the disc + furthest corner of the Radius sized box in the direction of the normal
	FVector3f Lo = FVector3f::Min(Center + Corner, Center - Radius * Disc);
	FVector3f Hi = FVector3f::Max(Center + Corner, Center + Radius * Disc);

	// Take into account barndoor frustum if enabled
	if (BarnCos > 0.035f)
	{
		const FVector3f dPdv = Tangent;
		const FVector3f dPdu = Normal.Cross(Tangent);
		const float BarnSin = FMath::Sqrt(1 - BarnCos * BarnCos);

		FVector3f BoundingPlane = FVector3f(
			2 * HalfWidth + BarnLen * BarnSin,
			2 * HalfHeight + BarnLen * BarnSin,
			BarnLen * BarnCos
		);
		FVector3f BLo = Center, BHi = Center;
		// loop through 9 points to get extremes of the "rounded" pyramid defined by the barndoor penumbra + radius
		for (int Dy = -1; Dy <= 1; Dy++)
			for (int Dx = -1; Dx <= 1; Dx++)
			{
				// Get point on rectangle
				const FVector3f Rxy = Center + Dx * HalfWidth * dPdu + Dy * HalfHeight * dPdv;
				// Get penumbra plane vector, normalize it and scale to edge of the sphere (roughly, since we aren't starting from the center)
				const FVector3f Bxy = Rxy + (Dx * BoundingPlane.X * dPdu + Dy * BoundingPlane.Y * dPdv + BoundingPlane.Z * Normal).GetUnsafeNormal() * Radius;
				BLo = FVector3f::Min(BLo, Rxy); BHi = FVector3f::Max(BHi, Rxy);
				BLo = FVector3f::Min(BLo, Bxy); BHi = FVector3f::Max(BHi, Bxy);
			}

		// Include "axis" points if they lie inside the barndoor penumbra (similar to spot light test above, but the apex is behind the Center and different in X and Y due to the rectangle size)
		// A 2D visualization of this is here: https://www.desmos.com/calculator/15zh9boeqz
		const float TanAlphaX = BoundingPlane.X / BoundingPlane.Z, CosAlphaX = FMath::InvSqrt(1 + TanAlphaX * TanAlphaX), ApexX = HalfWidth / TanAlphaX;
		const float TanAlphaY = BoundingPlane.Y / BoundingPlane.Z, CosAlphaY = FMath::InvSqrt(1 + TanAlphaY * TanAlphaY), ApexY = HalfHeight / TanAlphaY;

		// Take the extreme point along each axis, then rotate it to local space
		const FVector3f Px = FVector3f(dPdu.X * Corner.X, dPdv.X * Corner.X, Normal.X * Corner.X);
		const FVector3f Py = FVector3f(dPdu.Y * Corner.Y, dPdv.Y * Corner.Y, Normal.Y * Corner.Y);
		const FVector3f Pz = FVector3f(dPdu.Z * Corner.Z, dPdv.Z * Corner.Z, Normal.Z * Corner.Z);

		// Now - check if dot product betwen this point and normal lies within the cone
		// We do two 2D cone tests as we have a different cone in X and Y
		// This is the analog of the simpler implementation possible for spotlights above
		if ((Px.Z - ApexX) > FVector2f(Px.X, Px.Z - ApexX).Length() * CosAlphaX &&
			(Px.Z - ApexY) > FVector2f(Px.Y, Px.Z - ApexY).Length() * CosAlphaY)
		{
			BLo.X = FMath::Min(BLo.X, Center.X + Corner.X);
			BHi.X = FMath::Max(BHi.X, Center.X + Corner.X);
		}
		if ((Py.Z - ApexX) > FVector2f(Py.X, Py.Z - ApexX).Length() * CosAlphaX &&
			(Py.Z - ApexY) > FVector2f(Py.Y, Py.Z - ApexY).Length() * CosAlphaY)
		{
			BLo.Y = FMath::Min(BLo.Y, Center.Y + Corner.Y);
			BHi.Y = FMath::Max(BHi.Y, Center.Y + Corner.Y);
		}
		if ((Pz.Z - ApexX) > FVector2f(Pz.X, Pz.Z - ApexX).Length() * CosAlphaX &&
			(Pz.Z - ApexY) > FVector2f(Pz.Y, Pz.Z - ApexY).Length() * CosAlphaY)
		{
			BLo.Z = FMath::Min(BLo.Z, Center.Z + Corner.Z);
			BHi.Z = FMath::Max(BHi.Z, Center.Z + Corner.Z);
		}
		// Now clip the new BBox against the old (conservative one) for the entire half-space
		Lo = FVector3f::Max(Lo, BLo);
		Hi = FVector3f::Min(Hi, BHi);
	}
	return FBox3f(Lo, Hi);
}


bool CanSampleSkyLightRealTimeCaptureData(const FScene* Scene)
{
	// We need a sky light, with bRealTimeCaptureEnabled (only true if supported by the platform settings) and if the captured data is ready.
	return Scene->SkyLight && Scene->SkyLight->bRealTimeCaptureEnabled && Scene->ConvolvedSkyRenderTargetReadyIndex >= 0;
}

void SetupReflectionUniformParameters(FRDGBuilder& GraphBuilder, const FViewInfo& View, FReflectionUniformParameters& OutParameters)
{
	FTextureRHIRef SkyLightTextureResource = nullptr;
	FSamplerStateRHIRef SkyLightCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
	FTexture* SkyLightBlendDestinationTextureResource = GBlackTextureCube;
	float ApplySkyLightMask = 0;
	float BlendFraction = 0;
	bool bSkyLightIsDynamic = false;
	float SkyAverageBrightness = 1.0f;

	const bool bApplySkyLight = View.Family->EngineShowFlags.SkyLighting;
	const FScene* Scene = (const FScene*)View.Family->Scene;
	ERDGTextureFlags SkyLightTextureFlags = ERDGTextureFlags::None;

	if (Scene
		&& Scene->SkyLight
		&& (Scene->SkyLight->ProcessedTexture || CanSampleSkyLightRealTimeCaptureData(Scene))
		&& bApplySkyLight)
	{
		const FSkyLightSceneProxy& SkyLight = *Scene->SkyLight;

		if (CanSampleSkyLightRealTimeCaptureData(Scene))
		{
			// Cannot blend with this capture mode as of today.
			SkyLightTextureResource = Scene->ConvolvedSkyRenderTarget[Scene->ConvolvedSkyRenderTargetReadyIndex]->GetRHI();
		}
		else if (SkyLight.ProcessedTexture)
		{
			SkyLightTextureResource = SkyLight.ProcessedTexture->TextureRHI;
			SkyLightCubemapSampler = SkyLight.ProcessedTexture->SamplerStateRHI;
			BlendFraction = SkyLight.BlendFraction;

			if (SkyLight.BlendFraction > 0.0f && SkyLight.BlendDestinationProcessedTexture)
			{
				if (SkyLight.BlendFraction < 1.0f)
				{
					SkyLightBlendDestinationTextureResource = SkyLight.BlendDestinationProcessedTexture;
				}
				else
				{
					SkyLightTextureResource = SkyLight.BlendDestinationProcessedTexture->TextureRHI;
					SkyLightCubemapSampler = SkyLight.ProcessedTexture->SamplerStateRHI;
					BlendFraction = 0;
				}
			}

			SkyLightTextureFlags = ERDGTextureFlags::SkipTracking;
		}

		ApplySkyLightMask = 1;
		bSkyLightIsDynamic = !SkyLight.bHasStaticLighting && !SkyLight.bWantsStaticShadowing;
		SkyAverageBrightness = SkyLight.AverageBrightness;
	}

	FRDGTextureRef SkyLightTexture = nullptr;
	if (SkyLightTextureResource)
	{
		SkyLightTexture = RegisterExternalTexture(GraphBuilder, SkyLightTextureResource, TEXT("SkyLightTexture"), SkyLightTextureFlags);
	}
	else
	{
		SkyLightTexture = GSystemTextures.GetCubeBlackDummy(GraphBuilder);
	}

	const int32 CubemapWidth = SkyLightTexture->Desc.Extent.X;
	const float SkyMipCount = FMath::Log2(static_cast<float>(CubemapWidth)) + 1.0f;

	OutParameters.SkyLightCubemap = SkyLightTexture;
	OutParameters.SkyLightCubemapSampler = SkyLightCubemapSampler;
	OutParameters.SkyLightBlendDestinationCubemap = SkyLightBlendDestinationTextureResource->TextureRHI;
	OutParameters.SkyLightBlendDestinationCubemapSampler = SkyLightBlendDestinationTextureResource->SamplerStateRHI;
	OutParameters.SkyLightParameters = FVector4f(SkyMipCount - 1.0f, ApplySkyLightMask, bSkyLightIsDynamic ? 1.0f : 0.0f, BlendFraction);

	// Note: GBlackCubeArrayTexture has an alpha of 0, which is needed to represent invalid data so the sky cubemap can still be applied
	FRHITexture* CubeArrayTexture = (SupportsTextureCubeArray(View.FeatureLevel)) ? GBlackCubeArrayTexture->TextureRHI : GBlackTextureCube->TextureRHI;

	if (View.Family->EngineShowFlags.ReflectionEnvironment
		&& SupportsTextureCubeArray(View.FeatureLevel)
		&& Scene
		&& Scene->ReflectionSceneData.CubemapArray.IsValid()
		&& Scene->ReflectionSceneData.RegisteredReflectionCaptures.Num())
	{
		CubeArrayTexture = Scene->ReflectionSceneData.CubemapArray.GetRenderTarget()->GetRHI();
	}

	OutParameters.ReflectionCubemap = CubeArrayTexture;
	OutParameters.ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	OutParameters.PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRHI();
	OutParameters.PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
}

void PrepareSkyTexture_Internal(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FReflectionUniformParameters& Parameters,
	uint32 Size,
	FLinearColor SkyColor,

	// Out
	FRDGTextureRef& SkylightTexture,
	FRDGTextureRef& SkylightPdf,
	float& SkylightInvResolution,
	int32& SkylightMipCount
)
{
	FRDGTextureDesc SkylightTextureDesc = FRDGTextureDesc::Create2D(
		FIntPoint(Size, Size),
		PF_A32B32G32R32F, // Must use float as CubeMap * Color could have float range (could use half if we didn't include Color in the map)
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	SkylightTexture = GraphBuilder.CreateTexture(SkylightTextureDesc, TEXT("PathTracer.Skylight"), ERDGTextureFlags::None);

	FRDGTextureDesc SkylightPdfDesc = FRDGTextureDesc::Create2D(
		FIntPoint(Size, Size),
		PF_R32_FLOAT, // Must use float as CubeMap * Color could have float range (could use half if we didn't include Color in the map)
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV,
		FMath::CeilLogTwo(Size) + 1);

	SkylightPdf = GraphBuilder.CreateTexture(SkylightPdfDesc, TEXT("PathTracer.SkylightPdf"), ERDGTextureFlags::None);

	SkylightInvResolution = 1.0f / Size;
	SkylightMipCount = SkylightPdfDesc.NumMips;

	// run a simple compute shader to sample the cubemap and prep the top level of the mipmap hierarchy
	{
		TShaderMapRef<FHVPT_PathTracingSkylightPrepareCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FHVPT_PathTracingSkylightPrepareCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_PathTracingSkylightPrepareCS::FParameters>();
		PassParameters->SkyColor = FVector3f(SkyColor.R, SkyColor.G, SkyColor.B);
		PassParameters->SkyLightCubemap0 = Parameters.SkyLightCubemap;
		PassParameters->SkyLightCubemap1 = Parameters.SkyLightBlendDestinationCubemap;
		PassParameters->SkyLightCubemapSampler0 = Parameters.SkyLightCubemapSampler;
		PassParameters->SkyLightCubemapSampler1 = Parameters.SkyLightBlendDestinationCubemapSampler;
		PassParameters->SkylightBlendFactor = Parameters.SkyLightParameters.W;
		PassParameters->SkylightInvResolution = SkylightInvResolution;
		PassParameters->SkylightTextureOutput = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SkylightTexture, 0));
		PassParameters->SkylightTexturePdf = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SkylightPdf, 0));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SkylightPrepare"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntPoint(Size, Size), FComputeShaderUtils::kGolden2DGroupSize));
	}
	FGenerateMips::ExecuteCompute(GraphBuilder, FeatureLevel, SkylightPdf, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
}

bool PrepareSkyTexture(FRDGBuilder& GraphBuilder, FScene* Scene, const FViewInfo& View, bool SkylightEnabled, FPathTracingSkylight* SkylightParameters)
{
	SkylightParameters->SkylightTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FReflectionUniformParameters Parameters;
	::HVPT::Private::SetupReflectionUniformParameters(GraphBuilder, View, Parameters);
	if (!SkylightEnabled || !(Parameters.SkyLightParameters.Y > 0))
	{
		// textures not ready, or skylight not active
		// just put in a placeholder
		SkylightParameters->SkylightTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		SkylightParameters->SkylightPdf = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		SkylightParameters->SkylightInvResolution = 0;
		SkylightParameters->SkylightMipCount = 0;
		return false;
	}

	// the sky is actually enabled, lets see if someone already made use of it for this frame
	const FPathTracingSkylight* PreviousSkylightParameters = GraphBuilder.Blackboard.Get<FPathTracingSkylight>();
	if (PreviousSkylightParameters != nullptr)
	{
		*SkylightParameters = *PreviousSkylightParameters;
		return true;
	}

	// should we remember the skylight prep for the next frame?
	const bool IsSkylightCachingEnabled = true;
	FLinearColor SkyColor = Scene->SkyLight->GetEffectiveLightColor();
	const bool bSkylightColorChanged = SkyColor != Scene->PathTracingSkylightColor;
	if (!IsSkylightCachingEnabled || bSkylightColorChanged)
	{
		// we don't want any caching (or the light color changed)
		// release what we might have been holding onto so we get the right texture for this frame
		Scene->PathTracingSkylightTexture.SafeRelease();
		Scene->PathTracingSkylightPdf.SafeRelease();
	}

	if (Scene->PathTracingSkylightTexture.IsValid() &&
		Scene->PathTracingSkylightPdf.IsValid())
	{
		// we already have a valid texture and pdf, just re-use them!
		// it is the responsability of code that may invalidate the contents to reset these pointers
		SkylightParameters->SkylightTexture = GraphBuilder.RegisterExternalTexture(Scene->PathTracingSkylightTexture, TEXT("PathTracer.Skylight"));
		SkylightParameters->SkylightPdf = GraphBuilder.RegisterExternalTexture(Scene->PathTracingSkylightPdf, TEXT("PathTracer.SkylightPdf"));
		SkylightParameters->SkylightInvResolution = 1.0f / SkylightParameters->SkylightTexture->Desc.GetSize().X;
		SkylightParameters->SkylightMipCount = SkylightParameters->SkylightPdf->Desc.NumMips;
		return true;
	}
	RDG_EVENT_SCOPE(GraphBuilder, "Path Tracing SkylightPrepare");
	Scene->PathTracingSkylightColor = SkyColor;
	// since we are resampled into an octahedral layout, we multiply the cubemap resolution by 2 to get roughly the same number of texels
	uint32 Size = FMath::RoundUpToPowerOfTwo(2 * Scene->SkyLight->CaptureCubeMapResolution);

	RDG_GPU_MASK_SCOPE(GraphBuilder,
		IsSkylightCachingEnabled ? FRHIGPUMask::All() : GraphBuilder.RHICmdList.GetGPUMask());

	::HVPT::Private::PrepareSkyTexture_Internal(
		GraphBuilder,
		View.FeatureLevel,
		Parameters,
		Size,
		SkyColor,
		// Out
		SkylightParameters->SkylightTexture,
		SkylightParameters->SkylightPdf,
		SkylightParameters->SkylightInvResolution,
		SkylightParameters->SkylightMipCount
	);

	// hang onto these for next time (if caching is enabled)
	if (IsSkylightCachingEnabled)
	{
		GraphBuilder.QueueTextureExtraction(SkylightParameters->SkylightTexture, &Scene->PathTracingSkylightTexture);
		GraphBuilder.QueueTextureExtraction(SkylightParameters->SkylightPdf, &Scene->PathTracingSkylightPdf);
	}

	// remember the skylight parameters for future passes within this frame
	GraphBuilder.Blackboard.Create<FPathTracingSkylight>() = *SkylightParameters;

	return true;
}

void PrepareLightGrid(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel, FPathTracingLightGrid* LightGridParameters, const FPathTracingLight* Lights, uint32 NumLights, uint32 NumInfiniteLights, FRDGBufferSRV* LightsSRV)
{
	const float Inf = std::numeric_limits<float>::infinity();
	LightGridParameters->SceneInfiniteLightCount = NumInfiniteLights;
	LightGridParameters->SceneLightsTranslatedBoundMin = FVector3f(+Inf, +Inf, +Inf);
	LightGridParameters->SceneLightsTranslatedBoundMax = FVector3f(-Inf, -Inf, -Inf);
	LightGridParameters->LightGrid = nullptr;
	LightGridParameters->LightGridData = nullptr;

	int NumFiniteLights = NumLights - NumInfiniteLights;
	// if we have some finite lights -- build a light grid
	if (NumFiniteLights > 0)
	{
		// get bounding box of all finite lights
		const FPathTracingLight* FiniteLights = Lights + NumInfiniteLights;
		for (int Index = 0; Index < NumFiniteLights; Index++)
		{
			const FPathTracingLight& Light = FiniteLights[Index];
			FBox3f Box;

			const float Radius = 1.0f / Light.Attenuation;
			const FVector3f Center = Light.TranslatedWorldPosition;
			const FVector3f Normal = Light.Normal;
			switch (Light.Flags & PATHTRACER_FLAG_TYPE_MASK)
			{
			case PATHTRACING_LIGHT_POINT:
			{
				Box = GetPointLightBounds(Center, Radius);
				break;
			}
			case PATHTRACING_LIGHT_SPOT:
			{
				Box = GetSpotLightBounds(Center, Normal, Radius, Light.Shaping.X);
				break;
			}
			case PATHTRACING_LIGHT_RECT:
			{
				Box = GetRectLightBounds(Center, Normal, Light.Tangent, Light.Dimensions.X * 0.5f, Light.Dimensions.Y * 0.5f, Radius, Light.Shaping.X, Light.Shaping.Y);
				break;
			}
			default:
			{
				// non-finite lights should not appear in this case
				checkNoEntry();
				break;
			}
			}
			LightGridParameters->SceneLightsTranslatedBoundMin = FVector3f::Min(LightGridParameters->SceneLightsTranslatedBoundMin, Box.Min);
			LightGridParameters->SceneLightsTranslatedBoundMax = FVector3f::Max(LightGridParameters->SceneLightsTranslatedBoundMax, Box.Max);
		}

		const uint32 Resolution = 256;
		const uint32 MaxCount = 128;

		LightGridParameters->LightGridResolution = Resolution;
		LightGridParameters->LightGridMaxCount = MaxCount;

		LightGridParameters->LightGridAxis = -1;
		FHVPT_PathTracingBuildLightGridCS::FParameters* LightGridPassParameters = GraphBuilder.AllocParameters<FHVPT_PathTracingBuildLightGridCS::FParameters>();

		FRDGTextureDesc LightGridDesc = FRDGTextureDesc::Create2DArray(
			FIntPoint(Resolution, Resolution),
			PF_R32_UINT,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV, 3);

		// jhoerner TODO 9/30/2022: Hack to work around MGPU resource transition architectural bug in RDG.  Mask PathTracer.LightGrid texture
		// to only be present on current GPU.  The bug is that RDG batches transitions, but the execution of batched transitions uses the
		// GPU Mask of the current Pass that's executing, not the GPU Mask that's relevant to the Passes where a given resource is used.  This
		// causes an assert due to a mismatch in the expected transition state on a specific GPU, when an intermediate transition was skipped
		// on that GPU, due to the arbitrary nature of the GPU mask when a transition batch is flushed.  The hack works by removing the
		// resource from GPUs it's not actually used on, where the intermediate transition gets skipped.
		LightGridDesc.GPUMask = GraphBuilder.RHICmdList.GetGPUMask();

		FRDGTexture* LightGridTexture = GraphBuilder.CreateTexture(LightGridDesc, TEXT("PathTracer.LightGrid"), ERDGTextureFlags::None);
		LightGridPassParameters->RWLightGrid = GraphBuilder.CreateUAV(LightGridTexture);

		EPixelFormat LightGridDataFormat = PF_R32_UINT;
		size_t LightGridDataNumBytes = sizeof(uint32);
		if (NumLights <= (MAX_uint8 + 1))
		{
			LightGridDataFormat = PF_R8_UINT;
			LightGridDataNumBytes = sizeof(uint8);
		}
		else if (NumLights <= (MAX_uint16 + 1))
		{
			LightGridDataFormat = PF_R16_UINT;
			LightGridDataNumBytes = sizeof(uint16);
		}
		FRDGBufferDesc LightGridDataDesc = FRDGBufferDesc::CreateBufferDesc(LightGridDataNumBytes, 3 * MaxCount * Resolution * Resolution);
		FRDGBuffer* LightGridData = GraphBuilder.CreateBuffer(LightGridDataDesc, TEXT("PathTracer.LightGridData"));
		LightGridPassParameters->RWLightGridData = GraphBuilder.CreateUAV(LightGridData, LightGridDataFormat);
		LightGridPassParameters->LightGridParameters = *LightGridParameters;
		LightGridPassParameters->SceneLights = LightsSRV;
		LightGridPassParameters->SceneLightCount = NumLights;

		TShaderMapRef<FHVPT_PathTracingBuildLightGridCS> ComputeShader(GetGlobalShaderMap(FeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Light Grid Create (%u lights)", NumFiniteLights),
			ComputeShader,
			LightGridPassParameters,
			FComputeShaderUtils::GetGroupCount(FIntVector(Resolution, Resolution, 3), FIntVector(FComputeShaderUtils::kGolden2DGroupSize, FComputeShaderUtils::kGolden2DGroupSize, 1)));

		// hookup to the actual rendering pass
		LightGridParameters->LightGrid = LightGridTexture;
		LightGridParameters->LightGridData = GraphBuilder.CreateSRV(LightGridData, LightGridDataFormat);
	}
	else
	{
		// light grid is not needed - just hookup dummy data
		LightGridParameters->LightGridResolution = 0;
		LightGridParameters->LightGridMaxCount = 0;
		LightGridParameters->LightGridAxis = 0;
		LightGridParameters->LightGrid = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);
		FRDGBufferDesc LightGridDataDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
		FRDGBuffer* LightGridData = GraphBuilder.CreateBuffer(LightGridDataDesc, TEXT("PathTracer.LightGridData"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LightGridData, PF_R32_UINT), 0);
		LightGridParameters->LightGridData = GraphBuilder.CreateSRV(LightGridData, PF_R32_UINT);
	}
}

} // namespace HVPT::Private


void HVPT::Private::SetPathTracingLightParameters(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const bool bUseAtmosphere,
	// output args
	FPathTracingSkylight* SkylightParameters,
	FPathTracingLightGrid* LightGridParameters,
	uint32* SceneVisibleLightCount,
	uint32* SceneLightCount,
	FRDGBufferSRVRef* SceneLights
)
{
	check(SkylightParameters != nullptr);
	check(SceneVisibleLightCount != nullptr);
	check(SceneLightCount != nullptr);
	check(SceneLights != nullptr);
	*SceneVisibleLightCount = 0;

	FScene* Scene = View.Family->Scene->GetRenderScene();
	check(Scene != nullptr);

	// Lights
	uint32 MaxNumLights = 1 + Scene->Lights.Num(); // upper bound
	// Allocate from the graph builder so that we don't need to copy the data again when queuing the upload
	FPathTracingLight* Lights = (FPathTracingLight*)GraphBuilder.Alloc(sizeof(FPathTracingLight) * MaxNumLights, 16);
	uint32 NumLights = 0;

	// Prepend SkyLight to light buffer since it is not part of the regular light list
	// skylight should be excluded if we are using the reference atmosphere calculation (don't bother checking again if an atmosphere is present)
	const bool bEnableSkydome = !bUseAtmosphere;
	if (::HVPT::Private::PrepareSkyTexture(GraphBuilder, Scene, View, bEnableSkydome, SkylightParameters))
	{
		check(Scene->SkyLight != nullptr);
		FPathTracingLight& DestLight = Lights[NumLights++];
		DestLight.Color = FVector3f(1, 1, 1); // not used (it is folded into the importance table directly)
		DestLight.Flags = Scene->SkyLight->bTransmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
		DestLight.Flags |= PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
		DestLight.Flags |= PATHTRACING_LIGHT_SKY;
		DestLight.Flags |= Scene->SkyLight->bCastShadows ? PATHTRACER_FLAG_CAST_SHADOW_MASK : 0;
		DestLight.Flags |= Scene->SkyLight->bCastVolumetricShadow ? PATHTRACER_FLAG_CAST_VOL_SHADOW_MASK : 0;
		DestLight.DiffuseSpecularScale = ::HVPT::Private::PackRG16(1.f, 1.f);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		DestLight.IndirectLightingScale = Scene->SkyLight->IndirectLightingIntensity;
#endif
		DestLight.VolumetricScatteringIntensity = Scene->SkyLight->VolumetricScatteringIntensity;
		DestLight.IESAtlasIndex = INDEX_NONE;
		DestLight.MissShaderIndex = 0;
		if (Scene->SkyLight->bRealTimeCaptureEnabled && (View.SkyAtmosphereUniformShaderParameters == nullptr || !IsSkyAtmosphereHoldout(View.CachedViewUniformShaderParameters->EnvironmentComponentsFlags)))
		{
			// When using the realtime capture system, always make the skylight visible
			// because this is our only way of "seeing" the atmo/clouds at the moment
			// The one exception to this case is if the sky atmo has been marked as holdout.

			// Also allow seeing just the sky via a cvar for debugging purposes
			*SceneVisibleLightCount = 1;

			if (Scene->SkyLight->bRealTimeCaptureEnabled)
			{
				// NOTE: this color is already baked into the skylight texture so that importance sampling takes it into account, we pass it in here so that camera rays can factor it out
				// This is only for the realtime capture case, because otherwise (specified cube map case) we want the displayed texture and lighting to match
				DestLight.Color = FVector3f(Scene->SkyLight->GetEffectiveLightColor());
			}
		}
	}

	const FRayTracingLightFunctionMap* RayTracingLightFunctionMap = GraphBuilder.Blackboard.Get<FRayTracingLightFunctionMap>();

	// Add directional lights next (all lights with infinite bounds should come first)
	if (View.Family->EngineShowFlags.DirectionalLights)
	{
		for (const FLightSceneInfoCompact& Light : Scene->Lights)
		{
			ELightComponentType LightComponentType = (ELightComponentType)Light.LightSceneInfo->Proxy->GetLightType();

			if (LightComponentType != LightType_Directional)
			{
				continue;
			}

			FLightRenderParameters LightParameters;
			Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);

			if (FVector3f(LightParameters.Color).IsZero())
			{
				continue;
			}

			FPathTracingLight& DestLight = Lights[NumLights++];
			uint32 Transmission = Light.LightSceneInfo->Proxy->Transmission();
			uint8 LightingChannelMask = Light.LightSceneInfo->Proxy->GetLightingChannelMask();

			DestLight.Flags = Transmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
			DestLight.Flags |= LightingChannelMask & PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->CastsDynamicShadow() ? PATHTRACER_FLAG_CAST_SHADOW_MASK : 0;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->CastsVolumetricShadow() ? PATHTRACER_FLAG_CAST_VOL_SHADOW_MASK : 0;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->GetCastCloudShadows() ? PATHTRACER_FLAG_CAST_CLOUD_SHADOW_MASK : 0;
			DestLight.IESAtlasIndex = INDEX_NONE;
			DestLight.MissShaderIndex = 0;

			if (RayTracingLightFunctionMap)
			{
				const int32* LightFunctionIndex = RayTracingLightFunctionMap->Find(Light.LightSceneInfo);
				if (LightFunctionIndex)
				{
					DestLight.MissShaderIndex = *LightFunctionIndex;
				}
			}

			// these mean roughly the same thing across all light types
			DestLight.Color = FVector3f(LightParameters.Color) * LightParameters.GetLightExposureScale(View.PreExposure);
			DestLight.TranslatedWorldPosition = FVector3f(LightParameters.WorldPosition + View.ViewMatrices.GetPreViewTranslation());
			DestLight.Normal = -LightParameters.Direction;
			DestLight.Tangent = LightParameters.Tangent;
			DestLight.Shaping = FVector2f(0.0f, 0.0f);
			DestLight.DiffuseSpecularScale = ::HVPT::Private::PackRG16(LightParameters.DiffuseScale, LightParameters.SpecularScale);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			DestLight.IndirectLightingScale = Light.LightSceneInfo->Proxy->GetIndirectLightingScale();
#endif
			DestLight.Attenuation = LightParameters.InvRadius;
			DestLight.FalloffExponent = 0;
			DestLight.VolumetricScatteringIntensity = Light.LightSceneInfo->Proxy->GetVolumetricScatteringIntensity();
			DestLight.RectLightAtlasUVOffset = FVector2f(0.0f, 0.0f);
			DestLight.RectLightAtlasUVScale = FVector2f(0.0f, 0.0f);

			DestLight.Normal = LightParameters.Direction;
			DestLight.Dimensions = FVector2f(LightParameters.SourceRadius, 0.0f);
			DestLight.Flags |= PATHTRACING_LIGHT_DIRECTIONAL;
		}
	}

	if (bUseAtmosphere && (View.SkyAtmosphereUniformShaderParameters == nullptr || !IsSkyAtmosphereHoldout(View.CachedViewUniformShaderParameters->EnvironmentComponentsFlags)))
	{
		// show directional lights when atmosphere is enabled and not marked as holdout
		// NOTE: there cannot be any skydome in this case
		*SceneVisibleLightCount = NumLights;
	}

	uint32 NumInfiniteLights = NumLights;

	for (const FLightSceneInfoCompact& Light : Scene->Lights)
	{
		ELightComponentType LightComponentType = (ELightComponentType)Light.LightSceneInfo->Proxy->GetLightType();

		if ((LightComponentType == LightType_Directional) /* already handled by the loop above */ ||
			((LightComponentType == LightType_Rect) && !View.Family->EngineShowFlags.RectLights) ||
			((LightComponentType == LightType_Spot) && !View.Family->EngineShowFlags.SpotLights) ||
			((LightComponentType == LightType_Point) && !View.Family->EngineShowFlags.PointLights))
		{
			// This light type is not currently enabled
			continue;
		}

		FLightRenderParameters LightParameters;
		Light.LightSceneInfo->Proxy->GetLightShaderParameters(LightParameters);

		if (FVector3f(LightParameters.Color).IsZero())
		{
			continue;
		}

		FPathTracingLight& DestLight = Lights[NumLights++];

		uint32 Transmission = Light.LightSceneInfo->Proxy->Transmission();
		uint8 LightingChannelMask = Light.LightSceneInfo->Proxy->GetLightingChannelMask();

		DestLight.Flags = Transmission ? PATHTRACER_FLAG_TRANSMISSION_MASK : 0;
		DestLight.Flags |= LightingChannelMask & PATHTRACER_FLAG_LIGHTING_CHANNEL_MASK;
		DestLight.Flags |= Light.LightSceneInfo->Proxy->CastsDynamicShadow() ? PATHTRACER_FLAG_CAST_SHADOW_MASK : 0;
		DestLight.Flags |= Light.LightSceneInfo->Proxy->CastsVolumetricShadow() ? PATHTRACER_FLAG_CAST_VOL_SHADOW_MASK : 0;
		DestLight.Flags |= Light.LightSceneInfo->Proxy->GetCastCloudShadows() ? PATHTRACER_FLAG_CAST_CLOUD_SHADOW_MASK : 0;
		DestLight.IESAtlasIndex = LightParameters.IESAtlasIndex;
		DestLight.MissShaderIndex = 0;

		// these mean roughly the same thing across all light types
		DestLight.Color = FVector3f(LightParameters.Color) * LightParameters.GetLightExposureScale(View.PreExposure);
		DestLight.TranslatedWorldPosition = FVector3f(LightParameters.WorldPosition + View.ViewMatrices.GetPreViewTranslation());
		DestLight.Normal = -LightParameters.Direction;
		DestLight.Tangent = LightParameters.Tangent;
		DestLight.Shaping = FVector2f(0.0f, 0.0f);
		DestLight.DiffuseSpecularScale = ::HVPT::Private::PackRG16(LightParameters.DiffuseScale, LightParameters.SpecularScale);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
		DestLight.IndirectLightingScale = Light.LightSceneInfo->Proxy->GetIndirectLightingScale();
#endif
		DestLight.Attenuation = LightParameters.InvRadius;
		DestLight.FalloffExponent = 0;
		DestLight.VolumetricScatteringIntensity = Light.LightSceneInfo->Proxy->GetVolumetricScatteringIntensity();
		DestLight.RectLightAtlasUVOffset = FVector2f(0.0f, 0.0f);
		DestLight.RectLightAtlasUVScale = FVector2f(0.0f, 0.0f);

		if (RayTracingLightFunctionMap)
		{
			const int32* LightFunctionIndex = RayTracingLightFunctionMap->Find(Light.LightSceneInfo);
			if (LightFunctionIndex)
			{
				DestLight.MissShaderIndex = *LightFunctionIndex;
			}
		}

		switch (LightComponentType)
		{
		case LightType_Rect:
		{
			DestLight.Dimensions = FVector2f(2.0f * LightParameters.SourceRadius, 2.0f * LightParameters.SourceLength);
			DestLight.Shaping = FVector2f(LightParameters.RectLightBarnCosAngle, LightParameters.RectLightBarnLength);
			DestLight.FalloffExponent = LightParameters.FalloffExponent;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->IsInverseSquared() ? 0 : PATHTRACER_FLAG_NON_INVERSE_SQUARE_FALLOFF_MASK;
			DestLight.Flags |= PATHTRACING_LIGHT_RECT;


			// Rect light atlas UV transformation
			DestLight.RectLightAtlasUVOffset = LightParameters.RectLightAtlasUVOffset;
			DestLight.RectLightAtlasUVScale = LightParameters.RectLightAtlasUVScale;
			if (LightParameters.RectLightAtlasMaxLevel < 16)
			{
				DestLight.Flags |= PATHTRACER_FLAG_HAS_RECT_TEXTURE_MASK;
			}
			break;
		}
		case LightType_Spot:
		{
			DestLight.Dimensions = FVector2f(LightParameters.SourceRadius, LightParameters.SourceLength);
			DestLight.Shaping = LightParameters.SpotAngles;
			DestLight.FalloffExponent = LightParameters.FalloffExponent;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->IsInverseSquared() ? 0 : PATHTRACER_FLAG_NON_INVERSE_SQUARE_FALLOFF_MASK;
			DestLight.Flags |= PATHTRACING_LIGHT_SPOT;
			break;
		}
		case LightType_Point:
		{
			DestLight.Dimensions = FVector2f(LightParameters.SourceRadius, LightParameters.SourceLength);
			DestLight.FalloffExponent = LightParameters.FalloffExponent;
			DestLight.Flags |= Light.LightSceneInfo->Proxy->IsInverseSquared() ? 0 : PATHTRACER_FLAG_NON_INVERSE_SQUARE_FALLOFF_MASK;
			DestLight.Flags |= PATHTRACING_LIGHT_POINT;
			break;
		}
		default:
		{
			// Just in case someone adds a new light type one day ...
			checkNoEntry();
			break;
		}
		}
	}

	*SceneLightCount = NumLights;
	{
		// Upload the buffer of lights to the GPU
		uint32 NumCopyLights = FMath::Max(1u, NumLights); // need at least one since zero-sized buffers are not allowed
		size_t DataSize = sizeof(FPathTracingLight) * NumCopyLights;
		*SceneLights = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CreateStructuredBuffer(GraphBuilder, TEXT("PathTracer.LightsBuffer"), sizeof(FPathTracingLight), NumCopyLights, Lights, DataSize, ERDGInitialDataFlags::NoCopy)));
	}

	::HVPT::Private::PrepareLightGrid(GraphBuilder, View.FeatureLevel, LightGridParameters, Lights, NumLights, NumInfiniteLights, *SceneLights);
}

#endif
