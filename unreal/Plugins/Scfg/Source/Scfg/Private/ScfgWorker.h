// Twin of scfg_core/worker.py's Worker and godot/addons/scfg/core/worker.gd's ScfgWorker: the single owner of the
// Engine, which it drives on the game thread from FTSTicker, one command at a time.
//
// The facade never touches the Engine's commands directly. It:
//   - submits commands (StartSession, ReportHint, Tick, ...), each with a completion callback that gets the core's
//     Result<T> (the glue never sees a Co or a Future);
//   - reads the latest detached Snapshot (GetSnapshot), replaced after every command, and hooks OnStateChanged (the
//     store saves from it, task 4);
//   - collects the tick's StepOutcomes with DrainOutcomes() (a "waiting" one only when it has an error).
//
// ONE COMMAND AT A TIME. The commands wait in a FIFO; the next starts when the running one has finished, as worker.py
// and worker.gd never interleave jobs (the Engine's busy flag would abort an overlap; it is checked before each
// start). A network command (StartSession, BuildSolanaPayment, Tick, Reverify, RestoreLookup) runs as an scfg::Job
// and finishes when the transport completes its last request; the others finish at once.
//
// TICKS. A tick is a command too, but coalesced, and it yields to queued commands: at most one tick is pending, and
// it starts only when no command is queued (worker.py drains its queue before the tick of a pass). A tick is
// requested by Tick() (callers' callbacks gather on the pending one), by every other command (worker.py's submit
// wakes the loop, so a command is followed by a tick), by Start(), and by the clock: Interval() seconds (default
// POLL_INTERVAL, worker.py's 15.0) after the last tick ended. The very first tick runs Engine::reverify() before
// Engine::tick() (worker.py's first run_once); when that reverify fails, the tick fails with its Err and the reverify
// is not retried (worker.gd). A failing tick or reverify is reported to OnError; the next tick comes an interval later.
//
// CALLBACKS. Every completion callback and OnStateChanged run on the game thread from the worker's ticker, never
// inside a call to the worker (a sync command's callback comes on the next frame), and in this order: the snapshot is
// refreshed, OnStateChanged fires, then the command's callback. A callback may submit commands, Stop() or Start()
// the worker, or destroy it (the worker notices and touches nothing more). OnError and the Engine's contained errors
// run inside the running command's coroutine: they must not destroy the worker (checked); a Stop() there is deferred
// to the next frame.
//
// START AND STOP. Commands submitted before Start() wait for it. Stop() cancels the running command (its Job is
// destroyed, which cancels its coroutine) and every queued one: each callback gets CancelledError, on the next
// frame; no tick runs until Start() again, and a command submitted while stopped is cancelled the same way. (worker.gd
// lets a pass already in flight finish; Unreal's quit has no later frame for it.) Destroying the worker cancels
// everything without calling back, then tears down in the order Jobs -> Engine -> transport (Shutdown()).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Delegates/Delegate.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#include "ScfgTransport.h"

#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// FScfgWorker's options (FScfgWorker::FOptions).
struct FScfgWorkerOptions
{
	// Seconds between ticks, read when a tick ends (the facade shortens it while a sheet is open). Default:
	// FScfgWorker::POLL_INTERVAL.
	TFunction<double()> Interval;
	// The Engine's clock: whole seconds (session timestamps and windows). Default: Unix time.
	TFunction<int64()> Clock;
	// A failing tick or reverify, a failing snapshot, and the Engine's contained errors (Engine::on_error). Default:
	// logged once to LogScfg. A contained error is reported from inside the running command's coroutine (see
	// FScfgWorker's CALLBACKS).
	TFunction<void(const scfg::Err&)> OnError;
	// The ticker that drives the worker; it must outlive the worker. Default: FTSTicker::GetCoreTicker(). The
	// tests pass their own and tick it by hand.
	FTSTicker* Ticker = nullptr;
	// Passed to the Engine (Engine.h): the chains' RPC objects, and the randomness (default: the core's
	// SystemEntropy, the OS source; it must outlive the worker).
	scfg::engine::RpcFactory RpcFactory;
	scfg::Entropy* Entropy = nullptr;
};

class SCFG_API FScfgWorker
{
public:
	// worker.py's default interval: seconds between the end of one tick and the next, unless a command wakes it.
	static constexpr double POLL_INTERVAL = 15.0;

	// A command's completion callback (TOnDone<void> gets a Status).
	template <class T>
	using TOnDone = TUniqueFunction<void(scfg::Result<T>)>;
	using FOutcomes = std::vector<scfg::watcher::StepOutcome>;

	using FOptions = FScfgWorkerOptions;

