// Copyright 2026 Repulse
// Twin of scfg_core/worker.py and godot/addons/scfg/core/worker.gd (see ScfgWorker.h).
#include "ScfgWorker.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "Misc/DateTime.h"

#include "Scfg/Co.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ScfgWorkerDetail
{

scfg::Err Cancelled()
{
	return scfg::Err::make("CancelledError", "the worker was stopped before the command finished");
}

// Not an error to report: a cancellation, or an abandoned command's refused request (the player moved on).
bool IsCancelled(const scfg::Err& Error)
{
	return Error.kind == "CancelledError" || Error.kind == "Abandoned";
}

}  // namespace ScfgWorkerDetail

// One queued command. Start() begins it on the Engine (a network command starts its Job, a plain one runs);
// Poll() tells whether it has finished (and may start its next step); Cancel() destroys its Job, if any, and makes
// its result CancelledError; Deliver() hands the result to its callback, once. None of them is ever called from
// inside a Job's on_done: the worker calls them from its ticker.
class FScfgWorker::FCommand
{
public:
	virtual ~FCommand() = default;
	virtual void Start(scfg::engine::Engine& Engine) = 0;
	virtual bool Poll(scfg::engine::Engine& Engine) = 0;
	virtual void Cancel() = 0;
	virtual void Deliver() = 0;
	virtual bool IsTick() const { return false; }
	// Its requests are refused while a command waits (worker.py: commands come first): a tick inside Engine::tick(),
	// not its first-run reverify.
	virtual bool Yields() const { return false; }
	// The Err the command finished with (not called before Poll() said it finished), or null for a value.
	virtual const scfg::Err* Error() const = 0;

	// Start() was called: the Engine's state may have changed (OnStateChanged follows).
	bool bStarted = false;
	// The abandon flag it was submitted with (ABANDONED COMMANDS in ScfgWorker.h), or null.
	scfg::worker::AbandonFlag Abandoned;
	bool IsAbandoned() const { return Abandoned && *Abandoned; }
};

namespace ScfgWorkerDetail
{

// A command whose Engine method is a coroutine (Engine.h's network commands), run as an scfg::Job.
template <class T>
class TNetworkCommand final : public FScfgWorker::FCommand
{
public:
	using FMake = TUniqueFunction<scfg::Co<T>(scfg::engine::Engine&)>;

	TNetworkCommand(FMake InMake, FScfgWorker::TOnDone<T> InOnDone)
		: Make(MoveTemp(InMake)), OnDone(MoveTemp(InOnDone))
	{
	}

	virtual void Start(scfg::engine::Engine& Engine) override
	{
		bStarted = true;
		Job.emplace();
		// on_done only keeps the result: the worker acts on it from its ticker (Co.h: an on_done must not destroy or
		// restart its Job).
		Job->start(Make(Engine), [this](scfg::Result<T> R) { Result.emplace(std::move(R)); });
	}
	virtual bool Poll(scfg::engine::Engine&) override { return Result.has_value(); }
	virtual void Cancel() override
	{
		Job.reset();  // destroys the suspended coroutine: cancellation (Co.h)
		if (!Result)
		{
			Result.emplace(Cancelled());
		}
	}
	virtual void Deliver() override
	{
		if (OnDone)
		{
			OnDone(std::move(*Result));
		}
	}
	virtual const scfg::Err* Error() const override { return Result && !Result->ok() ? &Result->err() : nullptr; }

private:
	FMake Make;
	FScfgWorker::TOnDone<T> OnDone;
	std::optional<scfg::Job<T>> Job;
	std::optional<scfg::Result<T>> Result;
};

// A command whose Engine method returns at once (report_hint, set_ui_status, discard_session, restore_apply).
template <class T>
class TPlainCommand final : public FScfgWorker::FCommand
{
public:
	using FRun = TUniqueFunction<scfg::Result<T>(scfg::engine::Engine&)>;

	TPlainCommand(FRun InRun, FScfgWorker::TOnDone<T> InOnDone) : Run(MoveTemp(InRun)), OnDone(MoveTemp(InOnDone)) {}

	virtual void Start(scfg::engine::Engine& Engine) override
	{
		bStarted = true;
		Result.emplace(Run(Engine));
	}
	virtual bool Poll(scfg::engine::Engine&) override { return Result.has_value(); }
	virtual void Cancel() override
	{
		if (!Result)
		{
			Result.emplace(Cancelled());
		}
	}
	virtual void Deliver() override
	{
		if (OnDone)
		{
			OnDone(std::move(*Result));
		}
	}
	virtual const scfg::Err* Error() const override { return Result && !Result->ok() ? &Result->err() : nullptr; }

private:
	FRun Run;
	FScfgWorker::TOnDone<T> OnDone;
	std::optional<scfg::Result<T>> Result;
};

}  // namespace ScfgWorkerDetail

