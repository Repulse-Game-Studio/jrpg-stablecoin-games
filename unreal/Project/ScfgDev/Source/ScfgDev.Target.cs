using UnrealBuildTool;

public class ScfgDevTarget : TargetRules
{
	public ScfgDevTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		// Every warning in our modules is an error. The engine is installed (precompiled), so this target shares
		// its build environment, and UBT refuses a changed setting there unless it is overridden; the override only
		// reaches the modules UBT compiles here, which are ours.
		bWarningsAsErrors = true;
		bOverrideBuildEnvironment = true;
		ExtraModuleNames.Add("ScfgDev");
	}
}
