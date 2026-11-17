// No Python twin: the module registration of ScfgTests; its tests are the Scfg*Tests.cpp files beside it.
//
// THE PROJECT'S OWN STATE FILE. Every facade of the tests has a state directory of its own (FWorld's temporary one).
// While an automation test runs, a store that opens the project's Saved/Scfg/state.json (FScfgStore::DefaultPath(),
// what a game instance's own facade uses: a test that made a real game instance without FScfgBlankProjectSettings,
// ScfgSubsystemTestAccess.h, and whose project settings then started payments) is refused the file unread and logs an
// Error, which fails that test. tools/run_unreal_tests.py also fails a run whose log shows that path opened at all
// (between two tests, where no test is there to fail).
#include "Misc/AutomationTest.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#include "ScfgLog.h"
#include "ScfgStore.h"

class FScfgTestsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FScfgStore::DefaultPathHookForTests() = [](const FString& Path)
		{
			if (!GIsAutomationTesting)
			{
				return false;  // an uncooked game of the dev project (the e2e) loads this module too: its store is its own
			}
			UE_LOG(LogScfg, Error, TEXT("scfg tests: a test opened the project's own payment state file %s (refused): "
				"start it from blank project settings (FScfgBlankProjectSettings) or give its facade a StoreDirectory"), *Path);
			return true;
		};
	}

	virtual void ShutdownModule() override
	{
		FScfgStore::DefaultPathHookForTests() = nullptr;
	}
};

IMPLEMENT_MODULE(FScfgTestsModule, ScfgTests)
