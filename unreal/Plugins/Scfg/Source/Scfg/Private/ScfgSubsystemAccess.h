// Copyright 2026 Repulse
// No Python twin: the plugin's own access to UScfgSubsystem's private parts (its friend), for the plugin's own code
// (the async nodes, the tests) and the development project's end-to-end driver, never a game's: the facade (a Private type), the async starts that leave the teardown
// to the caller, and, outside Shipping, the facade's start with the tests' options.
#pragma once

#include "CoreMinimal.h"

#include "ScfgGuard.h"
#include "ScfgSubsystem.h"

class FScfgFacade;
class UScfgSettings;
struct FScfgFacadeOptions;

struct FScfgSubsystemAccess
{
	// The flows; null outside Initialize..Deinitialize.
	static FScfgFacade* Facade(const UScfgSubsystem& Subsystem) { return Subsystem.Facade.Get(); }

	// UScfgSubsystem::PayAsync / RestoreAsync with OnResolved called at the teardown too (the nodes drop it there).
	static void PayAsyncForNode(UScfgSubsystem& Subsystem, EScfgPayKind Kind, const FScfgPayRequest& Request,
		TFunction<void(const FScfgPaymentResult&)> OnResolved)
	{
		Subsystem.StartPayAsync(Kind, Request, MoveTemp(OnResolved));
	}
	static void RestoreAsyncForNode(UScfgSubsystem& Subsystem, TFunction<void(const FScfgRestoreResult&)> OnResolved)
	{
		Subsystem.StartRestoreAsync(MoveTemp(OnResolved), nullptr);
	}

#if !UE_BUILD_SHIPPING
	// A new facade with these options (the tests' fakes), then Settings applied as Initialize applies the project's
	// (null: none, so a test does not depend on the project's Config/DefaultGame.ini).
	static void StartFacade(UScfgSubsystem& Subsystem, const FScfgFacadeOptions& Options,
		const UScfgSettings* Settings = nullptr)
	{
		Subsystem.StartFacade(Options, Settings);
	}
	// How many calls, process-wide, were refused because they came from another thread (each logged once).
	static int32 NumOffThreadCalls() { return ScfgGuard::NumOffThreadCalls(); }
#endif
};
