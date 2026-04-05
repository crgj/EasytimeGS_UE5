using UnrealBuildTool;
using System.Collections.Generic;

public class EasytimeGS_UE_DemoTarget : TargetRules
{
	public EasytimeGS_UE_DemoTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		bOverrideBuildEnvironment = true;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("EasytimeGS_UE_Demo");
	}
}
