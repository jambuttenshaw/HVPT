#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "HVPTWorldSubsystem.generated.h"


class FHVPTViewExtension;

UCLASS()
class UHVPTWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem interface	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End	UWorldSubsystem interface

protected:
	TSharedPtr<FHVPTViewExtension> HVPTViewExtension;
};
