/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

using System.IO;
using UnrealBuildTool;

public class EasytimeSplatThirdParty : ModuleRules
{
	public EasytimeSplatThirdParty(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDefinitions.Add("SPLAT_EXPORT_API=__declspec(dllexport)");
        PrivateDependencyModuleNames.Add("Core");

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "splat"));
	}
}

