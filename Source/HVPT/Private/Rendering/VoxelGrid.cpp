#include "VoxelGrid.h"

#include "RenderGraphBuilder.h"
#include "SceneRendering.h"
#include "SystemTextures.h"

#include "HVPTViewState.h"

IMPLEMENT_UNIFORM_BUFFER_STRUCT(FHVPTOrthoGridUniformBufferParameters, "HVPT_OrthoGrid")
IMPLEMENT_UNIFORM_BUFFER_STRUCT(FHVPTFrustumGridUniformBufferParameters, "HVPT_FrustumGrid")


//////////////////////////
// --- FRUSTUM GRID --- //
//////////////////////////

TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> HVPT::CreateEmptyFrustumVoxelGridUniformBuffer(FRDGBuilder& GraphBuilder)
{
	FHVPTFrustumGridUniformBufferParameters* UniformBufferParameters = GraphBuilder.AllocParameters<FHVPTFrustumGridUniformBufferParameters>();
	{
		UniformBufferParameters->WorldToClip = FMatrix44f::Identity;
		UniformBufferParameters->ClipToWorld = FMatrix44f::Identity;
		UniformBufferParameters->WorldToView = FMatrix44f::Identity;
		UniformBufferParameters->ViewToWorld = FMatrix44f::Identity;
		UniformBufferParameters->ViewToClip = FMatrix44f::Identity;
		UniformBufferParameters->ClipToView = FMatrix44f::Identity;

		UniformBufferParameters->TopLevelFroxelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_TopLevelGridData)));
		UniformBufferParameters->ExtinctionFroxelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		UniformBufferParameters->EmissionFroxelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		UniformBufferParameters->ScatteringFroxelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		UniformBufferParameters->VelocityFroxelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));

		UniformBufferParameters->TopLevelGridWorldBoundsMin = FVector3f(0);
		UniformBufferParameters->TopLevelGridWorldBoundsMax = FVector3f(0);
		UniformBufferParameters->TopLevelFroxelGridResolution = FIntVector(0);
		UniformBufferParameters->VoxelDimensions = FIntVector(0);
		UniformBufferParameters->bUseFrustumGrid = false;
		UniformBufferParameters->NearPlaneDepth = 0.0;
		UniformBufferParameters->FarPlaneDepth = 0.0;
		UniformBufferParameters->TanHalfFOV = 1.0;
	}
	return GraphBuilder.CreateUniformBuffer(UniformBufferParameters);
}

TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> HVPT::GetFrustumVoxelGridUniformBuffer(FRDGBuilder& GraphBuilder, FHVPTViewState& ViewState)
{
	if (ViewState.FrustumGridUniformBuffer)
	{
		return ViewState.FrustumGridUniformBuffer;
	}

	if (ViewState.FrustumGridParameterCache.TopLevelGridBuffer)
	{
		RegisterExternalFrustumVoxelGridUniformBuffer(GraphBuilder, ViewState.FrustumGridParameterCache, ViewState.FrustumGridUniformBuffer);
		return ViewState.FrustumGridUniformBuffer;
	}

	return nullptr;
}

void HVPT::ExtractFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder, const TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumGridUniformBuffer, FHVPTFrustumGridParameterCache& ParameterCache
)
{
	const TRDGParameterStruct<FHVPTFrustumGridUniformBufferParameters>& Parameters = FrustumGridUniformBuffer->GetParameters();

	ParameterCache.TopLevelGridWorldBoundsMin = Parameters->TopLevelGridWorldBoundsMin;
	ParameterCache.TopLevelGridWorldBoundsMax = Parameters->TopLevelGridWorldBoundsMax;
	ParameterCache.TopLevelGridResolution = Parameters->TopLevelFroxelGridResolution;
	ParameterCache.VoxelDimensions = Parameters->VoxelDimensions;
	ParameterCache.bUseFrustumGrid = Parameters->bUseFrustumGrid;
	ParameterCache.NearPlaneDepth = Parameters->NearPlaneDepth;
	ParameterCache.FarPlaneDepth = Parameters->FarPlaneDepth;
	ParameterCache.TanHalfFOV = Parameters->TanHalfFOV;


	ParameterCache.WorldToClip = Parameters->WorldToClip;
	ParameterCache.ClipToWorld = Parameters->ClipToWorld;
	ParameterCache.WorldToView = Parameters->WorldToView;
	ParameterCache.ViewToWorld = Parameters->ViewToWorld;
	ParameterCache.ViewToClip = Parameters->ViewToClip;
	ParameterCache.ClipToView = Parameters->ClipToView;

	for (int i = 0; i < 6; ++i)
	{
		ParameterCache.ViewFrustumPlanes[i] = Parameters->ViewFrustumPlanes[i];
	}

	GraphBuilder.QueueBufferExtraction(Parameters->TopLevelFroxelGridBuffer->GetParent(), &ParameterCache.TopLevelGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->ExtinctionFroxelGridBuffer->GetParent(), &ParameterCache.ExtinctionGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->EmissionFroxelGridBuffer->GetParent(), &ParameterCache.EmissionGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->ScatteringFroxelGridBuffer->GetParent(), &ParameterCache.ScatteringGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->VelocityFroxelGridBuffer->GetParent(), &ParameterCache.VelocityGridBuffer);
}

