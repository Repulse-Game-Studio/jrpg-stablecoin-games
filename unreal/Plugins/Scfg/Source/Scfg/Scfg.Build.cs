// The Unreal glue around the payment core (unreal/README.md, "The glue module"): the worker that drives the core's
// Engine on the game thread, the FHttpModule transport, the UTF-8 bridge, the store (state.json), the facade with its
// game-instance subsystem and Blueprint API, the localhost checkout server, and the Slate screens (Private/UI).
using System.IO;
using UnrealBuildTool;

public class Scfg : ModuleRules
{
	public Scfg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ (the subsystem, the async nodes, the Blueprint library, the value types, the sheet host seam, the
		// default Slate host and its UMG wrapper) needs Core, CoreUObject, Engine, SlateCore (ISlateStyle) and UMG
		// (UScfgSheetWidget is a UUserWidget). The rest is private: the core's headers and the HTTP module stay out
		// of a dependent's include path (the glue never exposes scfg::Co or scfg::Future); ApplicationCore for the
		// clipboard.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "SlateCore", "UMG" });
		// DeveloperSettings for the project settings (UScfgSettings, Public/ScfgSettings.h, a UDeveloperSettings).
		PublicDependencyModuleNames.Add("DeveloperSettings");
		// Sockets for the localhost checkout server's listener (ScfgLocalServer.h), Projects to find the plugin's
		// Resources/Web/; Slate and InputCore for the screens (Private/UI), ImageWrapper to read the logo.
		// The same list on every platform of Scfg.uplugin, Android and iOS included: each is an engine runtime module
		// with an Android and an iOS implementation (Sockets: Private/Android and Private/IOS socket subsystems), and
		// the glue's code is platform-neutral. On a phone the server is never started (no browser method: the
		// controller's mobile flag), so only the page's files are left out there (below). Unverified: no Android or
		// iOS SDK on the development machine (unreal/README.md, "Mobile").
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "ImageWrapper", "InputCore", "Projects", "ScfgCore", "Slate", "Sockets" });
		// bEnableExceptions and bUseRTTI keep their defaults (false).
		// The checkout page's files the server serves (local_server::ASSETS; copies of plugin/game/scfg/web/, kept by
		// tools/sync_web_page.py), staged with a packaged game as loose files at the plugin's own path, where
		// FScfgLocalServer::WebDirectory() finds them. bridge.js is the web exports' bridge: never served, not staged.
		// Desktop games only: a phone has no localhost page.
		if (Target.Platform.IsInGroup(UnrealPlatformGroup.Desktop))
		{
			foreach (string Asset in new string[] { "index.html", "app.js", "evm.js", "solana.js" })
			{
				RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "Web", Asset), StagedFileType.NonUFS);
			}
		}
	}
}
