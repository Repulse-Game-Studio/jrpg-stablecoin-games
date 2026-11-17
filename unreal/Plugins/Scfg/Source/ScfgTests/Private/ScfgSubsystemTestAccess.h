// No Python twin: UScfgSubsystem's facade with the tests' hooks (the subsystem's friend), shared by the Blueprint and
// the sheet tests.
#pragma once

#include "CoreMinimal.h"

#include "ScfgFacade.h"
#include "ScfgSubsystem.h"

struct FScfgSubsystemTestAccess
{
	static void Start(UScfgSubsystem& Subsystem, const FScfgFacadeOptions& Options) { Subsystem.StartFacade(Options); }
};