void HVPT::RegisterExternalFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder, const FHVPTFrustumGridParameterCache& ParameterCache, TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumGridUniformBuffer
)
{
	FHVPTFrustumGridUniformBufferParameters* Parameters = GraphBuilder.AllocParameters<FHVPTFrustumGridUniformBufferParameters>();
	{
		Parameters->WorldToClip = ParameterCache.WorldToClip;
		Parameters->ClipToWorld = ParameterCache.ClipToWorld;

		Parameters->WorldToView = ParameterCache.WorldToView;
		Parameters->ViewToWorld = ParameterCache.ViewToWorld;

		Parameters->ViewToClip = ParameterCache.ViewToClip;
		Parameters->ClipToView = ParameterCache.ClipToView;

		Parameters->TopLevelGridWorldBoundsMin = ParameterCache.TopLevelGridWorldBoundsMin;
		Parameters->TopLevelGridWorldBoundsMax = ParameterCache.TopLevelGridWorldBoundsMax;
		Parameters->TopLevelFroxelGridResolution = ParameterCache.TopLevelGridResolution;
		Parameters->VoxelDimensions = ParameterCache.TopLevelGridResolution;
		Parameters->bUseFrustumGrid = ParameterCache.bUseFrustumGrid;
		Parameters->NearPlaneDepth = ParameterCache.NearPlaneDepth;
		Parameters->FarPlaneDepth = ParameterCache.FarPlaneDepth;
		Parameters->TanHalfFOV = ParameterCache.TanHalfFOV;

		// Frustum assignment
		for (int i = 0; i < 6; ++i)
		{
			Parameters->ViewFrustumPlanes[i] = ParameterCache.ViewFrustumPlanes[i];
		}

		Parameters->TopLevelFroxelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.TopLevelGridBuffer));
		Parameters->ExtinctionFroxelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.ExtinctionGridBuffer));
		Parameters->EmissionFroxelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.EmissionGridBuffer));
		Parameters->ScatteringFroxelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.ScatteringGridBuffer));
		Parameters->VelocityFroxelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.VelocityGridBuffer));
	}
	FrustumGridUniformBuffer = GraphBuilder.CreateUniformBuffer(Parameters);
}

