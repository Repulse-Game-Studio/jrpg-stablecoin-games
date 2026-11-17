// The Unreal glue around the payment core (unreal/README.md, "The glue module"): the worker that drives the core's
// Engine on the game thread, the FHttpModule transport, the UTF-8 bridge, and (later tasks) the store, the facade,
// the checkout server and the UI.
using UnrealBuildTool;

public class Scfg : ModuleRules
{
	public Scfg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ScfgModule.h needs only Core. The rest is private: the core's headers and the HTTP module stay out of
		// a dependent's include path (the glue never exposes scfg::Co or scfg::Future).
		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "HTTP", "ScfgCore" });
		// bEnableExceptions and bUseRTTI keep their defaults (false).
	}
}
