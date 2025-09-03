#pragma once

#include "ShaderParameterStruct.h"
#include "HVPT.h"

class FScene;

struct FHVPTViewState;
struct FVolumetricMeshBatch;

// Voxel Grid structures

struct FHVPT_TopLevelGridData
{
	uint32 PackedData[1];
};

struct FHVPT_GridData
{
	uint32 PackedData[2];
};


BEGIN_UNIFORM_BUFFER_STRUCT(FHVPTOrthoGridUniformBufferParameters, )
	SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
	SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)
	SHADER_PARAMETER(FIntVector, TopLevelGridResolution)

	SHADER_PARAMETER(int32, bUseOrthoGrid)

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_TopLevelGridData>, TopLevelGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, ExtinctionGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, EmissionGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, ScatteringGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, VelocityGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, MajorantGridBuffer)
END_UNIFORM_BUFFER_STRUCT()

BEGIN_UNIFORM_BUFFER_STRUCT(FHVPTFrustumGridUniformBufferParameters, )
	SHADER_PARAMETER(FMatrix44f, WorldToClip)
	SHADER_PARAMETER(FMatrix44f, ClipToWorld)

	SHADER_PARAMETER(FMatrix44f, WorldToView)
	SHADER_PARAMETER(FMatrix44f, ViewToWorld)

	SHADER_PARAMETER(FMatrix44f, ViewToClip)
	SHADER_PARAMETER(FMatrix44f, ClipToView)

	SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMin)
	SHADER_PARAMETER(FVector3f, TopLevelGridWorldBoundsMax)
	SHADER_PARAMETER(FIntVector, TopLevelFroxelGridResolution)
	SHADER_PARAMETER(FIntVector, VoxelDimensions)

	SHADER_PARAMETER(int32, bUseFrustumGrid)

	SHADER_PARAMETER(float, NearPlaneDepth)
	SHADER_PARAMETER(float, FarPlaneDepth)
	SHADER_PARAMETER(float, TanHalfFOV)

	SHADER_PARAMETER_ARRAY(FVector4f, ViewFrustumPlanes, [6])

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_TopLevelGridData>, TopLevelFroxelGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, ExtinctionFroxelGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, EmissionFroxelGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, ScatteringFroxelGridBuffer)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FHVPT_GridData>, VelocityFroxelGridBuffer)
END_UNIFORM_BUFFER_STRUCT()


struct FHVPT_VoxelGridBuildOptions
{
	float ShadingRateInFrustum = HVPT::GetInsideFrustumShadingRate();
	float ShadingRateOutOfFrustum = HVPT::GetOutsideFrustumShadingRate();

	bool bBuildOrthoGrid = true;
	bool bBuildFrustumGrid = true;
	bool bUseProjectedPixelSizeForOrthoGrid = true;
	bool bJitter = HVPT::GetShouldJitter();
};

struct FHVPTFrustumGridParameterCache
{
	FMatrix44f WorldToClip;
	FMatrix44f ClipToWorld;

	FMatrix44f WorldToView;
	FMatrix44f ViewToWorld;

	FMatrix44f ViewToClip;
	FMatrix44f ClipToView;

	FVector3f TopLevelGridWorldBoundsMin;
	FVector3f TopLevelGridWorldBoundsMax;
	FIntVector TopLevelGridResolution;
	FIntVector VoxelDimensions;

	int32 bUseFrustumGrid = false;

	float NearPlaneDepth;
	float FarPlaneDepth;
	float TanHalfFOV;

	FVector4f ViewFrustumPlanes[6];

	TRefCountPtr<FRDGPooledBuffer> TopLevelGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> ExtinctionGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> EmissionGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> ScatteringGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> VelocityGridBuffer = nullptr;
};

struct FHVPTOrthoGridParameterCache
{
	FVector3f TopLevelGridWorldBoundsMin;
	FVector3f TopLevelGridWorldBoundsMax;
	FIntVector TopLevelGridResolution;
	int32 bUseOrthoGrid = false;

	TRefCountPtr<FRDGPooledBuffer> TopLevelGridBuffer = nullptr;

	TRefCountPtr<FRDGPooledBuffer> ExtinctionGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> EmissionGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> ScatteringGridBuffer = nullptr;
	TRefCountPtr<FRDGPooledBuffer> VelocityGridBuffer = nullptr;

	TRefCountPtr<FRDGPooledBuffer> MajorantGridBuffer = nullptr;
};

namespace HVPT
{

// --- FRUSTUM GRID --- //

TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> CreateEmptyFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder
);

TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters> GetFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	FHVPTViewState& ViewState
);

void ExtractFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumGridUniformBuffer,
	FHVPTFrustumGridParameterCache& FrustumGridParameterCache
);

void RegisterExternalFrustumVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const FHVPTFrustumGridParameterCache& FrustumGridParameterCache,
	TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumGridUniformBuffer
);

void BuildFrustumVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	TRDGUniformBufferRef<FHVPTFrustumGridUniformBufferParameters>& FrustumVoxelGridUniformBuffer
);


// --- ORTHO GRID --- //

TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> CreateEmptyOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder
);

TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters> GetOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	FHVPTViewState& ViewState
);

void ExtractOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoGridUniformBuffer,
	FHVPTOrthoGridParameterCache& OrthoGridParameterCache
);

void RegisterExternalOrthoVoxelGridUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const FHVPTOrthoGridParameterCache& OrthoGridParameterCache,
	TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoGridUniformBuffer
);

void BuildOrthoVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	TRDGUniformBufferRef<FHVPTOrthoGridUniformBufferParameters>& OrthoVoxelGridUniformBuffer
);


namespace Private
{
// Builder helper functions
// Implemented in VoxelGridBuild.cpp

// Common helpers
float CalcTanHalfFOV(float FOVInDegrees);

// Frustum Grid Builder Helpers

void CalcViewBoundsAndMinimumVoxelSize(
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	FBoxSphereBounds& TopLevelGridBounds,
	float& MinimumVoxelSize
);

void ClipNearFarDistances(
	const FViewInfo& View,
	const FBoxSphereBounds& TopLevelGridBounds, 
	float& NearPlaneDistance, 
	float& FarPlaneDistance
);

void CalculateTopLevelGridResolutionForFrustumGrid(
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	float MinimumVoxelSize,
	float NearPlaneDistance,
	float& FarPlaneDistance,
	FIntVector& TopLevelGridResolution
);

void MarkTopLevelGridVoxelsForFrustumGrid(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FIntVector TopLevelGridResolution,
	float NearPlaneDistance,
	float FarPlaneDistance,
	const FMatrix& ViewToWorld,
	FRDGBufferRef& TopLevelGridBuffer
);

void GenerateRasterTiles(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	FIntVector TopLevelGridResolution,
	FRDGBufferRef TopLevelGridBuffer,
	FRDGBufferRef& RasterTileBuffer,
	FRDGBufferRef& RasterTileAllocatorBuffer
);

void RasterizeVolumesIntoFrustumVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	// Transform data
	FMatrix& ViewToWorld,
	float NearPlaneDistance,
	float FarPlaneDistance,
	// Raster tile
	FRDGBufferRef RasterTileBuffer,
	FRDGBufferRef RasterTileAllocatorBuffer,
	// Top-level grid
	FIntVector TopLevelGridResolution,
	FRDGBufferRef& TopLevelGridBuffer,
	// Bottom-level grid
	FRDGBufferRef& ExtinctionGridBuffer,
	FRDGBufferRef& EmissionGridBuffer,
	FRDGBufferRef& ScatteringGridBuffer,
	FRDGBufferRef& VelocityGridBuffer
);


// Ortho Grid Builder Helpers

void CollectHeterogeneousVolumeMeshBatches(
	const FViewInfo& View,
	TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches
);

void CalcGlobalBoundsAndMinimumVoxelSize(
	const FViewInfo& View,
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	FBoxSphereBounds::Builder& TopLevelGridBoundsBuilder,
	float& GlobalMinimumVoxelSize
);

void CalculateTopLevelGridResolution(
	const FBoxSphereBounds& TopLevelGridBounds,
	float GlobalMinimumVoxelSize,
	FIntVector& TopLevelGridResolution
);

void CalculateVoxelSize(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches,
	const FHVPT_VoxelGridBuildOptions& BuildOptions,
	const FBoxSphereBounds& TopLevelGridBounds,
	FIntVector TopLevelGridResolution,
	FRDGBufferRef& TopLevelGridBuffer
);

void MarkTopLevelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FBoxSphereBounds& TopLevelGridBounds,
	FIntVector TopLevelGridResolution,
	FRDGBufferRef& TopLevelGridBuffer
);

void RasterizeVolumesIntoOrthoVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const TSet<FVolumetricMeshBatch>& HeterogeneousVolumesMeshBatches,
	const FHVPT_VoxelGridBuildOptions BuildOptions,
	// Raster tile
	FRDGBufferRef RasterTileBuffer,
	FRDGBufferRef RasterTileAllocatorBuffer,
	// Top-level grid
	FBoxSphereBounds TopLevelGridBounds,
	FIntVector TopLevelGridResolution,
	FRDGBufferRef& TopLevelGridBuffer,
	// Bottom-level grid
	FRDGBufferRef& ExtinctionGridBuffer,
	FRDGBufferRef& EmissionGridBuffer,
	FRDGBufferRef& ScatteringGridBuffer,
	FRDGBufferRef& VelocityGridBuffer
);

void BuildMajorantVoxelGrid(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FIntVector& TopLevelGridResolution,
	FRDGBufferRef TopLevelGridBuffer,
	FRDGBufferRef ExtinctionGridBuffer,
	FRDGBufferRef& MajorantVoxelGridBuffer
);

}
}
