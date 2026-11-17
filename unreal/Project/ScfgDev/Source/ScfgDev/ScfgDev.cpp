#include "ScfgDev.h"

#include "Misc/AutomationTest.h"
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

#endif
