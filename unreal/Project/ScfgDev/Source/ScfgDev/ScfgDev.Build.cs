using UnrealBuildTool;

public class ScfgDev : ModuleRules
{
	public ScfgDev(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.Add("ScfgCore");
	}
}