// The pending or running tick: Engine::reverify() first on the worker's very first tick, then Engine::tick(). Every
// Tick() caller while it is pending waits on it.
class FScfgWorker::FTickCommand final : public FScfgWorker::FCommand
{
public:
	explicit FTickCommand(FScfgWorker& InWorker) : Worker(InWorker) {}

	TArray<TOnDone<FOutcomes>> Waiters;
	bool bReverifyFirst = false;

	virtual bool IsTick() const override { return true; }
	virtual bool Yields() const override { return TickJob.has_value() && !Result; }
	virtual void Start(scfg::engine::Engine& E) override
	{
		bStarted = true;
		if (bReverifyFirst)
		{
			ReverifyJob.emplace();
			ReverifyJob->start(E.reverify(), [this](scfg::Result<scfg::Json> R) { Reverified.emplace(std::move(R)); });
		}
		else
		{
			StartTick(E);
		}
	}
	virtual bool Poll(scfg::engine::Engine& E) override
	{
		if (!Result && Reverified && !TickJob)
		{
			// The reverify finished (its coroutine has returned, so the Engine is free): the tick follows at once, as
			// worker.py's first run_once does; a failed reverify ends the tick and is not retried (worker.gd).
			if (!Reverified->ok())
			{
				Result.emplace(Reverified->err());
			}
			else
			{
				ReverifyJob.reset();
				StartTick(E);
			}
		}
		return Result.has_value();
	}
	virtual void Cancel() override
	{
		// A startup reverify that was cancelled before it finished is owed: the next tick runs it (a failed one is
		// not retried, worker.gd).
		if (bStarted && bReverifyFirst && !Reverified)
		{
			Worker.bFirstTick = true;
		}
		TickJob.reset();
		ReverifyJob.reset();
		if (!Result)
		{
			// A startup reverify that failed before this tick was polled keeps its Err (reported, not retried).
			if (Reverified && !Reverified->ok())
			{
				Result.emplace(Reverified->err());
			}
			else
			{
				Result.emplace(ScfgWorkerDetail::Cancelled());
			}
		}
	}
	virtual void Deliver() override { Worker.EndTick(*Result, Waiters); }
	virtual const scfg::Err* Error() const override { return Result && !Result->ok() ? &Result->err() : nullptr; }

private:
	void StartTick(scfg::engine::Engine& E)
	{
		TickJob.emplace();
		TickJob->start(E.tick(), [this](scfg::Result<FOutcomes> R) { Result.emplace(std::move(R)); });
	}

	FScfgWorker& Worker;
	std::optional<scfg::Job<scfg::Json>> ReverifyJob;
	std::optional<scfg::Result<scfg::Json>> Reverified;
	std::optional<scfg::Job<FOutcomes>> TickJob;
	std::optional<scfg::Result<FOutcomes>> Result;
};

scfg::Result<TUniquePtr<FScfgWorker>> FScfgWorker::Create(scfg::config::Config Config,
	TUniquePtr<FScfgTransport> Transport, scfg::Json Pending, scfg::Json Receipts, FOptions Options)
{
	check(IsInGameThread());
	check(Transport.IsValid());
	TUniquePtr<FScfgWorker> Worker(new FScfgWorker(MoveTemp(Transport), MoveTemp(Options)));
	FScfgWorker* const Self = Worker.Get();

	std::function<int64_t()> Clock;
	if (Self->Options.Clock)
	{
		Clock = [Given = Self->Options.Clock]() -> int64_t { return Given(); };
	}
	else
	{
		Clock = []() -> int64_t { return FDateTime::UtcNow().ToUnixTimestamp(); };
	}
	// The Engine's contained errors: reported from inside a command's coroutine.
	std::function<void(const scfg::Err&)> OnEngineError = [Self](const scfg::Err& Error)
	{
		++Self->InsideEngine;
		Self->Report(Error);
		--Self->InsideEngine;
	};
	SCFG_TRY(std::unique_ptr<scfg::engine::Engine> Made,
		scfg::engine::Engine::make(std::move(Config), *Self->Transport, std::move(Pending), std::move(Receipts),
			std::move(Clock), Self->Options.RpcFactory, std::move(OnEngineError), Self->Options.Entropy));
	Self->Engine = TUniquePtr<scfg::engine::Engine>(Made.release());
	SCFG_TRY(scfg::engine::Snapshot First, Self->Engine->snapshot());
	Self->Snapshot = std::move(First);

	FTSTicker& Ticker = Self->Options.Ticker ? *Self->Options.Ticker : FTSTicker::GetCoreTicker();
	Self->TickerHandle =
		Ticker.AddTicker(TEXT("ScfgWorker"), 0.0f, [Self](float DeltaTime) { return Self->OnTicker(DeltaTime); });
	return scfg::Result<TUniquePtr<FScfgWorker>>(MoveTemp(Worker));
}

