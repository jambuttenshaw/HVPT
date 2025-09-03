#include "HVPTWorldSubsystem.h"

#include "HVPTViewExtension.h"


void UHVPTWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
#if RHI_RAYTRACING
	HVPTViewExtension = FSceneViewExtensions::NewExtension<FHVPTViewExtension>();
#endif
}

void UHVPTWorldSubsystem::Deinitialize()
{
}
