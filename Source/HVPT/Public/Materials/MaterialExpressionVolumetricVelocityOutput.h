#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "MaterialExpressionVolumetricVelocityOutput.generated.h"


UCLASS(CollapseCategories, HideCategories = Object, MinimalAPI)
class UMaterialExpressionVolumetricVelocityOutput : public UMaterialExpressionCustomOutput
{
    GENERATED_UCLASS_BODY()

public:

    /** Local-space volume velocity of animated volumes. For example, if the volume was exported from Houdini with a velocity attribute, then the velocity attribute should go here. */
    UPROPERTY(meta = (RequiredInput = "false", ToolTip = "Local-space volume velocity of animated volumes. For example, if the volume was exported from Houdini with a velocity attribute, then the velocity attribute should go here."))
    FExpressionInput LocalSpaceVelocity;

#if WITH_EDITOR
	//~ Begin UMaterialExpression Interface
	virtual int32 Compile(class FMaterialCompiler* Compiler, int32 OutputIndex) override;
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	//~ End UMaterialExpression Interface
#endif

	//~ Begin UMaterialExpressionCustomOutput Interface
	virtual int32 GetNumOutputs() const override;
	virtual FString GetFunctionName() const override;
	virtual FString GetDisplayName() const override;
	//~ End UMaterialExpressionCustomOutput Interface
};
