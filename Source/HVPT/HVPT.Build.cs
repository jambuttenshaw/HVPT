// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class HVPT : ModuleRules
{

	public string PluginRoot =>
		System.IO.Path.GetFullPath(
			System.IO.Path.Combine(ModuleDirectory, "../../")
		);

	public HVPT(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		string EnginePath = Path.GetFullPath(Target.RelativeEnginePath);
		
		PrivateIncludePaths.AddRange(
			new string[] {
				EnginePath + "Source/Runtime/Renderer/Private/",
				EnginePath + "Source/Runtime/Renderer/Internal/",

				PluginRoot + "Shaders/Shared/"
            }
        );
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"RHICore",
				"RHI",
                "RenderCore",
				"Renderer"
			}
		);
	}
}
