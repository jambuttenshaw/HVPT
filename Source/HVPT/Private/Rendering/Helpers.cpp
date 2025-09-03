#include "Helpers.h"

#include "ShaderCore.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "SceneCore.h"
#include "SceneRendering.h"
#include "SceneTextureParameters.h"


FSceneTextureParameters HVPT::Private::GetSceneTextureParameters(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
{
	const auto& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	FSceneTextureParameters Parameters;

	// Should always have a depth buffer around allocated, since early z-pass is first.
	Parameters.SceneDepthTexture = SceneTextures.Depth.Resolve;
	Parameters.SceneStencilTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(Parameters.SceneDepthTexture, PF_X24_G8));

	// Registers all the scene texture from the scene context. No fallback is provided to catch mistake at shader parameter validation time
	// when a pass is trying to access a resource before any other pass actually created it.
	Parameters.GBufferVelocityTexture = GetIfProduced(SceneTextures.Velocity);
	Parameters.GBufferATexture = GetIfProduced(SceneTextures.GBufferA);
	Parameters.GBufferBTexture = GetIfProduced(SceneTextures.GBufferB);
	Parameters.GBufferCTexture = GetIfProduced(SceneTextures.GBufferC);
	Parameters.GBufferDTexture = GetIfProduced(SceneTextures.GBufferD);
	Parameters.GBufferETexture = GetIfProduced(SceneTextures.GBufferE);
	Parameters.GBufferFTexture = GetIfProduced(SceneTextures.GBufferF, SystemTextures.MidGrey);

	return Parameters;
}


class FRaytracingShaderBindingLayout : public FShaderBindingLayoutContainer
{
public:
	static const FShaderBindingLayout& GetInstance(EBindingType BindingType)
	{
		static FRaytracingShaderBindingLayout Instance;
		return Instance.GetLayout(BindingType);
	}
private:

	FRaytracingShaderBindingLayout()
	{
		// No special binding layout flags required
		EShaderBindingLayoutFlags ShaderBindingLayoutFlags = EShaderBindingLayoutFlags::None;

		// Add scene, view and nanite ray tracing as global/static uniform buffers
		TArray<FShaderParametersMetadata*> StaticUniformBuffers;
		StaticUniformBuffers.Add(FindUniformBufferStructByName(TEXT("Scene")));
		StaticUniformBuffers.Add(FindUniformBufferStructByName(TEXT("View")));
		StaticUniformBuffers.Add(FindUniformBufferStructByName(TEXT("NaniteRayTracing")));
		StaticUniformBuffers.Add(FindUniformBufferStructByName(TEXT("LumenHardwareRayTracingUniformBuffer")));

		BuildShaderBindingLayout(StaticUniformBuffers, ShaderBindingLayoutFlags, *this);
	}
};

const FShaderBindingLayout* HVPT::Private::GetShaderBindingLayout(EShaderPlatform ShaderPlatform)
{
	if (RHIGetStaticShaderBindingLayoutSupport(ShaderPlatform) != ERHIStaticShaderBindingLayoutSupport::Unsupported)
	{
		// Retrieve the bindless shader binding table
		return &FRaytracingShaderBindingLayout::GetInstance(FShaderBindingLayoutContainer::EBindingType::Bindless);
	}

	// No binding table supported
	return nullptr;
}

TOptional<FScopedUniformBufferStaticBindings> HVPT::Private::BindStaticUniformBufferBindings(
	const FViewInfo& View,
	FRHIUniformBuffer* SceneUniformBuffer,
	FRHIUniformBuffer* NaniteRayTracingUniformBuffer,
	FRHICommandList& RHICmdList)
{
	TOptional<FScopedUniformBufferStaticBindings> StaticUniformBufferScope;

	// Setup the static uniform buffers used by the RTPSO if enabled
	const FShaderBindingLayout* ShaderBindingLayout = HVPT::Private::GetShaderBindingLayout(View.GetShaderPlatform());
	if (ShaderBindingLayout)
	{
		FUniformBufferStaticBindings StaticUniformBuffers(&ShaderBindingLayout->RHILayout);
		StaticUniformBuffers.AddUniformBuffer(View.ViewUniformBuffer.GetReference());
		StaticUniformBuffers.AddUniformBuffer(SceneUniformBuffer);
		StaticUniformBuffers.AddUniformBuffer(NaniteRayTracingUniformBuffer);
		StaticUniformBuffers.AddUniformBuffer(View.LumenHardwareRayTracingUniformBuffer.GetReference());

		StaticUniformBufferScope.Emplace(RHICmdList, StaticUniformBuffers);
	}

	return StaticUniformBufferScope;
}


