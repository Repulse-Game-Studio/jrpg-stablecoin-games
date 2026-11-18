// Copyright 2026 Repulse
// No Python twin: the transport the worker owns (scfg::coio::Transport, Coio.h), plus the shutdown that ends the
// teardown order Jobs -> Engine -> transport (Co.h, "Teardown order").
#pragma once

#include "CoreMinimal.h"

#include "Scfg/Coio.h"

// A core transport the worker can shut down. FScfgHttpTransport is the real one; the tests have fakes.
class SCFG_API FScfgTransport : public scfg::coio::Transport
{
public:
	// Ends every request in flight: its promise is dropped (CancelledError) and nothing of it may call back later.
	// Every later post_json is a CancelledError at once. The worker calls it after it destroyed its Jobs and its
	// Engine, so no coroutine waits on a dropped promise any more.
	virtual void Shutdown() = 0;
};
