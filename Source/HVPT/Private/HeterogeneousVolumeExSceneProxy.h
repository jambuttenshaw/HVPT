#pragma once

#include "LocalVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "StaticMeshResources.h"

#include "HeterogeneousVolumeExInterface.h"

class UHeterogeneousVolumeExComponent;


class FHeterogeneousVolumeExSceneProxy : public FPrimitiveSceneProxy
{
public:
	FHeterogeneousVolumeExSceneProxy(UHeterogeneousVolumeExComponent* InComponent);
	virtual ~FHeterogeneousVolumeExSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface.
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual SIZE_T GetTypeHash() const override
	{
		return GetStaticTypeHash();
	}
#if RHI_RAYTRACING
	virtual bool IsRayTracingRelevant() const override { return true; }
	virtual bool HasRayTracingRepresentation() const override { return true; }
#endif // RHI_RAYTRACING
	virtual uint32 GetMemoryFootprint(void) const override { return sizeof(*this) + GetAllocatedSize(); }
	//~ End FPrimitiveSceneProxy Interface.

	// This is a bit of an ugly hack to be able to identify when a FPrimitiveSceneProxy is a FHeterogeneousVolumeExSceneProxy
	static SIZE_T GetStaticTypeHash();

private:
	FLocalVertexFactory VertexFactory;
	FStaticMeshVertexBuffers StaticMeshVertexBuffers;
	FHeterogeneousVolumeExData HeterogeneousVolumeData;

	// Cache UObject values
	UMaterialInterface* MaterialInterface;
	FMaterialRelevance MaterialRelevance;
};
