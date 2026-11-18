// Copyright 2026 Repulse
// The development project's game module: the showcase (a minimal game over the Scfg plugin's public API:
// ScfgShowcase.h) and, outside Shipping, the end-to-end driver tools/run_unreal_e2e.py talks to (ScfgE2EDriver.h).
using System.IO;
using UnrealBuildTool;

public class ScfgDev : ModuleRules
{
	public ScfgDev(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		// The showcase: the plugin's public API and a Slate HUD.
		PrivateDependencyModuleNames.AddRange(new string[] { "InputCore", "Scfg", "ScfgCore", "Slate", "SlateCore" });
		if (Target.Configuration != UnrealTargetConfiguration.Shipping)
		{
			// The e2e driver (compiled out of Shipping): it reads what the player sees from the plugin's screens, which
			// are not the plugin's public API (Scfg's Private/, the classes carry SCFG_API), drives them through
			// FSlateApplication, reads its config with Json, talks to the runner over a loopback socket, and reads the
			// clipboard (ApplicationCore) and the HTTP transport's request count (HTTP, for its header).
			PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(PluginsDirectory(), "Scfg", "Source", "Scfg", "Private")));
			PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "Json", "Sockets" });
		}
	}

	// unreal/Plugins (the project's AdditionalPluginDirectories entry).
	private string PluginsDirectory()
	{
		return Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Plugins");
	}
}
