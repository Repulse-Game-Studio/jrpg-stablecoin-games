// Copyright 2026 Repulse
// No Python twin: the project settings blanked for a test that makes a real game instance. (A subsystem's facade
// with the tests' hooks is FScfgSubsystemAccess::StartFacade, ScfgSubsystemAccess.h.)
#pragma once

#include "CoreMinimal.h"

#include "ScfgFacade.h"
#include "ScfgSettings.h"
#include "ScfgSubsystem.h"
#include "ScfgSubsystemAccess.h"

// The project's settings (the class default, which holds the dev project's Config/DefaultGame.ini: the showcase's, with a
// wallet) blanked for the guard's scope (UScfgSettings::NewOptions(), no chains, no items), then put back. Every test
// that makes a real game instance (InitializeStandalone, whose UScfgSubsystem::Initialize applies them) holds one from
// before the instance is made until after it is shut down, so that instance's own facade (the default store, the real
// HTTP transport, the core ticker) stays unconfigured and never starts payments on the project's Saved/Scfg/state.json
// (ScfgTestsModule.cpp fails a test that opens it).
struct FScfgBlankProjectSettings
{
	FScfgBlankProjectSettings()
		: Project(GetMutableDefault<UScfgSettings>())
		, Options(Project->Options)
		, Chains(Project->Chains)
		, Items(Project->Items)
	{
		Project->Options = UScfgSettings::NewOptions();
		Project->Chains.Reset();
		Project->Items.Reset();
	}
	FScfgBlankProjectSettings(const FScfgBlankProjectSettings&) = delete;
	FScfgBlankProjectSettings& operator=(const FScfgBlankProjectSettings&) = delete;
	~FScfgBlankProjectSettings()
	{
		Project->Options = Options;
		Project->Chains = Chains;
		Project->Items = Items;
	}

	UScfgSettings* const Project;
	const FScfgOptions Options;
	const TArray<FScfgSettingsChain> Chains;
	const TArray<FScfgSettingsItem> Items;
};
