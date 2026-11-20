// Copyright 2026 Repulse
// The showcase's harness hook: a test harness may take over the setup (IScfgShowcaseHarness,
// UScfgShowcaseGameInstance::SetHarnessFactory). A game never sets one; the repository's development project sets
// one outside Shipping, its end-to-end driver, made only when the command line asks for it.
//
// A harness that provides the configuration (its Options() and Items()) points the game at a local test chain; one
// whose OwnConfig() is true leaves the configuration to the showcase (a tour of the chain-free buttons). A harness that
// cannot run, or a configuration that Configure / AddItem refuses under one, makes the game quit with exit code 1 (a
// test run never falls back to another setup); the missing wallet does not, so a harness can check that refusal too.
#pragma once

#include "CoreMinimal.h"

#include "ScfgTypes.h"

// A test harness that takes over the showcase's setup (see above): made by the factory SetHarnessFactory() sets, in
// the game instance's Init(), and released when it shuts down.
class IScfgShowcaseHarness
{
public:
	virtual ~IScfgShowcaseHarness() = default;
	// True: the showcase configures itself, as it does without a harness (Options() and Items() are not used).
	virtual bool OwnConfig() const = 0;
	// Configure()'s options and the items to AddItem(), when OwnConfig() is false.
	virtual const FScfgOptions& Options() const = 0;
	virtual const TMap<FString, FScfgItem>& Items() const = 0;
	// Called once the showcase is configured (or refused for want of a wallet): false (logged) quits with exit code 1.
	virtual bool Start() = 0;
};
