// Copyright 2026 Repulse
// The payment core: plain C++20, no Unreal header outside Private/ScfgCoreModule.cpp (unreal/README.md, "Under UBT").
using UnrealBuildTool;

public class ScfgCore : ModuleRules
{
	public ScfgCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Public"));
		// Only for IMPLEMENT_MODULE in Private/ScfgCoreModule.cpp: the public headers include no Unreal header, so no
		// dependent needs Core through us. It also gives the module Core's shared PCH.
		PrivateDependencyModuleNames.Add("Core");
		// bEnableExceptions and bUseRTTI keep their defaults (false), as the core is written for.
	}
}
