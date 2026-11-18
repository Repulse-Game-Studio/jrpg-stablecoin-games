// Copyright 2026 Repulse
// The plugin's editor module (Type "Editor": the editor only, never a game): the SCFG project settings' panel
// (Project Settings > Plugins > SCFG) gets a Validate button, which runs UScfgSettings::Validate() and shows the result
// as a notification and on the "SCFG" page of the Message Log (Private/ScfgSettingsDetails.cpp, Public/ScfgSettingsValidation.h).
// A UFUNCTION(CallInEditor) on the settings would not do: Project Settings edits the class default object, and the
// details panel drops CallInEditor buttons for an archetype (ObjectDetails.cpp, AddCallInEditorMethods).
using UnrealBuildTool;

public class ScfgEditor : ModuleRules
{
	public ScfgEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ScfgSettingsValidation.h names UScfgSettings (Scfg) and FScfgSettingsProblem.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Scfg" });
		// PropertyEditor for the details customization, MessageLog for the "SCFG" page, Slate and SlateCore for the
		// button and the notification.
		PrivateDependencyModuleNames.AddRange(new string[] { "MessageLog", "PropertyEditor", "Slate", "SlateCore" });
	}
}
