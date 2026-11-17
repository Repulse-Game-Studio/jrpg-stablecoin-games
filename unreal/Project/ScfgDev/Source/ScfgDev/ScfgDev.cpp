#include "ScfgDev.h"

#include "EngineAnalytics.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Scfg/Amounts.h"
#include "Scfg/Json.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ScfgDev, "ScfgDev");

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

#endif
