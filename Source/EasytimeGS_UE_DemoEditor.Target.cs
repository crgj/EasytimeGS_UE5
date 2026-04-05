using UnrealBuildTool;
using System.Collections.Generic;

public class EasytimeGS_UE_DemoEditorTarget : TargetRules
{
	public EasytimeGS_UE_DemoEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		bOverrideBuildEnvironment = true;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("EasytimeGS_UE_Demo");
	}
}
