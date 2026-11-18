// Copyright 2026 Repulse
#include "ScfgDev.h"

#include "ScfgDevLog.h"

#include "EngineAnalytics.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Scfg/Amounts.h"
#include "Scfg/Json.h"
#include "ScfgSettings.h"
#include "ScfgShowcase.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ScfgDev, "ScfgDev");

DEFINE_LOG_CATEGORY(LogScfgDev);

#if WITH_DEV_AUTOMATION_TESTS

// Proves that the core links into, and runs inside, the engine process: a call across the module boundary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgDevSmokeTest, "ScfgDev.Smoke", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FScfgDevSmokeTest::RunTest(const FString& Parameters)
{
	scfg::Result<scfg::Dec> Price = scfg::amounts::parse_amount(scfg::Json("4.99"));
	if (!TestTrue(TEXT("parse_amount(\"4.99\") succeeds"), Price.ok()))
	{
		return false;
	}
	TestEqual(TEXT("parse_amount(\"4.99\")"), FString(UTF8_TO_TCHAR(Price.value().str().c_str())), FString(TEXT("4.99")));
	return true;
}

// Our editor runs reach nothing beyond localhost (Config/DefaultEditorSettings.ini, Config/DefaultEngine.ini): no usage
// analytics, no crash report upload from an unattended run that crashes, and no Home panel, whose connectivity check
// fetches https://www.google.com/generate_204. It holds in any run, unattended or not (the session frontend's too).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgDevNoTelemetryTest, "ScfgDev.NoTelemetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FScfgDevNoTelemetryTest::RunTest(const FString& Parameters)
{
	bool bSendUsageData = true;
	bool bSendUnattendedBugReports = true;
	// GEditorSettingsIni: BaseEditorSettings.ini, then the project's DefaultEditorSettings.ini, then the per-user
	// EditorSettings.ini that every 5.8 project shares, which would win
	TestTrue(TEXT("bSendUsageData is set"), GConfig->GetBool(TEXT("/Script/UnrealEd.AnalyticsPrivacySettings"),
		TEXT("bSendUsageData"), bSendUsageData, GEditorSettingsIni));
	TestTrue(TEXT("bSendUnattendedBugReports is set"), GConfig->GetBool(TEXT("/Script/UnrealEd.CrashReportsPrivacySettings"),
		TEXT("bSendUnattendedBugReports"), bSendUnattendedBugReports, GEditorSettingsIni));
	TestFalse(TEXT("no usage analytics"), bSendUsageData);
	TestFalse(TEXT("no unattended crash report upload"), bSendUnattendedBugReports);
	TestFalse(TEXT("no analytics provider was started"), FEngineAnalytics::IsAvailable());
	const IConsoleVariable* HomeScreen = IConsoleManager::Get().FindConsoleVariable(TEXT("HomeScreen.EnableHomeScreen"));
	if (TestNotNull(TEXT("the HomeScreen.EnableHomeScreen cvar"), HomeScreen))
	{
		TestFalse(TEXT("no Home panel (no connectivity check to www.google.com)"), HomeScreen->GetBool());
	}
	return true;
}

// The showcase's configuration without code (Config/DefaultGame.ini, [/Script/Scfg.ScfgSettings]) is what the plugin
// applies at start: valid as Validate() checks it, with a wallet (so it is applied), on testnet, with the item the e2e
// unlocks, and a logo a packaged game has too (no Validate warning). tests/unreal_showcase plays it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgDevShowcaseSettingsTest, "ScfgDev.ShowcaseSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FScfgDevShowcaseSettingsTest::RunTest(const FString& Parameters)
{
	const UScfgSettings* Settings = GetDefault<UScfgSettings>();
	for (const FScfgSettingsProblem& Problem : Settings->Validate())
	{
		AddError(FString::Printf(TEXT("the project settings: %s: %s"), *Problem.Code, *Problem.Message));
	}
	TestFalse(TEXT("they have a wallet, so the plugin applies them"), Settings->IsEmpty());
	TestTrue(TEXT("testnet: no real money moves"), Settings->Options.bTestnet);
	TestTrue(TEXT("no Solana wallet: the start checks nothing over the network"), Settings->Options.SolanaWallet.IsEmpty());
	TArray<FString> Ids;
	for (const FScfgSettingsItem& Item : Settings->Items)
	{
		Ids.Add(Item.Id);
	}
	TestEqual(TEXT("the showcase's three items"), FString::Join(Ids, TEXT(",")), FString(TEXT("chapter2,soundtrack,supporter")));
	TestTrue(TEXT("the e2e's item among them"), Ids.Contains(UScfgShowcaseGameInstance::ITEM));
	TestEqual(TEXT("a logo"), Settings->Options.Logo, FString(TEXT("Content/Showcase/logo.png")));
	TestTrue(TEXT("whose folder the packaging settings stage (Additional Non-Asset Directories to Package)"),
		UScfgSettings::StagedDirectories().Contains(TEXT("Showcase")));
	for (const FString& Warning : Settings->Warnings())
	{
		AddError(FString::Printf(TEXT("the project settings warn: %s"), *Warning));
	}
	return true;
}

#endif
