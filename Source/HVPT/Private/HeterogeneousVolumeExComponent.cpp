#include "HeterogeneousVolumeExComponent.h"

#include "HeterogeneousVolumeExSceneProxy.h"

#include "Components/BillboardComponent.h"

#include "MaterialTypes.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "ContentStreaming.h"

#include "SparseVolumeTexture/SparseVolumeTexture.h"


// USparseVolumeTexture::GetOptimalStreamingMipLevel was not marked for DLL export
float SparseVolumeTexture_GetOptimalStreamingMipLevel(const USparseVolumeTexture* SparseVolumeTexture, const FBoxSphereBounds& Bounds, float MipBias)
{
	check(IsInGameThread());
	float ResultMipLevel = 0.0f;
	if (IStreamingManager* StreamingManager = IStreamingManager::Get_Concurrent())
	{
		ResultMipLevel = FLT_MAX;
		const int32 NumViews = StreamingManager->GetNumViews();
		for (int32 ViewIndex = 0; ViewIndex < NumViews; ++ViewIndex)
		{
			const FStreamingViewInfo& ViewInfo = StreamingManager->GetViewInformation(ViewIndex);

			// Determine the pixel-width at the near-plane.
			const float PixelWidth = 1.0f / (ViewInfo.FOVScreenSize * 0.5f); // FOVScreenSize = ViewRect.Width / Tan(FOV * 0.5)

			// Project to nearest distance of volume bounds.
			const float Distance = FMath::Max<float>(1.0f, ((ViewInfo.ViewOrigin - Bounds.Origin).GetAbs() - Bounds.BoxExtent).Length());
			const float VoxelWidth = Distance * PixelWidth;

			// MIP is defined as the log of the ratio of native voxel resolution to pixel-coverage of volume bounds.
			// We want to be conservative here (use potentially lower mip), so try to minimize the term we pass into Log2() by using
			// the maximum dimension of the bounds and the minimum extent of the volume resolution. The bounds are axis aligned, so
			// we can't assume that a given dimension in SVT UV space aligns with any particular dimension of the axis aligned bounds.
			const float PixelWidthCoverage = (2.0f * Bounds.BoxExtent.GetMax()) / VoxelWidth;
			const float VoxelResolution = SparseVolumeTexture->GetVolumeResolution().GetMin();
			float ViewMipLevel = FMath::Log2(VoxelResolution / PixelWidthCoverage) + MipBias;
			ViewMipLevel = FMath::Clamp(ViewMipLevel, 0.0f, SparseVolumeTexture->GetNumMipLevels() - 1.0f);

			ResultMipLevel = FMath::Min(ViewMipLevel, ResultMipLevel);
		}
	}
	return ResultMipLevel;
}


UHeterogeneousVolumeExComponent::UHeterogeneousVolumeExComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	// What is this?
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;

#if WITH_EDITORONLY_DATA
	bTickInEditor = true;
#endif // WITH_EDITORONLY_DATA

	MaterialInstanceDynamic = nullptr;
	VolumeResolution = FIntVector(128);
	FrameRate = 24.0f;
	bPlaying = false;
	bLooping = false;
	Frame = 0;
	StartFrame = 0;
	EndFrame = 0;
	StepFactor = 1.0f;
	ShadowStepFactor = 2.0f;
	ShadowBiasFactor = 0.5f;
	LightingDownsampleFactor = 2.0f;
	StreamingMipBias = 0.0f;
	bIssueBlockingRequests = false;
	bPivotAtCentroid = false;
	PreviousSVT = nullptr;
}