	// A worker with its Engine over Transport and the stored pending and receipts (each None or a dict: anything
	// else is the TypeError of Engine::make, before the Engine sees it), and the Engine's first Snapshot (its Err
	// otherwise). Stopped until Start().
	static scfg::Result<TUniquePtr<FScfgWorker>> Create(scfg::config::Config Config,
		TUniquePtr<FScfgTransport> Transport, scfg::Json Pending, scfg::Json Receipts, FOptions Options = FOptions());

	FScfgWorker(const FScfgWorker&) = delete;
	FScfgWorker& operator=(const FScfgWorker&) = delete;
	~FScfgWorker();

	// Starts running commands and ticking: the first tick (after its reverify) is requested at once.
	void Start();
	// Cancels the running and queued commands and stops ticking (see the top of this file).
	void Stop();
	// Between Start() and Stop().
	bool IsRunning() const { return bStarted; }

	// --- commands (the Engine's, Engine.h; each followed by a tick) ---

	void StartSession(scfg::Json Kind, scfg::Json Chain, scfg::Json Opts, TOnDone<scfg::Json> OnDone);
	void BuildSolanaPayment(scfg::Json Sid, scfg::Json Account, TOnDone<scfg::Json> OnDone);
	void ReportHint(scfg::Json Sid, scfg::Json TxHash, TOnDone<void> OnDone);
	void SetUiStatus(scfg::Json Sid, scfg::Json Status, TOnDone<void> OnDone);
	void Discard(scfg::Json Sid, TOnDone<void> OnDone);
	void Reverify(TOnDone<scfg::Json> OnDone);
	void RestoreLookup(scfg::Json TxHash, TOnDone<std::vector<scfg::restore::RestoreCandidate>> OnDone);
	void RestoreApply(std::vector<scfg::restore::RestoreCandidate> Candidates,
		std::optional<std::map<int64_t, scfg::Json>> Choices, TOnDone<scfg::Json> OnDone);
	// A tick as soon as the queued commands are done (Python's wake()), joining the pending one if any. OnDone, if
	// given, gets that tick's outcomes (all of them) or its Err.
	void Tick(TOnDone<FOutcomes> OnDone = nullptr);

	// --- readers ---

	// The Snapshot taken after the last command (the Engine's own copy is never exposed).
	const scfg::engine::Snapshot& GetSnapshot() const { return Snapshot; }
	// The ticks' outcomes since the last call that are not "waiting", or have an error (worker.py's drain_outcomes).
	FOutcomes DrainOutcomes();
	// The Engine, for its readers (config(), chain(), running()); never for its commands.
	const scfg::engine::Engine& GetEngine() const { return *Engine; }
	// Commands waiting (not counting the running one or a pending tick); a command runs; a tick is pending.
	int32 NumQueued() const { return Queue.Num(); }
	bool IsBusy() const { return Current.IsValid(); }
	bool IsTickPending() const { return PendingTick.IsValid(); }

	// After every command that started (one cancelled while it ran included), once the Snapshot is refreshed.
	FSimpleMulticastDelegate OnStateChanged;

	// The commands (defined in ScfgWorker.cpp).
	class FCommand;
	class FTickCommand;

private:
	FScfgWorker(TUniquePtr<FScfgTransport> InTransport, FOptions InOptions);

	void Submit(TUniquePtr<FCommand> Command);
	void RequestTick();
	TUniquePtr<FCommand> TakeNext();
	bool OnTicker(float DeltaTime);
	void StopNow();
	// The tick's end (FTickCommand::Deliver): outcomes, the next interval, OnError, then each waiter.
	void EndTick(const scfg::Result<FOutcomes>& Result, TArray<TOnDone<FOutcomes>>& Waiters);
	void Report(const scfg::Err& Error);
	void Refresh();
	bool IsTickRunning() const;

	// Declared in teardown order, reversed: the transport outlives the Engine, which outlives the commands' Jobs.
	TUniquePtr<FScfgTransport> Transport;
	TUniquePtr<scfg::engine::Engine> Engine;
	TUniquePtr<FCommand> Current;
	TArray<TUniquePtr<FCommand>> Queue;
	TUniquePtr<FTickCommand> PendingTick;
	TArray<TUniquePtr<FCommand>> Finished;  // ran or cancelled, their callbacks not called yet
	FOutcomes Outcomes;
	scfg::engine::Snapshot Snapshot;
	FOptions Options;
	FTSTicker::FDelegateHandle TickerHandle;
	TSharedRef<bool> Alive = MakeShared<bool>(true);
	double SecondsToTick = 0;
	bool bStarted = false;
	bool bStopped = false;  // Stop() was called and Start() not since: new commands are cancelled
	bool bFirstTick = true;
	bool bStopWanted = false;
	int32 InsideEngine = 0;  // > 0 while the Engine's on_error runs (inside a command's coroutine)
};
