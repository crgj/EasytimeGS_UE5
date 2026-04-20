/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class EasytimeSplatRuntime : ModuleRules
{
	public EasytimeSplatRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"FileUtilities",
				"GeometryCore",
				"ImageWrapper",
				"Json",
				"Projects",
				"RenderCore",
				"Renderer",
				"RHI",
				"Slate",
				"SlateCore",
			}
		);

		PrivateIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(GetModuleDirectory("Renderer"), "Private"),
				Path.Combine(GetModuleDirectory("Renderer"), "Internal"),
			}
		);
	}
}