void HVPT::BuildFrustumVoxelGrid(
	FRDGBuilder& GraphBuilder, const FScene* Scene, const FViewInfo& View, const FHVPT_VoxelGridBuildOptions& BuildOptions, TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumGridUniformBuffer
)
{
	if (!HVPT::ShouldRenderHVPTForView(View) || !HVPT::EnableFrustumGrid() || !BuildOptions.bBuildFrustumGrid)
	{
		FrustumGridUniformBuffer = CreateEmptyFrustumVoxelGridUniformBuffer(GraphBuilder);
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: Frustum Grid Build");

	// Determine the minimum voxel size for the scene, based on screen projection or user-defined minima
	FBoxSphereBounds TopLevelGridBounds;
	float MinimumVoxelSize;
	HVPT::Private::CalcViewBoundsAndMinimumVoxelSize(View, BuildOptions, TopLevelGridBounds, MinimumVoxelSize);

	if (TopLevelGridBounds.SphereRadius == 0)
	{
		FrustumGridUniformBuffer = CreateEmptyFrustumVoxelGridUniformBuffer(GraphBuilder);
		return;
	}

	float NearPlaneDistance = HVPT::GetNearPlaneDistanceForFrustumGrid();
	float FarPlaneDistance = HVPT::GetFarPlaneDistanceForFrustumGrid();
	HVPT::Private::ClipNearFarDistances(View, TopLevelGridBounds, NearPlaneDistance, FarPlaneDistance);

	FIntVector TopLevelGridResolution;
	HVPT::Private::CalculateTopLevelGridResolutionForFrustumGrid(
		View,
		BuildOptions,
		MinimumVoxelSize,
		NearPlaneDistance,
		FarPlaneDistance,
		TopLevelGridResolution
	);

	// Construct top-level grid over global bounding domain with some pre-determined resolution
	int32 TopLevelVoxelCount = TopLevelGridResolution.X * TopLevelGridResolution.Y * TopLevelGridResolution.Z;
	if (TopLevelVoxelCount == 0)
	{
		FrustumGridUniformBuffer = CreateEmptyFrustumVoxelGridUniformBuffer(GraphBuilder);
		return;
	}

	FMatrix WorldToView = View.ViewMatrices.GetViewMatrix();
	FMatrix ViewToWorld = View.ViewMatrices.GetInvViewMatrix();

	// Mark top-level voxels for rasterization
	FRDGBufferRef TopLevelGridBuffer;
	HVPT::Private::MarkTopLevelGridVoxelsForFrustumGrid(
		GraphBuilder,
		View,
		TopLevelGridResolution,
		NearPlaneDistance,
		FarPlaneDistance,
		ViewToWorld,
		TopLevelGridBuffer
	);

	// Generate raster tiles of approximately equal work
	FRDGBufferRef RasterTileBuffer;
	FRDGBufferRef RasterTileAllocatorBuffer;
	HVPT::Private::GenerateRasterTiles(
		GraphBuilder,
		Scene,
		// Grid data
		TopLevelGridResolution,
		TopLevelGridBuffer,
		// Tile data
		RasterTileBuffer,
		RasterTileAllocatorBuffer
	);

	FRDGBufferRef ExtinctionGridBuffer;
	FRDGBufferRef EmissionGridBuffer;
	FRDGBufferRef ScatteringGridBuffer;
	FRDGBufferRef VelocityGridBuffer;
	HVPT::Private::RasterizeVolumesIntoFrustumVoxelGrid(
		GraphBuilder,
		Scene,
		View,
		BuildOptions,
		ViewToWorld,
		NearPlaneDistance,
		FarPlaneDistance,
		// Raster tile
		RasterTileBuffer,
		RasterTileAllocatorBuffer,
		// Top-level grid
		TopLevelGridResolution,
		TopLevelGridBuffer,
		// Bottom-level grid
		ExtinctionGridBuffer,
		EmissionGridBuffer,
		ScatteringGridBuffer,
		VelocityGridBuffer
	);

	// Create Voxel Grid uniform buffer
	FHVPTFrustumGridUniformBufferParameters* UniformBufferParameters = GraphBuilder.AllocParameters<FHVPTFrustumGridUniformBufferParameters>();
	{
		FMatrix ViewToClip = FPerspectiveMatrix(
			FMath::DegreesToRadians(View.FOV * 0.5),
			TopLevelGridResolution.X,
			TopLevelGridResolution.Y,
			NearPlaneDistance,
			FarPlaneDistance
		);
		FMatrix ClipToView = ViewToClip.Inverse();
		UniformBufferParameters->ViewToClip = FMatrix44f(ViewToClip);
		UniformBufferParameters->ClipToView = FMatrix44f(ClipToView);

		FMatrix WorldToClip = WorldToView * ViewToClip;
		FMatrix ClipToWorld = ClipToView * ViewToWorld;
		UniformBufferParameters->WorldToClip = FMatrix44f(WorldToClip);
		UniformBufferParameters->ClipToWorld = FMatrix44f(ClipToWorld);

		UniformBufferParameters->WorldToView = FMatrix44f(WorldToView);
		UniformBufferParameters->ViewToWorld = FMatrix44f(ViewToWorld);

		UniformBufferParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
		UniformBufferParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);
		UniformBufferParameters->TopLevelFroxelGridResolution = TopLevelGridResolution;
		UniformBufferParameters->VoxelDimensions = TopLevelGridResolution;

		UniformBufferParameters->bUseFrustumGrid = HVPT::EnableFrustumGrid();
		UniformBufferParameters->NearPlaneDepth = NearPlaneDistance;
		UniformBufferParameters->FarPlaneDepth = FarPlaneDistance;
		UniformBufferParameters->TanHalfFOV = Private::CalcTanHalfFOV(View.FOV);

		// Frustum assignment
		{
			// Near/Far plane definition is reversed when explicitly specifying clipping planes
			FPlane NearPlane;
			ViewToClip.GetFrustumFarPlane(NearPlane);
			UniformBufferParameters->ViewFrustumPlanes[0] = FVector4f(NearPlane.X, NearPlane.Y, NearPlane.Z, NearPlane.W);

			FPlane FarPlane;
			ViewToClip.GetFrustumNearPlane(FarPlane);
			UniformBufferParameters->ViewFrustumPlanes[1] = FVector4f(FarPlane.X, FarPlane.Y, FarPlane.Z, FarPlane.W);

			FPlane LeftPlane;
			ViewToClip.GetFrustumLeftPlane(LeftPlane);
			UniformBufferParameters->ViewFrustumPlanes[2] = FVector4f(LeftPlane.X, LeftPlane.Y, LeftPlane.Z, LeftPlane.W);

			FPlane RightPlane;
			ViewToClip.GetFrustumRightPlane(RightPlane);
			UniformBufferParameters->ViewFrustumPlanes[3] = FVector4f(RightPlane.X, RightPlane.Y, RightPlane.Z, RightPlane.W);

			FPlane TopPlane;
			ViewToClip.GetFrustumTopPlane(TopPlane);
			UniformBufferParameters->ViewFrustumPlanes[4] = FVector4f(TopPlane.X, TopPlane.Y, TopPlane.Z, TopPlane.W);

			FPlane BottomPlane;
			ViewToClip.GetFrustumBottomPlane(BottomPlane);
			UniformBufferParameters->ViewFrustumPlanes[5] = FVector4f(BottomPlane.X, BottomPlane.Y, BottomPlane.Z, BottomPlane.W);
		}

		UniformBufferParameters->TopLevelFroxelGridBuffer = GraphBuilder.CreateSRV(TopLevelGridBuffer);
		UniformBufferParameters->ExtinctionFroxelGridBuffer = GraphBuilder.CreateSRV(ExtinctionGridBuffer);
		UniformBufferParameters->EmissionFroxelGridBuffer = GraphBuilder.CreateSRV(EmissionGridBuffer);
		UniformBufferParameters->ScatteringFroxelGridBuffer = GraphBuilder.CreateSRV(ScatteringGridBuffer);
		UniformBufferParameters->VelocityFroxelGridBuffer = GraphBuilder.CreateSRV(VelocityGridBuffer);
	}

	FrustumGridUniformBuffer = GraphBuilder.CreateUniformBuffer(UniformBufferParameters);
}

