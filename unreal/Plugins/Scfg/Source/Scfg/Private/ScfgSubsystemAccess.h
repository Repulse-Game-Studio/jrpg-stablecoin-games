// No Python twin: the plugin's own access to UScfgSubsystem's private parts (its friend), for the plugin's modules
// (the async nodes, the tests, the development project's e2e driver), never a game's: the facade (a Private type) and
// the async starts that leave the teardown to the caller.
#pragma once

#include "CoreMinimal.h"

#include "ScfgSubsystem.h"

class FScfgFacade;

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
};
