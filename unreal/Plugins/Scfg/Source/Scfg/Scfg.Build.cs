// The Unreal glue around the payment core (unreal/README.md, "The glue module"): the worker that drives the core's
// Engine on the game thread, the FHttpModule transport, the UTF-8 bridge, the store (state.json), the facade with its
// game-instance subsystem and Blueprint API, and (later tasks) the checkout server and the UI.
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
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "ScfgCore" });
		// bEnableExceptions and bUseRTTI keep their defaults (false).
	}
}
