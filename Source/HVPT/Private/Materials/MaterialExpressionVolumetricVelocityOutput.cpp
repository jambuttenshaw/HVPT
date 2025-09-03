#include "Materials/MaterialExpressionVolumetricVelocityOutput.h"

#include "MaterialCompiler.h"


UMaterialExpressionVolumetricVelocityOutput::UMaterialExpressionVolumetricVelocityOutput(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITOR
	Outputs.Reset();
#endif
}

#if WITH_EDITOR

int32 UMaterialExpressionVolumetricVelocityOutput::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	int32 CodeInput = INDEX_NONE;

	// Generates function names GetVolumetricAdvanceMaterialOutput{index} used in BasePixelShader.usf.
	if (OutputIndex == 0)
	{
		CodeInput = LocalSpaceVelocity.IsConnected() ? LocalSpaceVelocity.Compile(Compiler) : Compiler->Constant3(0.0f, 0.0f, 0.0f);
	}

	return Compiler->CustomOutput(this, OutputIndex, CodeInput);
}

void UMaterialExpressionVolumetricVelocityOutput::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(TEXT("Volumetric Velocity Output"));
}

int32 UMaterialExpressionVolumetricVelocityOutput::GetNumOutputs() const
{
	return 1;
}

FString UMaterialExpressionVolumetricVelocityOutput::GetFunctionName() const
{
	return TEXT("GetVolumetricVelocityOutput");
}

FString UMaterialExpressionVolumetricVelocityOutput::GetDisplayName() const
{
	return TEXT("Volumetric Velocity Output");
}

#endif
