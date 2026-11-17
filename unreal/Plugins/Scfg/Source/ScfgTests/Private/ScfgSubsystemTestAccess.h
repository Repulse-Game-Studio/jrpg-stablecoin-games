// No Python twin: UScfgSubsystem's facade with the tests' hooks (the subsystem's friend), shared by the Blueprint and
// the sheet tests.
#pragma once

#include "CoreMinimal.h"

#include "ScfgFacade.h"
#include "ScfgSettings.h"
#include "ScfgSubsystem.h"

struct FScfgSubsystemTestAccess
{
	// Without settings: the subsystem's tests do not depend on the dev project's Config/DefaultGame.ini.
	static void Start(UScfgSubsystem& Subsystem, const FScfgFacadeOptions& Options) { Subsystem.StartFacade(Options, nullptr); }
	// As Initialize does, with these settings (GetDefault<UScfgSettings>() there).
	static void Start(UScfgSubsystem& Subsystem, const FScfgFacadeOptions& Options, const UScfgSettings* Settings)
	{
		Subsystem.StartFacade(Options, Settings);
	}
};