FHVPT_PathTracingFogParameters HVPT::Private::PrepareFogParameters(const FViewInfo& View, const FExponentialHeightFogSceneInfo& FogInfo)
{
	static_assert(FExponentialHeightFogSceneInfo::NumFogs == 2, "Path tracing code assumes a fixed number of fogs");
	FHVPT_PathTracingFogParameters Parameters = {};

	const FVector PreViewTranslation = View.ViewMatrices.GetPreViewTranslation();

	// See VolumetricFog.usf - the factor of .5 is needed for a better match to HeightFog behavior
	const float MatchHeightFogFactor = .5f;
	Parameters.FogDensity.X = MatchHeightFogFactor * FogInfo.FogData[0].Density * FogInfo.VolumetricFogExtinctionScale;
	Parameters.FogDensity.Y = MatchHeightFogFactor * FogInfo.FogData[1].Density * FogInfo.VolumetricFogExtinctionScale;
	Parameters.FogHeight.X = FogInfo.FogData[0].Height + PreViewTranslation.Z;
	Parameters.FogHeight.Y = FogInfo.FogData[1].Height + PreViewTranslation.Z;
	// Clamp to UI limit to avoid division by 0 in the transmittance calculations
	// Note that we have to adjust by factor of 1000.0 that is applied in FExponentialHeightFogSceneInfo()
	Parameters.FogFalloff.X = FMath::Max(FogInfo.FogData[0].HeightFalloff, 0.001f / 1000.0f);
	Parameters.FogFalloff.Y = FMath::Max(FogInfo.FogData[1].HeightFalloff, 0.001f / 1000.0f);
	Parameters.FogAlbedo = FogInfo.VolumetricFogAlbedo;
	Parameters.FogPhaseG = FogInfo.VolumetricFogScatteringDistribution;

	const float DensityEpsilon = 1e-6f;
	const float Radius = FogInfo.VolumetricFogDistance;
	// compute the value of Z at which the density becomes negligible (but don't go beyond the radius)
	const float ZMax0 = Parameters.FogHeight.X + FMath::Min(Radius, FMath::Log2(FMath::Max(Parameters.FogDensity.X, DensityEpsilon) / DensityEpsilon) / Parameters.FogFalloff.X);
	const float ZMax1 = Parameters.FogHeight.Y + FMath::Min(Radius, FMath::Log2(FMath::Max(Parameters.FogDensity.Y, DensityEpsilon) / DensityEpsilon) / Parameters.FogFalloff.Y);
	// lowest point is just defined by the radius (fog is homogeneous below the height)
	const float ZMin0 = Parameters.FogHeight.X - Radius;
	const float ZMin1 = Parameters.FogHeight.Y - Radius;

	// center X,Y around the current view point
	// NOTE: this can lead to "sliding" when the view distance is low, would it be better to just use the component center instead?
	// NOTE: the component position is not available here, would need to be added to FogInfo ...
	const FVector O = View.ViewMatrices.GetViewOrigin() + PreViewTranslation;
	Parameters.FogCenter = FVector2f(O.X, O.Y);
	Parameters.FogMinZ = FMath::Min(ZMin0, ZMin1);
	Parameters.FogMaxZ = FMath::Max(ZMax0, ZMax1);
	Parameters.FogRadius = Radius;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	static auto CVarPathTracingFogDensityClamp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.FogDensityClamp"));
	float DensityClamp = FMath::Clamp(CVarPathTracingFogDensityClamp->GetFloat(), 1.0f, 256.0f);
#else
	float DensityClamp = 8.0f;
#endif
	Parameters.FogFalloffClamp = -FMath::Log2(DensityClamp);

	return Parameters;
}