FScfgWorker::FScfgWorker(TUniquePtr<FScfgTransport> InTransport, FOptions InOptions)
	: Transport(MoveTemp(InTransport)), Options(MoveTemp(InOptions))
{
}

FScfgWorker::~FScfgWorker()
{
	check(IsInGameThread());
	// Destroying the worker from inside the Engine's on_error would destroy the coroutine frame that is running.
	checkf(InsideEngine == 0, TEXT("an FScfgWorker destroyed from inside its Engine's on_error"));
	*Alive = false;
	FTSTicker::RemoveTicker(TickerHandle);
	// Jobs first (cancelled, no callback), then the Engine, then the transport (Co.h, "Teardown order").
	Current.Reset();
	Queue.Reset();
	PendingTick.Reset();
	Finished.Reset();
	Engine.Reset();
	if (Transport)
	{
		Transport->Shutdown();
	}
	Transport.Reset();
}

void FScfgWorker::Start()
{
	check(IsInGameThread());
	bStopWanted = false;  // the last call wins over a Stop() deferred from inside the Engine
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	bStopped = false;
	RequestTick();
}

void FScfgWorker::Stop()
{
	check(IsInGameThread());
	if (InsideEngine > 0)
	{
		bStopWanted = true;  // the running coroutine is on the stack: cancel it from the next frame
		return;
	}
	StopNow();
}

void FScfgWorker::StopNow()
{
	bStarted = false;
	bStopped = true;
	bStopWanted = false;
	if (Current)
	{
		Current->Cancel();
		Finished.Add(MoveTemp(Current));
	}
	for (TUniquePtr<FCommand>& Each : Queue)
	{
		Each->Cancel();
		Finished.Add(MoveTemp(Each));
	}
	Queue.Reset();
	if (PendingTick)
	{
		PendingTick->Cancel();
		Finished.Add(MoveTemp(PendingTick));
	}
	// The Engine is idle now. A command whose last answer arrived since the last frame has already written into it
	// (a receipt, a state change, scan progress) and was cancelled before its snapshot: take it now, so a reader right
	// after Stop() (the facade's Shutdown saves it) sees it. Last: a failing snapshot is reported, and OnError may
	// destroy the worker.
	Refresh();
}

void FScfgWorker::Submit(TUniquePtr<FCommand> Command)
{
	check(IsInGameThread());
	if (bStopped)
	{
		Command->Cancel();
		Finished.Add(MoveTemp(Command));
		return;
	}
	Queue.Add(MoveTemp(Command));
	RequestTick();  // worker.py: a submit wakes the loop, whose pass ends with a tick
}

void FScfgWorker::RequestTick()
{
	if (!PendingTick && !bStopped)
	{
		PendingTick = MakeUnique<FTickCommand>(*this);
	}
}

void FScfgWorker::Tick(TOnDone<FOutcomes> OnDone)
{
	check(IsInGameThread());
	if (bStopped)
	{
		TUniquePtr<FTickCommand> Refused = MakeUnique<FTickCommand>(*this);
		if (OnDone)
		{
			Refused->Waiters.Add(MoveTemp(OnDone));
		}
		Refused->Cancel();
		Finished.Add(MoveTemp(Refused));
		return;
	}
	RequestTick();
	if (OnDone)
	{
		PendingTick->Waiters.Add(MoveTemp(OnDone));
	}
}

void FScfgWorker::StartSession(scfg::Json Kind, scfg::Json Chain, scfg::Json Opts, TOnDone<scfg::Json> OnDone,
	scfg::worker::AbandonFlag Abandoned)
{
	TUniquePtr<FCommand> Command = MakeUnique<ScfgWorkerDetail::TNetworkCommand<scfg::Json>>(
		[Kind = std::move(Kind), Chain = std::move(Chain), Opts = std::move(Opts)](scfg::engine::Engine& E) mutable
		{ return E.start_session(std::move(Kind), std::move(Chain), std::move(Opts)); },
		MoveTemp(OnDone));
	Command->Abandoned = std::move(Abandoned);
	Submit(MoveTemp(Command));
}