///////////////////////
// --- ORTHO GRID--- //
///////////////////////

TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> HVPT::CreateEmptyOrthoVoxelGridUniformBuffer(FRDGBuilder& GraphBuilder)
{
	FHVPTOrthoGridUniformBufferParameters* OrthoGridUniformBufferParameters = GraphBuilder.AllocParameters<FHVPTOrthoGridUniformBufferParameters>();
	{
		OrthoGridUniformBufferParameters->TopLevelGridWorldBoundsMin = FVector3f(0);
		OrthoGridUniformBufferParameters->TopLevelGridWorldBoundsMax = FVector3f(0);
		OrthoGridUniformBufferParameters->TopLevelGridResolution = FIntVector(0);

		OrthoGridUniformBufferParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_TopLevelGridData)));
		OrthoGridUniformBufferParameters->ExtinctionGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		OrthoGridUniformBufferParameters->EmissionGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		OrthoGridUniformBufferParameters->ScatteringGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));
		OrthoGridUniformBufferParameters->VelocityGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_GridData)));

		OrthoGridUniformBufferParameters->bUseOrthoGrid = false;
		OrthoGridUniformBufferParameters->MajorantGridBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FHVPT_MajorantGridData)));
	}
	return GraphBuilder.CreateUniformBuffer(OrthoGridUniformBufferParameters);
}

TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> HVPT::GetOrthoVoxelGridUniformBuffer(FRDGBuilder& GraphBuilder, FHVPTViewState& ViewState)
{
	if (ViewState.OrthoGridUniformBuffer)
	{
		return ViewState.OrthoGridUniformBuffer;
	}

	if (ViewState.OrthoGridParameterCache.TopLevelGridBuffer)
	{
		RegisterExternalOrthoVoxelGridUniformBuffer(GraphBuilder, ViewState.OrthoGridParameterCache, ViewState.OrthoGridUniformBuffer);
		return ViewState.OrthoGridUniformBuffer;
	}

	return nullptr;
}

