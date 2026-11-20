// Copyright 2026 Repulse
// No Python twin: the glue's shared misuse helpers, in one place: the thread guard of the public
// calls, the code-or-kind rule of a Failed result, and the Failed "RuntimeError" results of a call that could not run.
//
// THE THREAD GUARD. UScfgSubsystem checks every public call with OffGameThread() before it touches anything, so the
// facade's own check (the same function) is reached only by the plugin's tests, which drive FScfgFacade directly. A
// call from another thread logs "scfg: <What> must be called on the game thread" every time (an ensure reports only
// once per process), is counted outside Shipping (NumOffThreadCalls), and raises one ensure with its callstack; the caller then
// returns its Failed result or neutral value. The worker, the store and the transport are not public: they are
// reached only from the facade on the game thread and keep check(IsInGameThread()) as an assertion of that.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#include "ScfgTypes.h"

#include "Scfg/Err.h"

namespace ScfgGuard
{

// True off the game thread (logged, counted, ensured: see above).
SCFG_API bool OffGameThread(const TCHAR* What);
#if !UE_BUILD_SHIPPING
// How many calls, process-wide, OffGameThread() refused (the plugin's tests read it).
SCFG_API int32 NumOffThreadCalls();
#endif
// Fn runs on the game thread at the next tick of the core ticker (FTSTicker::AddTicker may be called from any
// thread): an off-thread Pay or Restore gets its Failed result there, never on the calling thread.
SCFG_API void OnGameThread(TFunction<void()> Fn);

// A ConfigError's code, else the Err's kind ("TypeError"): what a Failed result's Error and a configuration call
// return.
SCFG_API FString ErrCode(const scfg::Err& Error);
// The same, "" for none.
SCFG_API FString ErrCode(const TOptional<scfg::Err>& Error);
// A ConfigError of the core whose message names Python's keywords (wallet.missing: "configure() needs wallet= ...",
// chain.needs_evm_wallet) with Unreal's names instead (FScfgOptions' Wallet / SolanaWallet, the project settings'
// fields); any other Err as it is. What Configure() and the settings' Validate() report.
SCFG_API scfg::Err EngineSpelling(const scfg::Err& Error);

// The code a call that could not run (another thread, no running subsystem) returns, and its Failed result's Error.
inline constexpr const TCHAR* NotRunningCode = TEXT("RuntimeError");

// Failed "RuntimeError": a Pay that could not run (another thread, no running subsystem).
SCFG_API FScfgPaymentResult PaymentNotRunning(
	EScfgPayKind Kind = EScfgPayKind::Unlock, const FString& Item = FString(), const FString& Token = FString());
// The same for a Restore.
SCFG_API FScfgRestoreResult RestoreNotRunning();

}  // namespace ScfgGuard