void FScfgWorker::BuildSolanaPayment(scfg::Json Sid, scfg::Json Account, TOnDone<scfg::Json> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TNetworkCommand<scfg::Json>>(
		[Sid = std::move(Sid), Account = std::move(Account)](scfg::engine::Engine& E) mutable
		{ return E.build_solana_payment(std::move(Sid), std::move(Account)); },
		MoveTemp(OnDone)));
}

void FScfgWorker::ReportHint(scfg::Json Sid, scfg::Json TxHash, TOnDone<void> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TPlainCommand<void>>(
		[Sid = std::move(Sid), TxHash = std::move(TxHash)](scfg::engine::Engine& E)
		{ return E.report_hint(Sid, TxHash); },
		MoveTemp(OnDone)));
}

void FScfgWorker::SetUiStatus(scfg::Json Sid, scfg::Json Status, TOnDone<void> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TPlainCommand<void>>(
		[Sid = std::move(Sid), Status = std::move(Status)](scfg::engine::Engine& E)
		{ return E.set_ui_status(Sid, Status); },
		MoveTemp(OnDone)));
}

void FScfgWorker::Discard(scfg::Json Sid, TOnDone<void> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TPlainCommand<void>>(
		[Sid = std::move(Sid)](scfg::engine::Engine& E) { return E.discard_session(Sid); }, MoveTemp(OnDone)));
}

void FScfgWorker::Reverify(TOnDone<scfg::Json> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TNetworkCommand<scfg::Json>>(
		[](scfg::engine::Engine& E) { return E.reverify(); }, MoveTemp(OnDone)));
}

void FScfgWorker::RestoreLookup(scfg::Json TxHash, TOnDone<std::vector<scfg::restore::RestoreCandidate>> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TNetworkCommand<std::vector<scfg::restore::RestoreCandidate>>>(
		[TxHash = std::move(TxHash)](scfg::engine::Engine& E) mutable { return E.restore_lookup(std::move(TxHash)); },
		MoveTemp(OnDone)));
}

void FScfgWorker::RestoreApply(std::vector<scfg::restore::RestoreCandidate> Candidates,
	std::optional<std::map<int64_t, scfg::Json>> Choices, TOnDone<scfg::Json> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TPlainCommand<scfg::Json>>(
		[Candidates = std::move(Candidates), Choices = std::move(Choices)](scfg::engine::Engine& E)
		{ return E.restore_apply(Candidates, Choices); },
		MoveTemp(OnDone)));
}

void FScfgWorker::Run(
	TUniqueFunction<scfg::Result<scfg::Json>(scfg::engine::Engine&)> Fn, TOnDone<scfg::Json> OnDone)
{
	Submit(MakeUnique<ScfgWorkerDetail::TPlainCommand<scfg::Json>>(MoveTemp(Fn), MoveTemp(OnDone)));
}

FScfgWorker::FOutcomes FScfgWorker::DrainOutcomes()
{
	FOutcomes Out = std::move(Outcomes);
	Outcomes.clear();
	return Out;
}

bool FScfgWorker::IsTickRunning() const
{
	return Current.IsValid() && Current->IsTick();
}

bool FScfgWorker::RefusesCurrent() const
{
	return Current.IsValid() && (Current->IsAbandoned() || (Current->Yields() && Queue.Num() > 0));
}

TUniquePtr<FScfgWorker::FCommand> FScfgWorker::TakeNext()
{
	if (Queue.Num() > 0)
	{
		TUniquePtr<FCommand> Next = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
		return Next;
	}
	if (PendingTick)
	{
		PendingTick->bReverifyFirst = bFirstTick;
		bFirstTick = false;
		return MoveTemp(PendingTick);
	}
	return nullptr;
}