void HVPT::ExtractOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder, const TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoGridUniformBuffer, FHVPTOrthoGridParameterCache& ParameterCache
)
{
	const TRDGParameterStruct<FHVPTOrthoGridUniformBufferParameters>& Parameters = OrthoGridUniformBuffer->GetParameters();

	ParameterCache.TopLevelGridWorldBoundsMin = Parameters->TopLevelGridWorldBoundsMin;
	ParameterCache.TopLevelGridWorldBoundsMax = Parameters->TopLevelGridWorldBoundsMax;
	ParameterCache.TopLevelGridResolution = Parameters->TopLevelGridResolution;
	ParameterCache.bUseOrthoGrid = Parameters->bUseOrthoGrid;

	GraphBuilder.QueueBufferExtraction(Parameters->TopLevelGridBuffer->GetParent(), &ParameterCache.TopLevelGridBuffer);

	GraphBuilder.QueueBufferExtraction(Parameters->ExtinctionGridBuffer->GetParent(), &ParameterCache.ExtinctionGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->EmissionGridBuffer->GetParent(), &ParameterCache.EmissionGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->ScatteringGridBuffer->GetParent(), &ParameterCache.ScatteringGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->VelocityGridBuffer->GetParent(), &ParameterCache.VelocityGridBuffer);
	GraphBuilder.QueueBufferExtraction(Parameters->MajorantGridBuffer->GetParent(), &ParameterCache.MajorantGridBuffer);
}

void HVPT::RegisterExternalOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder, const FHVPTOrthoGridParameterCache& ParameterCache, TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoGridUniformBuffer
)
{
	FHVPTOrthoGridUniformBufferParameters* UniformBufferParameters = GraphBuilder.AllocParameters<FHVPTOrthoGridUniformBufferParameters>();
	{
		UniformBufferParameters->TopLevelGridWorldBoundsMin = ParameterCache.TopLevelGridWorldBoundsMin;
		UniformBufferParameters->TopLevelGridWorldBoundsMax = ParameterCache.TopLevelGridWorldBoundsMax;
		UniformBufferParameters->TopLevelGridResolution = ParameterCache.TopLevelGridResolution;
		UniformBufferParameters->bUseOrthoGrid = ParameterCache.bUseOrthoGrid;

		UniformBufferParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.TopLevelGridBuffer));

		UniformBufferParameters->ExtinctionGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.ExtinctionGridBuffer));
		UniformBufferParameters->EmissionGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.EmissionGridBuffer));
		UniformBufferParameters->ScatteringGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.ScatteringGridBuffer));
		UniformBufferParameters->VelocityGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.VelocityGridBuffer));
		UniformBufferParameters->MajorantGridBuffer = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ParameterCache.MajorantGridBuffer));
	}
	OrthoGridUniformBuffer = GraphBuilder.CreateUniformBuffer(UniformBufferParameters);
}

