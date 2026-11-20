// Copyright 2026 Repulse
// No Python twin: the plugin's own access to UScfgBlueprintLibrary's state, for the plugin's tests (outside Shipping
// only), never a game's.
#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING
struct FScfgBlueprintLibraryAccess
{
	// A node without a game instance logs one error per node kind per session (a UMG binding is evaluated every
	// frame): this forgets which kinds logged, so a test starts each check afresh.
	static SCFG_API void ResetLogOnce();
};
#endif