void UHeterogeneousVolumeExComponent::SetStreamingMipBias(int32 NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& StreamingMipBias != NewValue)
	{
		StreamingMipBias = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetVolumeResolution(FIntVector NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& VolumeResolution != NewValue)
	{
		VolumeResolution = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetFrame(float NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& Frame != NewValue)
	{
		Frame = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetFrameRate(float NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& FrameRate != NewValue)
	{
		FrameRate = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetStartFrame(float NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& StartFrame != NewValue)
	{
		StartFrame = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetEndFrame(float NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& EndFrame != NewValue)
	{
		EndFrame = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetPlaying(bool NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& bPlaying != NewValue)
	{
		bPlaying = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::SetLooping(bool NewValue)
{
	if (AreDynamicDataChangesAllowed()
		&& bLooping != NewValue)
	{
		bLooping = NewValue;
		MarkRenderStateDirty();
	}
}

void UHeterogeneousVolumeExComponent::Play()
{
	if (AreDynamicDataChangesAllowed())
	{
		bPlaying = 1;
		Frame = 0;
		MarkRenderStateDirty();
	}
}

FPrimitiveSceneProxy* UHeterogeneousVolumeExComponent::CreateSceneProxy()
{
	return new FHeterogeneousVolumeExSceneProxy(this);
}

FBoxSphereBounds UHeterogeneousVolumeExComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBoxSphereBounds NewBounds;

	FVector HalfVolumeResolution = FVector(VolumeResolution) * 0.5;
	if (bPivotAtCentroid)
	{
		NewBounds.Origin = FVector::ZeroVector;
	}
	else
	{
		NewBounds.Origin = HalfVolumeResolution;
	}
	NewBounds.BoxExtent = HalfVolumeResolution;
	NewBounds.SphereRadius = NewBounds.BoxExtent.Length();
	return NewBounds.TransformBy(FrameTransform * LocalToWorld);
}

void UHeterogeneousVolumeExComponent::PostLoad()
{
	Super::PostLoad();

	MaterialInstanceDynamic = nullptr;
	if (UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial())
	{
		MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
	}
}

void UHeterogeneousVolumeExComponent::PostInitProperties()
{
	Super::PostInitProperties();

	if (IsInGameThread())
	{
		MaterialInstanceDynamic = nullptr;
		if (UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial())
		{
			MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
		}
	}
}

void UHeterogeneousVolumeExComponent::PostReinitProperties()
{
	Super::PostReinitProperties();

	if (IsInGameThread())
	{
		MaterialInstanceDynamic = nullptr;
		if (UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial())
		{
			MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
		}
	}
}

USparseVolumeTexture* UHeterogeneousVolumeExComponent::GetSparseVolumeTexture(UMaterialInterface* MaterialInterface, int32 ParameterIndex, FName* OutParamName)
{
	USparseVolumeTexture* SparseVolumeTexture = nullptr;

	if (MaterialInterface)
	{
		// Get parameter infos for all SVTs in the material
		TArray<FMaterialParameterInfo> ParameterInfo;
		TArray<FGuid> ParameterIds;
		MaterialInterface->GetAllSparseVolumeTextureParameterInfo(ParameterInfo, ParameterIds);

		// Get the SVT object
		if (ParameterInfo.IsValidIndex(ParameterIndex))
		{
			MaterialInterface->GetSparseVolumeTextureParameterValue(ParameterInfo[ParameterIndex], SparseVolumeTexture);
		}

		// The SVT in MaterialInterface might be a frame of a UStreamableSparseVolumeTexture. In that case we try to get the owning SVT object.
		if (SparseVolumeTexture && SparseVolumeTexture->IsA<USparseVolumeTextureFrame>())
		{
			USparseVolumeTextureFrame* Frame = Cast<USparseVolumeTextureFrame>(SparseVolumeTexture);
			UObject* FrameOuter = Frame->GetOuter();
			check(FrameOuter->IsA<UStreamableSparseVolumeTexture>());
			SparseVolumeTexture = Cast<USparseVolumeTexture>(FrameOuter);
			check(SparseVolumeTexture);
		}

		if (SparseVolumeTexture && OutParamName)
		{
			*OutParamName = ParameterInfo[ParameterIndex].Name;
		}
	}

	return SparseVolumeTexture;
}

UMaterialInstanceDynamic* UHeterogeneousVolumeExComponent::CreateOrCastToMID(UMaterialInterface* MaterialInterface)
{
	if (MaterialInterface->IsA<UMaterialInstanceDynamic>())
	{
		return Cast<UMaterialInstanceDynamic>(MaterialInterface);
	}
	else
	{
		return UMaterialInstanceDynamic::Create(MaterialInterface, nullptr);
	}
}

void UHeterogeneousVolumeExComponent::OnSparseVolumeTextureChanged(const USparseVolumeTexture* SparseVolumeTexture)
{
	if (SparseVolumeTexture != PreviousSVT)
	{
		if (SparseVolumeTexture)
		{
			VolumeResolution = SparseVolumeTexture->GetVolumeResolution();
			StartFrame = 0;
			EndFrame = SparseVolumeTexture->GetNumFrames() - 1;
			Frame = FMath::Clamp(Frame, StartFrame, EndFrame);
		}
		else
		{
			VolumeResolution = FIntVector(128);
			Frame = 0.0f;
			StartFrame = 0.0f;
			EndFrame = 0.0f;
		}

		PreviousSVT = SparseVolumeTexture;
		MarkRenderStateDirty();
	}
}

UMaterialInterface* UHeterogeneousVolumeExComponent::GetHeterogeneousVolumeMaterial() const
{
	const uint32 MaterialIndex = 0;
	UMaterialInterface* MaterialInterface = GetMaterial(MaterialIndex);
	if (MaterialInterface)
	{
		const UMaterial* Material = MaterialInterface->GetMaterial();
		if (Material && Material->MaterialDomain == EMaterialDomain::MD_Volume)
		{
			MaterialInterface->CheckMaterialUsage(MATUSAGE_HeterogeneousVolumes);
			return MaterialInterface;
		}
	}
	return nullptr;
}

#if WITH_EDITOR
void UHeterogeneousVolumeExComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const int32 SVTParameterIndex = 0;

	FName PropertyName;
	if (PropertyChangedEvent.Property)
	{
		PropertyName = PropertyChangedEvent.Property->GetFName();
	}

	// When this component is copied/duplicated in the editor, PostEditChangeProperty() is called with a null PropertyChangedEvent, so we also 
	// create the MID in that case. Otherwise the component will be copied but not play back because MaterialInstanceDynamic stays nullptr.
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeExComponent, OverrideMaterials) || PropertyChangedEvent.Property == nullptr)
	{
		MaterialInstanceDynamic = nullptr; // Reset internal MID. We either create a new one from the new material or leave it as null if the material was unset
		UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial();
		if (MaterialInterface)
		{
			MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
		}
		OnSparseVolumeTextureChanged(GetSparseVolumeTexture(MaterialInterface, SVTParameterIndex));
		MarkRenderStateDirty();
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeExComponent, VolumeResolution))
	{
		// Prevent resolution changes when using SVT
		const USparseVolumeTexture* SparseVolumeTexture = GetSparseVolumeTexture(GetHeterogeneousVolumeMaterial(), SVTParameterIndex);
		if (SparseVolumeTexture)
		{
			VolumeResolution = SparseVolumeTexture->GetVolumeResolution();
		}
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeExComponent, Frame))
	{
		const USparseVolumeTexture* SparseVolumeTexture = GetSparseVolumeTexture(GetHeterogeneousVolumeMaterial(), SVTParameterIndex);
		if (SparseVolumeTexture)
		{
			Frame = FMath::Clamp(Frame, StartFrame, EndFrame);
		}
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeExComponent, StartFrame))
	{
		const USparseVolumeTexture* SparseVolumeTexture = GetSparseVolumeTexture(GetHeterogeneousVolumeMaterial(), SVTParameterIndex);
		if (SparseVolumeTexture)
		{
			StartFrame = FMath::Clamp(StartFrame, 0, EndFrame);
			Frame = FMath::Clamp(Frame, StartFrame, EndFrame);
		}
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHeterogeneousVolumeExComponent, EndFrame))
	{
		const USparseVolumeTexture* SparseVolumeTexture = GetSparseVolumeTexture(GetHeterogeneousVolumeMaterial(), SVTParameterIndex);
		if (SparseVolumeTexture)
		{
			const int32 FrameCount = SparseVolumeTexture->GetNumFrames();
			EndFrame = FMath::Clamp(EndFrame, StartFrame, FrameCount - 1);
			Frame = FMath::Clamp(Frame, StartFrame, EndFrame);
		}
	}
}
#endif // WITH_EDITOR

void UHeterogeneousVolumeExComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	UMeshComponent::GetUsedMaterials(OutMaterials, bGetDebugMaterials);

	if (MaterialInstanceDynamic)
	{
		OutMaterials.Add(MaterialInstanceDynamic);
	}
}

bool UHeterogeneousVolumeExComponent::IsMaterialSlotNameValid(FName MaterialSlotName) const
{
	return GetMaterialIndex(MaterialSlotName) >= 0;
}

int32 UHeterogeneousVolumeExComponent::GetMaterialIndex(FName MaterialSlotName) const
{
	TArray<UMaterialInterface*> Materials;
	GetUsedMaterials(Materials);
	for (int32 MaterialIndex = 0; MaterialIndex < Materials.Num(); ++MaterialIndex)
	{
		if (Materials[MaterialIndex]->GetFName() == MaterialSlotName)
		{
			return MaterialIndex;
		}
	}

	return INDEX_NONE;
}

void UHeterogeneousVolumeExComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* Material)
{
	Super::SetMaterial(ElementIndex, Material);
	if (Material && ElementIndex == 0)
	{
		MaterialInstanceDynamic = nullptr; // Reset internal MID. We either create a new one from the new material or leave it as null if the material was unset
		UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial();
		if (MaterialInterface)
		{
			MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
		}
		OnSparseVolumeTextureChanged(GetSparseVolumeTexture(MaterialInterface, 0));
	}
}

void UHeterogeneousVolumeExComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Forcibly update the MID if it's not initialized
	if (!MaterialInstanceDynamic)
	{
		UMaterialInterface* MaterialInterface = GetHeterogeneousVolumeMaterial();
		if (MaterialInterface)
		{
			MaterialInstanceDynamic = CreateOrCastToMID(MaterialInterface);
		}
	}

	// Aligning renderable state behavior with Instanced Static Mesh and Skinned Mesh. HV is currently not supporoted in indirect lighting and ray-tracing far field.
	static const auto CVarHeterogeneousVolumes = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HeterogeneousVolumes"));
	bool bHeterogeneousVolumesEnabled = CVarHeterogeneousVolumes && CVarHeterogeneousVolumes->GetInt() > 0;
	if ((ShouldRender() || bCastHiddenShadow) && DoesPlatformSupportHeterogeneousVolumes(GetWorld()->Scene->GetShaderPlatform()) && bHeterogeneousVolumesEnabled && MaterialInstanceDynamic)
	{
		const int32 SVTParameterIndex = 0;
		FName SVTParameterName;
		USparseVolumeTexture* SparseVolumeTexture = GetSparseVolumeTexture(GetHeterogeneousVolumeMaterial(), SVTParameterIndex, &SVTParameterName);

#if WITH_EDITOR
		// Detect an update to the material
		if (SparseVolumeTexture != PreviousSVT)
		{
			OnSparseVolumeTextureChanged(SparseVolumeTexture);
		}
#endif // WITH_EDITOR

		if (SparseVolumeTexture)
		{
			const int32 FrameCount = SparseVolumeTexture->GetNumFrames();

			// Determine active frame based on animation controls if playing
			if (bPlaying)
			{
				Frame += DeltaTime * FrameRate;
			}

			if (bLooping)
			{
				float FrameRange = EndFrame - StartFrame + 1;
				Frame = FMath::Fmod(Frame - StartFrame, (float)FrameRange) + StartFrame;
			}
			else
			{
				Frame = FMath::Clamp(Frame, StartFrame, EndFrame);
			}

			const bool bIsBlocking = bIssueBlockingRequests != 0;
			const bool bHasValidFrameRate = bPlaying != 0;
			const float MipLevel = SparseVolumeTexture_GetOptimalStreamingMipLevel(SparseVolumeTexture, Bounds, StreamingMipBias);
			USparseVolumeTextureFrame* SparseVolumeTextureFrame = USparseVolumeTextureFrame::GetFrameAndIssueStreamingRequest(SparseVolumeTexture, GetTypeHash(this), FrameRate, Frame, MipLevel, bIsBlocking, bHasValidFrameRate);
			if (SparseVolumeTextureFrame)
			{
				FIntVector PerFrameVolumeResolution = SparseVolumeTextureFrame->GetVolumeResolution();
				if (VolumeResolution != PerFrameVolumeResolution)
				{
					VolumeResolution = PerFrameVolumeResolution;
					MarkRenderTransformDirty();
				}

				FTransform PerFrameTransform = SparseVolumeTextureFrame->GetFrameTransform();
				if (!PerFrameTransform.Equals(FrameTransform))
				{
					FrameTransform = PerFrameTransform;
					MarkRenderStateDirty();
				}
			}

			MaterialInstanceDynamic->SetSparseVolumeTextureParameterValue(SVTParameterName, SparseVolumeTextureFrame);
		}
	}
}


AHeterogeneousVolumeEx::AHeterogeneousVolumeEx(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeterogeneousVolumeExComponent = CreateDefaultSubobject<UHeterogeneousVolumeExComponent>(TEXT("HeterogeneousVolumeExComponent"));
	RootComponent = HeterogeneousVolumeExComponent;

#if WITH_EDITORONLY_DATA

	if (!IsRunningCommandlet())
	{
		// Structure to hold one-time initialization
		struct FConstructorStatics
		{
			FName ID_HeterogeneousVolume;
			FText NAME_HeterogeneousVolume;
			FConstructorStatics()
				: ID_HeterogeneousVolume(TEXT("Fog"))
				, NAME_HeterogeneousVolume(NSLOCTEXT("SpriteCategory", "Fog", "Fog"))
			{
			}
		};
		static FConstructorStatics ConstructorStatics;
		if (GetSpriteComponent())
		{
			GetSpriteComponent()->SetRelativeScale3D(FVector(0.5f, 0.5f, 0.5f));
			GetSpriteComponent()->bHiddenInGame = true;
			GetSpriteComponent()->bIsScreenSizeScaled = true;
			GetSpriteComponent()->SpriteInfo.Category = ConstructorStatics.ID_HeterogeneousVolume;
			GetSpriteComponent()->SpriteInfo.DisplayName = ConstructorStatics.NAME_HeterogeneousVolume;
			GetSpriteComponent()->SetupAttachment(HeterogeneousVolumeExComponent);
			GetSpriteComponent()->bReceivesDecals = false;
		}
	}
#endif // WITH_EDITORONLY_DATA

	PrimaryActorTick.bCanEverTick = true;
	SetHidden(false);
}
