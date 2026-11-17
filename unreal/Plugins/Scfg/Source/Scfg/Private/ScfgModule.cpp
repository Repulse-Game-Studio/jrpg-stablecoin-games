// No Python twin: the Scfg module's registration (see ScfgModule.h) and its log category.
#include "ScfgModule.h"

#include "ScfgLog.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogScfg);

void FScfgModule::StartupModule()
{
}

void FScfgModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FScfgModule, Scfg)
