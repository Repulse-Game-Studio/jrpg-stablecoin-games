// No Python twin: the Validate button's action on the SCFG project settings (ScfgSettings.h), Unity's
// ScfgSettingsEditor.Verdict: the core's validation, shown on the "SCFG" page of the Message Log and, from the button,
// as a notification. The check itself is UScfgSettings::Validate() (no UI), which the tests call too.
#pragma once

#include "CoreMinimal.h"

#include "ScfgSettings.h"

namespace ScfgEditor
{

// The Message Log page the results go to ("SCFG"), registered by the module.
SCFGEDITOR_API extern const FName MessageLogName;

// Settings.Validate(), posted to a new page of the "SCFG" Message Log: an error line per problem ("<code>:
// <message>"), else an info line saying they are valid, then a warning when they are not applied (no wallet) or use
// the test networks. Not echoed to the output log. With bNotify, also a notification (success or failure, with a link
// to the log). Returns the problems.
SCFGEDITOR_API TArray<FScfgSettingsProblem> ValidateSettings(const UScfgSettings& Settings, bool bNotify);

}  // namespace ScfgEditor
