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
// is not retried (worker.gd), unless it was cancelled by Stop() before it finished: then the next tick runs it. The
// next tick comes an interval after a failing one.
//
// ERRORS ARE LOGGED HERE, ONCE. Every Err a command or a tick ends with (a CancelledError excepted) goes to OnError
// (default: LogScfg) exactly once, whether the command has a callback or not and however many Tick() callers wait on
// that tick; so do a failing snapshot and the Engine's contained errors. The callbacks get the Result as well, to
// turn it into a Failed result: they must not log it again.
//
// CALLBACKS. Every completion callback and OnStateChanged run on the game thread from the worker's ticker, never
// inside a call to the worker (a sync command's callback comes on the next frame), and in this order: the Err (if
// any) is reported to OnError, the snapshot is refreshed, OnStateChanged's handlers run, then the command's callback.
// Any of them (OnError included, when it is called from the ticker) may submit commands, Stop() or Start() the worker,
// or destroy it: the worker notices and touches nothing more, and calls no further handler or callback (the Err was
// reported first, so it is never lost that way). The Engine's contained errors are reported from inside the running
// command's coroutine: OnError must not destroy the worker then (checked), and a Stop() there is deferred to the next
// frame (IsRunning() is false meanwhile, and a Start() after it wins).
//
// ONSTATECHANGED'S HANDLERS (FScfgStateChanged) run in the order they were added. A handler removed by an earlier one
// is skipped, one added during the broadcast waits for the next, and none runs once a handler has destroyed the
// worker. A handler's owner may still die meanwhile (the facade destroying the store, say): bind it weakly (AddSP,
// AddSPLambda: the owner is checked at call time) or Remove() it in the owner's destructor, never a raw `this`
// alone. The list is shared (a TSharedRef), so a subscriber that may outlive the worker keeps a TWeakPtr to it and
// removes its handler only while it is still there.
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

// FScfgWorker::OnStateChanged: the handlers to run after every command (see ONSTATECHANGED'S HANDLERS in FScfgWorker's
// comment). Game thread only.
class SCFG_API FScfgStateChanged
{
public:
	FDelegateHandle Add(FSimpleDelegate Handler);
	// The handler runs only while Owner is alive.
	template <class UserClass, class Method>
	FDelegateHandle AddSP(const TSharedRef<UserClass>& Owner, Method Fn)
	{
		return Add(FSimpleDelegate::CreateSP(Owner, Fn));
	}
	template <class UserClass, class Functor>
	FDelegateHandle AddSPLambda(const TSharedRef<UserClass>& Owner, Functor&& Fn)
	{
		return Add(FSimpleDelegate::CreateSPLambda(Owner, Forward<Functor>(Fn)));
	}
	// A lambda with no owner check: its captures must stay valid while it is bound (tests, or Remove() in time).
	template <class Functor>
	FDelegateHandle AddLambda(Functor&& Fn)
	{
		return Add(FSimpleDelegate::CreateLambda(Forward<Functor>(Fn)));
	}
	// True when the handler was there.
	bool Remove(FDelegateHandle Handle);
	int32 Num() const { return Entries.Num(); }

private:
	friend class FScfgWorker;
	// Shared, so a broadcast holds each handler alive through its own call even if it is removed meanwhile, without
	// copying it (a mutable lambda keeps its state).
	struct FEntry
	{
		FDelegateHandle Handle;
		TSharedRef<const FSimpleDelegate> Handler;
	};
	bool Contains(FDelegateHandle Handle) const;
	TArray<FEntry> Entries;
};

// FScfgWorker's options (FScfgWorker::FOptions).
struct FScfgWorkerOptions
{
	// Seconds between ticks, read when a tick ends (the facade shortens it while a sheet is open). Default:
	// FScfgWorker::POLL_INTERVAL.
	TFunction<double()> Interval;
	// The Engine's clock: whole seconds (session timestamps and windows). Default: Unix time.
	TFunction<int64()> Clock;
	// Every Err the worker meets, once: a command's or a tick's (CancelledError excepted), a failing snapshot, and
	// the Engine's contained errors (Engine::on_error). Default: logged to LogScfg as "scfg: <Err::str()>". A
	// contained error is reported from inside the running command's coroutine (see FScfgWorker's CALLBACKS).
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
	// Between Start() and Stop() (a Stop() deferred from inside the Engine counts at once).
	bool IsRunning() const { return bStarted && !bStopWanted; }

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
	// worker.py's submit(fn) for a plain function: Fn runs on the Engine in its turn, as any command (the facade's
	// ClearLocalData wipes the stores this way, and the tests change the Engine's state). Fn must not start a coroutine.
	void Run(TUniqueFunction<scfg::Result<scfg::Json>(scfg::engine::Engine&)> Fn, TOnDone<scfg::Json> OnDone);

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
	// Valid and true while the worker exists: a holder that may outlive it (the Submitter adapter) checks it before
	// every call.
	TWeakPtr<bool> GetLifetime() const { return Alive; }

	// After every command that started (one cancelled while it ran included), once the Snapshot is refreshed. Shared,
	// so a subscriber can hold it weakly (see ONSTATECHANGED'S HANDLERS above).
	const TSharedRef<FScfgStateChanged> OnStateChanged = MakeShared<FScfgStateChanged>();

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
	// OnStateChanged's handlers; stops as soon as one has destroyed the worker (StillAlive false).
	void BroadcastStateChanged(const TSharedRef<bool>& StillAlive);
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
