// Copyright 2026 Repulse
// The showcase's module (ScfgShowcase.h): a minimal game over the Scfg plugin's public API, its game instance, game
// mode, player controller and Slate HUD. Nothing of the plugin's Private/ is used, so it builds in any project that
// has the Scfg plugin next to it.
using System.IO;
using UnrealBuildTool;

public class ScfgShowcase : ModuleRules
{
	public ScfgShowcase(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ScfgShowcase.h: the game framework classes, the HUD (an SCompoundWidget) and the plugin's value types.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Scfg", "SlateCore" });
		// The HUD's widgets and keys, and Projects to find this plugin's own folder (the logo, below).
		PrivateDependencyModuleNames.AddRange(new string[] { "InputCore", "Projects", "Slate" });
		// The logo ConfigureInCode() passes (ScfgShowcase.cpp, LOGO_FILE), a loose file the cooker does not stage by
		// itself: staged with a packaged game at the plugin's own path, where the game instance finds it, as Scfg.Build.cs
		// stages the checkout page. A project that configures the showcase from its own settings names its own logo.
		// (Configure() takes only a logo inside the project folder: the plugin must be in the project's Plugins folder.)
		RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "logo.png"), StagedFileType.NonUFS);
	}
}
