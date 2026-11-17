// No Python twin: the ScfgEditor module (ScfgEditor.Build.cs): registers the "SCFG" Message Log page and the settings'
// details customization, and the Validate action (ScfgSettingsValidation.h).
#include "ScfgSettingsDetails.h"
#include "ScfgSettingsValidation.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Logging/MessageLog.h"
#include "MessageLogInitializationOptions.h"
#include "MessageLogModule.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ScfgEditor"

const FName ScfgEditor::MessageLogName(TEXT("SCFG"));

TArray<FScfgSettingsProblem> ScfgEditor::ValidateSettings(const UScfgSettings& Settings, bool bNotify)
{
	const TArray<FScfgSettingsProblem> Problems = Settings.Validate();
	const TArray<FString> Lines = Settings.Verdict(Problems);
	{
		FMessageLog Log(MessageLogName);
		Log.SuppressLoggingToOutputLog(true);
		Log.NewPage(FText::Format(LOCTEXT("Page", "Validate {0}"), FText::AsTime(FDateTime::Now())));
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const FText Line = FText::FromString(Lines[I]);
			if (I < Problems.Num())
			{
				Log.Error(Line);
			}
			else if (I == 0)
			{
				Log.Info(Line);  // valid
			}
			else
			{
				Log.Warning(Line);  // not applied, or testnet
			}
		}
	}  // (flushed to the page here)
	if (bNotify)
	{
		FNotificationInfo Info(Problems.Num() == 0
			? LOCTEXT("Valid", "SCFG settings are valid")
			: FText::Format(LOCTEXT("Invalid", "SCFG settings: {0} {0}|plural(one=problem,other=problems)"), Problems.Num()));
		Info.SubText = FText::FromString(Lines[0]);
		Info.ExpireDuration = 8.f;
		Info.bUseSuccessFailIcons = true;
		Info.Hyperlink = FSimpleDelegate::CreateLambda([]() { FMessageLog(MessageLogName).Open(EMessageSeverity::Info, true); });
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show the SCFG log");
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(Problems.Num() == 0 ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}
	return Problems;
}

class FScfgEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMessageLogModule& MessageLog = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
		FMessageLogInitializationOptions Options;
		Options.bShowPages = true;
		Options.bAllowClear = true;
		Options.MaxPageCount = 10;
		MessageLog.RegisterLogListing(ScfgEditor::MessageLogName, LOCTEXT("LogLabel", "SCFG"), Options);

		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		SettingsClass = UScfgSettings::StaticClass()->GetFName();
		PropertyEditor.RegisterCustomClassLayout(SettingsClass,
			FOnGetDetailCustomizationInstance::CreateStatic(&FScfgSettingsDetails::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();
	}

	virtual void ShutdownModule() override
	{
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyEditor->UnregisterCustomClassLayout(SettingsClass);
			PropertyEditor->NotifyCustomizationModuleChanged();
		}
		if (FMessageLogModule* MessageLog = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog")))
		{
			MessageLog->UnregisterLogListing(ScfgEditor::MessageLogName);
		}
	}

private:
	FName SettingsClass;  // (no StaticClass() at shutdown)
};

IMPLEMENT_MODULE(FScfgEditorModule, ScfgEditor)

#undef LOCTEXT_NAMESPACE
