// The glue module's automation tests, Scfg.Glue.* (unreal/README.md, "The glue module"). UncookedOnly in
// Scfg.uplugin, as ScfgCoreTests: built into the editor and uncooked targets, never into a cooked game
// (tools/run_unreal_build.py fails a game build that links it).
using System.IO;
using UnrealBuildTool;

public class ScfgTests : ModuleRules
{
	public ScfgTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// The glue's classes under test live in Scfg's Private/ (they are not the plugin's public API); they carry
		// SCFG_API for this module.
		PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "Scfg", "Private")));
		// HTTPServer only for the local JSON-RPC endpoint of the real HTTP test (bound to 127.0.0.1); Sockets for the raw
		// loopback client of the checkout page tests, Projects to find the plugin's folders.
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "HTTP", "HTTPServer", "Projects", "Scfg", "ScfgCore", "Sockets" });
	}
}
