// Copyright 2026 Repulse
// The phase 1 conformance runners (unreal/core-tests) as Unreal automation tests, Scfg.Core.* (unreal/README.md,
// "Under UBT"). UncookedOnly in Scfg.uplugin: built into the editor and uncooked targets, never into a cooked game.
using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

public class ScfgCoreTests : ModuleRules
{
	public ScfgCoreTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// unreal/core-tests: the CMake test binary's sources. One copy of the runner code, compiled here in place.
		string CoreTests = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", "..", "core-tests"));
		// "support/Tier1.h" and the rest, and the support/*.cpp that Private/CoreTests/*.cpp include.
		PrivateIncludePaths.Add(CoreTests);
		// Every Tier 1 adapter (tier1/<Topic>.cpp, each registers its topic) is a source of this module, as if it were
		// in it: a new topic file is built with no edit here. UBT can only add a whole directory, so the support files,
		// which share theirs with the CMake-only Test.cpp, Lint.cpp and Locales.cpp, come in through one-line shims.
		if (!ConditionalAddModuleDirectory(new DirectoryReference(Path.Combine(CoreTests, "tier1"))))
		{
			throw new BuildException("ScfgCoreTests: {0} is missing", Path.Combine(CoreTests, "tier1"));
		}
		// Core for the automation framework, Projects for IPluginManager (the vectors are found from the plugin's
		// folder), ScfgCore for the code under test.
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "Projects", "ScfgCore" });
		// bEnableExceptions and bUseRTTI keep their defaults (false), as for ScfgCore.
	}
}
