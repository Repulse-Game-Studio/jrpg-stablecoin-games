// Copyright 2026 Repulse
// The development project's game module: the smoke tests and, outside Shipping, the end-to-end driver
// tools/run_unreal_e2e.py talks to (ScfgE2EDriver.h), which plays the showcase. The showcase itself is a plugin of its
// own (unreal/Plugins/ScfgShowcase, loaded through the project's AdditionalPluginDirectories, as Scfg is), which uses
// only the plugin's public API and knows nothing of the driver: this module hands it the driver through its harness
// hook (UScfgShowcaseGameInstance::SetHarnessFactory, ScfgDev.cpp).
using System.IO;
using UnrealBuildTool;

public class ScfgDev : ModuleRules
{
	public ScfgDev(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		// The smoke tests (the core's API and the project settings) and the showcase's game instance (its harness hook
		// and its constants, for the tests).
		PrivateDependencyModuleNames.AddRange(new string[] { "Scfg", "ScfgCore", "ScfgShowcase" });
		if (Target.Configuration != UnrealTargetConfiguration.Shipping)
		{
			// The e2e driver (compiled out of Shipping): it reads what the player sees from the showcase's HUD and the
			// plugin's screens, which are not the plugin's public API (Scfg's Private/, the classes carry SCFG_API),
			// drives them through FSlateApplication (Slate, SlateCore, InputCore), reads its config with Json, talks to
			// the runner over a loopback socket, and reads the clipboard (ApplicationCore) and the HTTP transport's
			// request count (HTTP, for its header).
			PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(PluginsDirectory(), "Scfg", "Source", "Scfg", "Private")));
			PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "InputCore", "Json", "Slate", "SlateCore", "Sockets" });
		}
	}

	// unreal/Plugins (the project's AdditionalPluginDirectories entry).
	private string PluginsDirectory()
	{
		return Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Plugins");
	}
}
