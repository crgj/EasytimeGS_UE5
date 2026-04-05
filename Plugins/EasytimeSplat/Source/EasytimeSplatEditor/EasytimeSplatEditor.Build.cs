/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class EasytimeSplatEditor : ModuleRules
{
	public EasytimeSplatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDefinitions.Add("SPLAT_EXPORT_API=__declspec(dllimport)");
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetDefinition",
				"Core",
				"CoreUObject",
				"GeometryCore",
				"EasytimeSplatRuntime",
				"EasytimeSplatThirdParty",
				"UnrealEd",
				"RenderCore",
				"RHI"
			}
		);
	}
}

