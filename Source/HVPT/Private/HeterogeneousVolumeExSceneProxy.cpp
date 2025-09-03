#include "HeterogeneousVolumeExSceneProxy.h"

#include "HeterogeneousVolumeExComponent.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "Materials/MaterialInstanceDynamic.h"

static size_t GHeterogeneousVolumeExSceneProxy_UniquePointer;


FHeterogeneousVolumeExSceneProxy::FHeterogeneousVolumeExSceneProxy(UHeterogeneousVolumeExComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, VertexFactory(GetScene().GetFeatureLevel(), "FHeterogeneousVolumeExSceneProxy")
#if ACTOR_HAS_LABELS
	, HeterogeneousVolumeData(this, InComponent->GetReadableName())
#else
	, HeterogeneousVolumeData(this)
#endif
	, MaterialInterface(InComponent->GetMaterial(0))
{
	bIsHeterogeneousVolume = true;
	bCastDynamicShadow = InComponent->CastShadow;

	// Heterogeneous volumes do not deform internally
	bHasDeformableMesh = false;
	ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;

	HeterogeneousVolumeData.VoxelResolution = InComponent->VolumeResolution;
	HeterogeneousVolumeData.InstanceToLocal = InComponent->FrameTransform.ToMatrixWithScale();

	// Infer minimum voxel size from bounds and resolution
	FVector VoxelSize = 2.0 * InComponent->Bounds.BoxExtent;
	VoxelSize.X /= InComponent->VolumeResolution.X;
	VoxelSize.Y /= InComponent->VolumeResolution.Y;
	VoxelSize.Z /= InComponent->VolumeResolution.Z;
	HeterogeneousVolumeData.MinimumVoxelSize = FMath::Max(VoxelSize.GetMin(), 0.001);

	if (InComponent->MaterialInstanceDynamic)
	{
		MaterialInterface = InComponent->MaterialInstanceDynamic;
	}

	if (MaterialInterface)
	{
		MaterialRelevance = MaterialInterface->GetRelevance_Concurrent(GetScene().GetFeatureLevel());
	}

	HeterogeneousVolumeData.StepFactor = InComponent->StepFactor;
	HeterogeneousVolumeData.ShadowStepFactor = InComponent->ShadowStepFactor;
	HeterogeneousVolumeData.ShadowBiasFactor = InComponent->ShadowBiasFactor;
	HeterogeneousVolumeData.LightingDownsampleFactor = InComponent->LightingDownsampleFactor;
	HeterogeneousVolumeData.MipBias = InComponent->StreamingMipBias;
	HeterogeneousVolumeData.bPivotAtCentroid = InComponent->bPivotAtCentroid;
	HeterogeneousVolumeData.bHoldout = InComponent->bHoldout;

	HeterogeneousVolumeData.bIsPlayingAnimation = InComponent->bPlaying;

	// Initialize vertex buffer data for a quad
	StaticMeshVertexBuffers.PositionVertexBuffer.Init(4);
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.Init(4, 1);
	StaticMeshVertexBuffers.ColorVertexBuffer.Init(4);

	for (uint32 VertexIndex = 0; VertexIndex < 4; ++VertexIndex)
	{
		StaticMeshVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex) = FColor::White;
	}

	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(0) = FVector3f(-1.0, -1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(1) = FVector3f(-1.0, 1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(2) = FVector3f(1.0, -1.0, -1.0);
	StaticMeshVertexBuffers.PositionVertexBuffer.VertexPosition(3) = FVector3f(1.0, 1.0, -1.0);

	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(0, 0, FVector2f(0, 0));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(1, 0, FVector2f(0, 1));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(2, 0, FVector2f(1, 0));
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(3, 0, FVector2f(1, 1));

	FHeterogeneousVolumeExSceneProxy* Self = this;
	ENQUEUE_RENDER_COMMAND(FHeterogeneousVolumeSceneProxyInit)(
		[Self](FRHICommandListImmediate& RHICmdList)
		{
			Self->StaticMeshVertexBuffers.PositionVertexBuffer.InitResource(RHICmdList);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.InitResource(RHICmdList);
			Self->StaticMeshVertexBuffers.ColorVertexBuffer.InitResource(RHICmdList);

			FLocalVertexFactory::FDataType Data;
			Self->StaticMeshVertexBuffers.PositionVertexBuffer.BindPositionVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(&Self->VertexFactory, Data);
			Self->StaticMeshVertexBuffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(&Self->VertexFactory, Data, 0);
			Self->StaticMeshVertexBuffers.ColorVertexBuffer.BindColorVertexBuffer(&Self->VertexFactory, Data);
			Self->VertexFactory.SetData(RHICmdList, Data);

			Self->VertexFactory.InitResource(RHICmdList);
		}
		);
}

// Virtual destructor
FHeterogeneousVolumeExSceneProxy::~FHeterogeneousVolumeExSceneProxy()
{
	VertexFactory.ReleaseResource();
	StaticMeshVertexBuffers.PositionVertexBuffer.ReleaseResource();
	StaticMeshVertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
	StaticMeshVertexBuffers.ColorVertexBuffer.ReleaseResource();
}

SIZE_T FHeterogeneousVolumeExSceneProxy::GetStaticTypeHash()
{
	return reinterpret_cast<size_t>(&GHeterogeneousVolumeExSceneProxy_UniquePointer);
}

FPrimitiveViewRelevance FHeterogeneousVolumeExSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;

	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.HeterogeneousVolumes;
	Result.bOpaque = false;
	Result.bStaticRelevance = false;
	Result.bDynamicRelevance = true;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bRenderInMainPass = ShouldRenderInMainPass();

	return Result;
}

void FHeterogeneousVolumeExSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (Views.IsEmpty())
	{
		return;
	}

	if (ViewFamily.EngineShowFlags.HeterogeneousVolumes)
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				if (MaterialInterface)
				{
					// Set up MeshBatch
					FMeshBatch& Mesh = Collector.AllocateMesh();

					Mesh.VertexFactory = &VertexFactory;
					Mesh.MaterialRenderProxy = MaterialInterface->GetRenderProxy();
					Mesh.LCI = NULL;
					Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative() ? true : false;
					Mesh.CastShadow = CastsDynamicShadow();
					Mesh.Type = PT_TriangleStrip;
					Mesh.bDisableBackfaceCulling = true;

					// Set up the FMeshBatchElement.
					FMeshBatchElement& BatchElement = Mesh.Elements[0];
					BatchElement.IndexBuffer = NULL;
					BatchElement.FirstIndex = 0;
					BatchElement.MinVertexIndex = 0;
					BatchElement.MaxVertexIndex = 3;
					BatchElement.NumPrimitives = 2;
					BatchElement.BaseVertexIndex = 0;

					// Heterogeneous Volume Interface is passed through UserData.
					BatchElement.UserData = &HeterogeneousVolumeData;

					Mesh.bCanApplyViewModeOverrides = true;
					Mesh.bUseWireframeSelectionColoring = IsSelected();
					Mesh.bUseSelectionOutline = false;
					Mesh.bSelectable = false;

					Collector.AddMesh(ViewIndex, Mesh);
				}

				const FSceneView* View = Views[ViewIndex];
				RenderBounds(Collector.GetPDI(ViewIndex), View->Family->EngineShowFlags, GetBounds(), IsSelected());
			}
		}
	}
}