void HVPT::BuildOrthoVoxelGrid(
	FRDGBuilder& GraphBuilder, const FScene* Scene, const FViewInfo& View, const FHVPT_VoxelGridBuildOptions& BuildOptions, TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoGridUniformBuffer
)
{
	if (!HVPT::ShouldRenderHVPTForView(View) || !HVPT::EnableOrthoGrid() || !BuildOptions.bBuildOrthoGrid)
	{
		OrthoGridUniformBuffer = CreateEmptyOrthoVoxelGridUniformBuffer(GraphBuilder);
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "HVPT: Ortho Grid Build");

	TSet<FVolumetricMeshBatch> HeterogeneousVolumesMeshBatches;
	HVPT::Private::CollectHeterogeneousVolumeMeshBatches(
		View,
		HeterogeneousVolumesMeshBatches
	);

	if (HeterogeneousVolumesMeshBatches.IsEmpty())
	{
		OrthoGridUniformBuffer = CreateEmptyOrthoVoxelGridUniformBuffer(GraphBuilder);
		return;
	}

	// Collect global bounds
	FBoxSphereBounds::Builder TopLevelGridBoundsBuilder;
	float GlobalMinimumVoxelSize;
	HVPT::Private::CalcGlobalBoundsAndMinimumVoxelSize(
		View,
		HeterogeneousVolumesMeshBatches,
		BuildOptions,
		TopLevelGridBoundsBuilder,
		GlobalMinimumVoxelSize
	);

	if (!TopLevelGridBoundsBuilder.IsValid())
	{
		OrthoGridUniformBuffer = CreateEmptyOrthoVoxelGridUniformBuffer(GraphBuilder);
		return;
	}
	FBoxSphereBounds TopLevelGridBounds(TopLevelGridBoundsBuilder);

	// Determine top-level grid resolution
	FIntVector TopLevelGridResolution;
	HVPT::Private::CalculateTopLevelGridResolution(
		TopLevelGridBounds,
		GlobalMinimumVoxelSize,
		TopLevelGridResolution
	);

	// Calculate the preferred voxel size for each bottom-level grid in a top-level cell
	FRDGBufferRef TopLevelGridBuffer;
	HVPT::Private::CalculateVoxelSize(
		GraphBuilder,
		View,
		HeterogeneousVolumesMeshBatches,
		BuildOptions,
		TopLevelGridBounds,
		TopLevelGridResolution,
		TopLevelGridBuffer
	);

	// Allocate bottom-level grid
	HVPT::Private::MarkTopLevelGrid(
		GraphBuilder,
		Scene,
		// Grid data
		TopLevelGridBounds,
		TopLevelGridResolution,
		TopLevelGridBuffer
	);

	// Generate raster tiles
	FRDGBufferRef RasterTileBuffer;
	FRDGBufferRef RasterTileAllocatorBuffer;
	HVPT::Private::GenerateRasterTiles(
		GraphBuilder,
		Scene,
		// Grid data
		TopLevelGridResolution,
		TopLevelGridBuffer,
		// Tile data
		RasterTileBuffer,
		RasterTileAllocatorBuffer
	);

	FRDGBufferRef ExtinctionGridBuffer;
	FRDGBufferRef EmissionGridBuffer;
	FRDGBufferRef ScatteringGridBuffer;
	FRDGBufferRef VelocityGridBuffer;

	HVPT::Private::RasterizeVolumesIntoOrthoVoxelGrid(
		GraphBuilder,
		Scene,
		View,
		HeterogeneousVolumesMeshBatches,
		BuildOptions,
		// Tile data
		RasterTileBuffer,
		RasterTileAllocatorBuffer,
		// Grid data
		TopLevelGridBounds,
		TopLevelGridResolution,
		TopLevelGridBuffer,
		ExtinctionGridBuffer,
		EmissionGridBuffer,
		ScatteringGridBuffer,
		VelocityGridBuffer
	);

	FRDGBufferRef MajorantGridBuffer;
	HVPT::Private::BuildMajorantVoxelGrid(GraphBuilder, Scene, TopLevelGridResolution, TopLevelGridBuffer, ExtinctionGridBuffer, MajorantGridBuffer);

	// Create Adpative Voxel Grid uniform buffer
	FHVPTOrthoGridUniformBufferParameters* OrthoGridUniformBufferParameters = GraphBuilder.AllocParameters<FHVPTOrthoGridUniformBufferParameters>();
	{
		OrthoGridUniformBufferParameters->TopLevelGridWorldBoundsMin = FVector3f(TopLevelGridBounds.Origin - TopLevelGridBounds.BoxExtent);
		OrthoGridUniformBufferParameters->TopLevelGridWorldBoundsMax = FVector3f(TopLevelGridBounds.Origin + TopLevelGridBounds.BoxExtent);
		OrthoGridUniformBufferParameters->TopLevelGridResolution = TopLevelGridResolution;

		OrthoGridUniformBufferParameters->TopLevelGridBuffer = GraphBuilder.CreateSRV(TopLevelGridBuffer);
		OrthoGridUniformBufferParameters->ExtinctionGridBuffer = GraphBuilder.CreateSRV(ExtinctionGridBuffer);
		OrthoGridUniformBufferParameters->EmissionGridBuffer = GraphBuilder.CreateSRV(EmissionGridBuffer);
		OrthoGridUniformBufferParameters->ScatteringGridBuffer = GraphBuilder.CreateSRV(ScatteringGridBuffer);
		OrthoGridUniformBufferParameters->VelocityGridBuffer = GraphBuilder.CreateSRV(VelocityGridBuffer);

		OrthoGridUniformBufferParameters->bUseOrthoGrid = HVPT::EnableOrthoGrid();
		OrthoGridUniformBufferParameters->MajorantGridBuffer = GraphBuilder.CreateSRV(MajorantGridBuffer);
	}

	OrthoGridUniformBuffer = GraphBuilder.CreateUniformBuffer(OrthoGridUniformBufferParameters);
}