bool FScfgWorker::OnTicker(float DeltaTime)
{
	check(IsInGameThread());
	const TSharedRef<bool> StillAlive = Alive;
	if (bStopWanted)
	{
		StopNow();
		if (!*StillAlive)
		{
			return false;  // OnError (a failing snapshot) destroyed the worker
		}
	}
	if (bStarted && !PendingTick && !IsTickRunning())
	{
		SecondsToTick -= DeltaTime;
		if (SecondsToTick <= 0)
		{
			RequestTick();
		}
	}
	for (;;)
	{
		if (Current && Current->bStarted && RefusesCurrent())
		{
			// Abandoned, or a tick yielding to a queued command: its request in flight ends now (Abandoned) instead of
			// at its answer or timeout, and the command, refused any further request, finishes (ABANDONED COMMANDS
			// and TICKS in ScfgWorker.h). From the ticker: the coroutine resumes inside this call.
			Engine->abandon_in_flight();
		}
		if (Current && Current->Poll(*Engine))
		{
			Finished.Add(MoveTemp(Current));
		}
		if (Finished.Num() > 0)
		{
			TUniquePtr<FCommand> Done = MoveTemp(Finished[0]);
			Finished.RemoveAt(0);
			// Every Err a command ends with is logged once, here, whether it has a callback or not; the callback
			// gets the Result too and must not log it again. A cancellation is not an error. First, so that a
			// handler below that destroys the worker cannot lose it.
			if (const scfg::Err* Failed = Done->Error(); Failed && !ScfgWorkerDetail::IsCancelled(*Failed))
			{
				Report(*Failed);
				if (!*StillAlive)
				{
					return false;
				}
			}
			if (Done->bStarted)
			{
				Refresh();  // a failing snapshot is reported: OnError may have destroyed the worker
				if (!*StillAlive)
				{
					return false;
				}
				BroadcastStateChanged(StillAlive);
				if (!*StillAlive)
				{
					return false;
				}
			}
			Done->Deliver();
			if (!*StillAlive)
			{
				return false;
			}
			continue;
		}
		if (!bStarted || Current)
		{
			return true;
		}
		Current = TakeNext();
		if (!Current)
		{
			return true;
		}
		if (Current->IsAbandoned())
		{
			// worker.py's abandon(fut) of a queued job: it never runs (ABANDONED COMMANDS in ScfgWorker.h).
			Current->Cancel();
			Finished.Add(MoveTemp(Current));
			continue;
		}
		// One command at a time: the Engine would abort an overlap (Engine.h).
		check(!Engine->running());
		// Its requests are refused once its flag turns true, and a tick's while a command waits (RefusesCurrent).
		Engine->refuse_requests = [this] { return RefusesCurrent(); };
		Current->Start(*Engine);
	}
}

void FScfgWorker::EndTick(const scfg::Result<FOutcomes>& Result, TArray<TOnDone<FOutcomes>>& Waiters)
{
	const TSharedRef<bool> StillAlive = Alive;
	SecondsToTick = Options.Interval ? Options.Interval() : POLL_INTERVAL;
	if (Result.ok())
	{
		for (const scfg::watcher::StepOutcome& Each : Result.value())
		{
			if (Each.status != "waiting" || Each.error)
			{
				Outcomes.push_back(Each);
			}
		}
	}
	for (TOnDone<FOutcomes>& Waiter : Waiters)
	{
		Waiter(Result);
		if (!*StillAlive)
		{
			return;
		}
	}
}

FDelegateHandle FScfgStateChanged::Add(FSimpleDelegate Handler)
{
	check(IsInGameThread());
	if (!Handler.IsBound())
	{
		return FDelegateHandle();
	}
	const FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	Entries.Add(FEntry{Handle, MakeShared<const FSimpleDelegate>(MoveTemp(Handler))});
	return Handle;
}

bool FScfgStateChanged::Remove(FDelegateHandle Handle)
{
	check(IsInGameThread());
	return Handle.IsValid() && Entries.RemoveAll([Handle](const FEntry& Each) { return Each.Handle == Handle; }) > 0;
}

bool FScfgStateChanged::Contains(FDelegateHandle Handle) const
{
	return Entries.ContainsByPredicate([Handle](const FEntry& Each) { return Each.Handle == Handle; });
}

void FScfgWorker::BroadcastStateChanged(const TSharedRef<bool>& StillAlive)
{
	// The list is copied (its handlers are shared, not copied): a handler may add or remove handlers, or destroy the
	// worker (the list itself is shared too, so Event stays valid).
	const TSharedRef<FScfgStateChanged> Event = OnStateChanged;
	const TArray<FScfgStateChanged::FEntry> Handlers = Event->Entries;
	for (const FScfgStateChanged::FEntry& Each : Handlers)
	{
		if (!*StillAlive)
		{
			return;
		}
		if (Event->Contains(Each.Handle))
		{
			Each.Handler->ExecuteIfBound();
		}
	}
}

void FScfgWorker::Report(const scfg::Err& Error)
{
	if (Options.OnError)
	{
		// A copy: a handler that destroys the worker destroys Options with it.
		const TFunction<void(const scfg::Err&)> Handler = Options.OnError;
		Handler(Error);
	}
	else
	{
		UE_LOG(LogScfg, Error, TEXT("scfg: %s"), *ScfgString::FromUtf8(Error.str()));
	}
}

void FScfgWorker::Refresh()
{
	scfg::Result<scfg::engine::Snapshot> Next = Engine->snapshot();
	if (Next.ok())
	{
		Snapshot = Next.take();
	}
	else
	{
		Report(Next.err());  // the last good Snapshot stays
	}
}
