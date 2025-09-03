#pragma once

#include "HeterogeneousVolumeInterface.h"


// Heterogeneous volume extended interface (for HVPT)
class IHeterogeneousVolumeExInterface : public IHeterogeneousVolumeInterface
{
public:
	virtual ~IHeterogeneousVolumeExInterface() {}

	// Animation
	virtual bool IsPlayingAnimation() const = 0;
};


class FHeterogeneousVolumeExData : public IHeterogeneousVolumeExInterface, FOneFrameResource
{
public:
	explicit FHeterogeneousVolumeExData(const FPrimitiveSceneProxy* SceneProxy)
		: PrimitiveSceneProxy(SceneProxy)
		, InstanceToLocal(FMatrix::Identity)
		, VoxelResolution(FIntVector::ZeroValue)
		, MinimumVoxelSize(0.1)
		, StepFactor(1.0)
		, ShadowStepFactor(8.0)
		, ShadowBiasFactor(0.0)
		, LightingDownsampleFactor(1.0)
		, MipBias(0.0)
		, bPivotAtCentroid(false)
		, bHoldout(false)
		, bIsPlayingAnimation(false)
	{
	}

	FHeterogeneousVolumeExData(const FPrimitiveSceneProxy* SceneProxy, FString Name)
		: PrimitiveSceneProxy(SceneProxy)
		, InstanceToLocal(FMatrix::Identity)
		, VoxelResolution(FIntVector::ZeroValue)
		, MinimumVoxelSize(0.1)
		, StepFactor(1.0)
		, ShadowStepFactor(8.0)
		, ShadowBiasFactor(0.0)
		, LightingDownsampleFactor(1.0)
		, MipBias(0.0)
		, bPivotAtCentroid(false)
		, bHoldout(false)
#if ACTOR_HAS_LABELS
		, ReadableName(Name)
#endif // ACTOR_HAS_LABELS
		, bIsPlayingAnimation(false)
	{
	}
	virtual ~FHeterogeneousVolumeExData() {}

	virtual const FPrimitiveSceneProxy* GetPrimitiveSceneProxy() const override { return PrimitiveSceneProxy; }

	// Local-space
	virtual const FBoxSphereBounds& GetBounds() const override { return PrimitiveSceneProxy->GetBounds(); }
	virtual const FBoxSphereBounds& GetLocalBounds() const override { return PrimitiveSceneProxy->GetLocalBounds(); }
	virtual const FMatrix& GetLocalToWorld() const override { return PrimitiveSceneProxy->GetLocalToWorld(); }
	virtual const FMatrix& GetInstanceToLocal() const override { return InstanceToLocal; }
	virtual const FMatrix GetInstanceToWorld() const override { return InstanceToLocal * PrimitiveSceneProxy->GetLocalToWorld(); }

	// Volume
	virtual FIntVector GetVoxelResolution() const override { return VoxelResolution; }
	virtual float GetMinimumVoxelSize() const override { return MinimumVoxelSize; }
	virtual bool IsPivotAtCentroid() const override { return bPivotAtCentroid; }

	// Lighting
	virtual float GetStepFactor() const override { return StepFactor; }
	virtual float GetShadowStepFactor() const override { return ShadowStepFactor; }
	virtual float GetShadowBiasFactor() const override { return ShadowBiasFactor; }
	virtual float GetLightingDownsampleFactor() const override { return LightingDownsampleFactor; }
	virtual float GetMipBias() const override { return MipBias; }

	// Rendering
	virtual bool IsHoldout() const override { return bHoldout; }

	// IHeterogeneousVolumeExInterface
	virtual bool IsPlayingAnimation() const override { return bIsPlayingAnimation; }

	const FPrimitiveSceneProxy* PrimitiveSceneProxy;
	FMatrix InstanceToLocal;
	FIntVector VoxelResolution;
	float MinimumVoxelSize;
	float StepFactor;
	float ShadowStepFactor;
	float ShadowBiasFactor;
	float LightingDownsampleFactor;
	float MipBias;
	bool bPivotAtCentroid;
	bool bHoldout;

#if ACTOR_HAS_LABELS
	FString ReadableName;
	virtual FString GetReadableName() const { return ReadableName; }
#else
	virtual FString GetReadableName() const { return PrimitiveSceneProxy->GetResourceName().ToString(); }
#endif // ACTOR_HAS_LABELS

	// IHeterogeneousVolumeExInterface
	bool bIsPlayingAnimation;
};
