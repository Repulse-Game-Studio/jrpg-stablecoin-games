// Copyright 2026 Repulse
using UnrealBuildTool;

public class ScfgDevEditorTarget : TargetRules
{
	public ScfgDevEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		// Every warning in our modules is an error. The engine is installed (precompiled), so this target shares
		// its build environment, and UBT refuses a changed setting there unless it is overridden; the override only
		// reaches the modules UBT compiles here, which are ours.
		bWarningsAsErrors = true;
		bOverrideBuildEnvironment = true;
		// A unity build is a unity build. UBT's adaptive unity takes the files it thinks are being edited out of the
		// unity file; with an installed engine and no .git in the project's folder or its parent it takes every
		// writable file, so without this no core file would ever be built in Module.ScfgCore.cpp.
		bUseAdaptiveUnityBuild = false;
		ExtraModuleNames.Add("ScfgDev");
	}
}
