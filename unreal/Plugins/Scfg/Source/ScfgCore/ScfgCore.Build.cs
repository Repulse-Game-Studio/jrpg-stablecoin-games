// Copyright 2026 Repulse
// The payment core: plain C++20 with no Unreal header outside Private/ScfgCoreModule.cpp, so it also builds under CMake
// (unreal/README.md, "Under UBT"). It keeps the Python reference's names (snake_case, namespace scfg) instead of Epic's
// naming so each file reads against its twin; the Scfg module is the Unreal-facing API.
using UnrealBuildTool;

public class ScfgCore : ModuleRules
{
	public ScfgCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Only for IMPLEMENT_MODULE in Private/ScfgCoreModule.cpp: the public headers include no Unreal header, so no
		// dependent needs Core through us. It also gives the module Core's shared PCH.
		PrivateDependencyModuleNames.Add("Core");
		// bEnableExceptions and bUseRTTI keep their defaults (false), as the core is written for.
	}
}
