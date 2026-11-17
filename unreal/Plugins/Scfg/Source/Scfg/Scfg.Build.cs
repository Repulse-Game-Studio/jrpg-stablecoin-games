// The Unreal glue around the payment core (unreal/README.md, "The glue module"): the worker that drives the core's
// Engine on the game thread, the FHttpModule transport, the UTF-8 bridge, the store (state.json), the facade with its
// game-instance subsystem and Blueprint API, the localhost checkout server, and (later tasks) the UI.
using System.IO;
using UnrealBuildTool;

public class Scfg : ModuleRules
{
	public Scfg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ (the subsystem, the async nodes, the Blueprint library, the value types, the sheet host seam) needs
		// Core, CoreUObject and Engine. The rest is private: the core's headers and the HTTP module stay out of a
		// dependent's include path (the glue never exposes scfg::Co or scfg::Future); ApplicationCore for the
		// clipboard.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		// Sockets for the localhost checkout server's listener (ScfgLocalServer.h), Projects to find the plugin's
		// Resources/Web/.
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "Projects", "ScfgCore", "Sockets" });
		// bEnableExceptions and bUseRTTI keep their defaults (false).
		// The checkout page's files the server serves (local_server::ASSETS; copies of plugin/game/scfg/web/, kept by
		// tools/sync_web_page.py), staged with a packaged game as loose files at the plugin's own path, where
		// FScfgLocalServer::WebDirectory() finds them. bridge.js is the web exports' bridge: never served, not staged.
		foreach (string Asset in new string[] { "index.html", "app.js", "evm.js", "solana.js" })
		{
			RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "Web", Asset), StagedFileType.NonUFS);
		}
	}
}
