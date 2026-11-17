// No Python twin: the Scfg glue module (unreal/README.md, "The glue module").
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// The module of the Unreal glue. It has no startup work of its own yet: the worker, the transport and the store
// are made by their owner (the game-instance subsystem, task 5) and torn down with it.
class FScfgModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
