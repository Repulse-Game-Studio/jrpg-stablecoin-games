// Copyright 2026 Repulse
// No Python twin: FScfgWorker's behaviour, the twin of tests/unit/test_worker.py and godot/tests/test_worker.gd
// (their non-thread, non-frame-pump parts), with the real Engine over a fake transport that answers each JSON-RPC
// request on a later frame, and a private FTSTicker that each test ticks by hand (one "frame": the fake's answers due
// now, then the worker's ticker).
//
// THE MAPPING, every test of the two twins (Godot's test_worker.gd, then Python's test_worker.py), to the test here
// that has its behaviour (the tests also name their twins above them), or why there is none:
//   test_initial_snapshot_taken_at_construction                    TheFirstSnapshotIsTakenAtCreation
//   test_run_once_reverifies_only_first_time_and_publishes_snapshot TheFirstTickReverifiesOnlyOnce
//   test_waiting_outcomes_without_error_are_not_published          OnlyNewsIsPublishedAsAnOutcome
//   test_waiting_outcome_with_error_is_published                   OnlyNewsIsPublishedAsAnOutcome
//   test_submitted_commands_run_before_tick_and_resolve_jobs       CommandsRunBeforeTheTickInOrder
//   test_a_command_submitted_by_a_command_runs_before_the_same_tick ACommandSubmittedByACallbackRunsBeforeTheTick
//   test_cancelled_job_is_skipped                                  StopCancelsEverything
//   test_timer_loop_wake_and_stop                                  TicksComeEveryInterval
//   test_loop_survives_errors                                      AFailingTickIsReportedAndTheNextComesLater
//   test_async_commands_are_awaited                                TheReceiptAndSolanaCommandsRunThroughTheWorker (each
//                                                                  network command finishes when the node answers, frames later)
//   test_pump_a_command_submitted_mid_tick_waits_for_the_tick      CommandsNeverInterleaveWithATick
//   test_pump_commands_run_before_a_due_tick_and_plain_results_resolve CommandsRunBeforeTheTickInOrder
//   test_pump_a_submit_landing_mid_run_gets_its_follow_up_at_once  CommandsNeverInterleaveWithATick
//   test_pump_a_run_without_a_mid_run_wake_waits_the_interval      TicksComeEveryInterval
//   test_pump_a_failing_tick_reports_and_schedules_the_next        AFailingTickIsReportedAndTheNextComesLater
//   test_pump_stop_cancels_queued_commands_and_runs_nothing_more   StopCancelsEverything
//   test_commands_never_interleave_with_a_tick                     CommandsNeverInterleaveWithATick
//   test_wake_during_a_run_schedules_exactly_one_follow_up         WakesDuringATickGiveExactlyOneFollowUp
//   test_first_run_reverifies_once                                 TheFirstTickReverifiesOnlyOnce
//   test_a_failing_reverify_ends_the_pass_and_is_not_retried       AFailingTickIsReportedAndTheNextComesLater
//   test_job_reports_exception                                     CommandsRunBeforeTheTickInOrder (a failing command gets
//                                                                  its Err), EveryErrIsReportedOnce
//   test_job_finished_signal_fires_once_per_job                    none: Godot's ScfgJob.finished signal; here a command's
//                                                                  callback is called once (every test counts it), and the core's
//                                                                  Job is the CMake CoTests'
//   test_stop_cancels_the_timer                                    StopCancelsEverything ("no tick comes" after Stop()),
//                                                                  TicksComeEveryInterval ("no tick after Stop()")
//   test_no_run_after_stop_even_if_a_run_is_in_flight              StopCancelsEverything
//   test_job_pending_result_is_an_error                            none: ScfgJob.result() of a pending job; the glue has no
//                                                                  such reader (a callback gets the Result once, when done)
//   test_reentrant_run_once_from_a_command_never_overlaps_ticks    none: there is no run_once here (ticks come only from the
//                                                                  worker's own ticker); no overlap: CommandsNeverInterleaveWithATick
//   test_wake_and_submit_from_a_command_give_one_follow_up         WakesDuringATickGiveExactlyOneFollowUp
//   test_cancelled_job_exception_is_the_cancelled_error            StopCancelsEverything (CancelledError)
//   test_submit_on_a_stopped_worker_is_cancelled_but_before_start_it_queues StopCancelsEverything
//   test_timer_firing_during_a_wake_triggered_run_yields_one_follow_up WakesDuringATickGiveExactlyOneFollowUp (one tick
//                                                                  pending at most, whatever asks)
//   test_joining_run_once_returns_the_chain_failure                AFailingTickIsReportedAndTheNextComesLater ("every waiter
//                                                                  got it")
//   test_job_wait_returns_at_once_on_a_done_job                    none: ScfgJob.wait() (GDScript await); the glue hands out
//                                                                  callbacks, never a Future (Co.h's are the CMake CoTests')
//   test_job_wait_resumes_when_the_job_finishes                    none: the same
//   test_tree_scheduler_fires_and_cancels                          none: Godot's SceneTree scheduler; here the FTSTicker
//                                                                  element, which every test drives (a private ticker)
//   test_tree_scheduler_survives_a_freed_target                    none: the same (no Callable target to free)
//   test_worker_builds_the_default_scheduler_from_a_tree           none: the same; the default core ticker runs the worker in
//                                                                  Scfg.Glue.Http.TheWorkerOverTheRealTransport
//   (test_worker.py) test_initial_snapshot_taken_at_construction   TheFirstSnapshotIsTakenAtCreation
//   test_run_once_reverifies_only_first_time_and_publishes_snapshot TheFirstTickReverifiesOnlyOnce
//   test_waiting_outcomes_without_error_are_not_published          OnlyNewsIsPublishedAsAnOutcome
//   test_waiting_outcome_with_error_is_published                   OnlyNewsIsPublishedAsAnOutcome
//   test_submitted_commands_run_before_tick_and_resolve_futures    CommandsRunBeforeTheTickInOrder
//   test_a_command_submitted_by_a_command_runs_before_the_same_tick ACommandSubmittedByACallbackRunsBeforeTheTick
//   test_cancelled_future_is_skipped                               StopCancelsEverything
//   test_thread_loop_wake_and_stop                                 TicksComeEveryInterval (Python's thread loop; here the
//                                                                  ticker: no thread)
//   test_loop_survives_exceptions                                  AFailingTickIsReportedAndTheNextComesLater
//   test_generator_commands_are_driven_by_the_engine               TheReceiptAndSolanaCommandsRunThroughTheWorker (the network
//                                                                  commands run as scfg::Job coroutines on the Engine)
// The tests here without a twin are the Unreal glue's own: refused stores, OnStateChanged's order and handlers,
// teardown, Stop() from inside the Engine, a handler destroying the worker, the cancelled or failed startup reverify
// through Stop(), the receipt commands, the fresh snapshot at Stop().
#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"

#include "ScfgString.h"
#include "ScfgTransport.h"
#include "ScfgWorker.h"

#include "Scfg/Co.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgWorkerTests
{

using scfg::Json;
using scfg::Result;

constexpr const char* kWallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
constexpr int64_t kNow = 1700000100;
const char* const kTx = "0x135de9b0d45e605167347488c624be20bda97481b0bbabbc6e1d97bb816d2d1a";

scfg::config::Config BaseConfig()
{
	Result<scfg::config::Config> Built = scfg::config::build_config(
		Json::dict({{"wallet", kWallet}, {"chains", Json::list({"base"})}, {"testnet", true}}));
	scfg::config::Config Out = Built.take();
	Result<scfg::models::Item> Item = scfg::config::add_item(
		Out, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
	(void)Item.take();
	return Out;
}

// An awaiting EVM session on base (EngineTests' awaiting()): each tick steps it with eth_blockNumber + eth_getLogs.
Json Awaiting(const char* Id, int64_t CreatedAt)
{
	return Json::dict({{"id", Id}, {"kind", "unlock"}, {"item", "chapter2"}, {"title", "Chapter 2"}, {"token", "USDC"},
		{"chain", "base"}, {"price", "4.99"}, {"code", 37}, {"nonce", 6}, {"units", 4993706}, {"start_block", 995},
		{"cursor", 995}, {"created_at", CreatedAt}, {"state", "awaiting"}, {"ui_status", "open"}, {"hint_tx", Json()},
		{"hint_at", Json()}, {"match", Json()}});
}

// A stored base receipt: reverify checks it with one eth_getLogs in its block.
Json Receipts()
{
	const std::string Key = std::string("base:") + kTx + ":2";
	return Json::dict({{Key, Json::dict({{"amount", "4.993742"}, {"block", 900}, {"chain", "base"},
		{"item", "chapter2"}, {"kind", "unlock"}, {"log_index", 2}, {"payer", "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"},
		{"recipient", kWallet}, {"source", "payment"}, {"token", "USDC"}, {"tx_hash", kTx}})}});
}

// What the tests see of the fake transport, also after the worker destroyed it.
struct FStats
{
	TArray<FString> Methods;
	int32 Shutdowns = 0;
	int32 DroppedAtShutdown = 0;
	int32 AnsweredLate = 0;
};

// Answers each JSON-RPC request after Delay frames (Pump calls), as FHttpModule answers on a later tick: from
// Answer(method), a result or an Err the request fails with. Hold keeps every request until it is set back.
class FFakeTransport final : public FScfgTransport
{
public:
	explicit FFakeTransport(TSharedRef<FStats> InStats) : Stats(InStats) {}

	// By default the head grows by one block per eth_blockNumber (1000, 1001, ...), so every step scans new blocks.
	TFunction<Result<Json>(const FString& Method)> Answer = [this](const FString& Method) -> Result<Json>
	{
		if (Method == TEXT("eth_blockNumber"))
		{
			return Json(ScfgString::ToUtf8(FString::Printf(TEXT("0x%llx"), static_cast<unsigned long long>(Head++))));
		}
		if (Method == TEXT("eth_getLogs"))
		{
			return Json::list();
		}
		return scfg::Err::transport_error("no answer for " + ScfgString::ToUtf8(Method), std::nullopt);
	};
	// A JSON-RPC error object to answer Method with instead (the node refusing the call), or nothing.
	TFunction<std::optional<Json>(const FString& Method)> RpcErrorFor = [](const FString&) { return std::nullopt; };
	int32 Delay = 1;
	bool bHold = false;
	int64_t Head = 1000;

	virtual scfg::Future<Json> post_json(std::string Url, Json Payload, double Timeout) override
	{
		(void)Url, (void)Timeout;
		const FString Method = ScfgString::FromUtf8(Payload.as_dict().get("method").as_str());
		Stats->Methods.Add(Method);
		if (bShutDown)
		{
			return scfg::Future<Json>::completed(scfg::Err::make("CancelledError", "shut down"));
		}
		Result<Json> Envelope = Respond(Method, Payload.as_dict().get("id"));
		if (Delay == 0 && !bHold)
		{
			return scfg::Future<Json>::completed(std::move(Envelope));
		}
		Held.push_back(FHeld{scfg::Promise<Json>(), std::move(Envelope), Delay});
		return Held.back().Promise.get_future();
	}

	virtual void Shutdown() override
	{
		++Stats->Shutdowns;
		bShutDown = true;
		std::deque<FHeld> Dropped = std::move(Held);
		Held.clear();
		Stats->DroppedAtShutdown += static_cast<int32>(Dropped.size());
	}

	// One frame: the requests due now are answered (taken out first: a resumed coroutine may post again).
	void Pump()
	{
		if (bHold)
		{
			return;
		}
		std::deque<FHeld> Due;
		std::deque<FHeld> Later;
		for (FHeld& Each : Held)
		{
			(--Each.FramesLeft <= 0 ? Due : Later).push_back(std::move(Each));
		}
		Held = std::move(Later);
		for (FHeld& Each : Due)
		{
			Each.Promise.complete(std::move(Each.Answer));
		}
	}

	int32 NumHeld() const { return static_cast<int32>(Held.size()); }

private:
	Result<Json> Respond(const FString& Method, const Json& Id)
	{
		if (std::optional<Json> Refusal = RpcErrorFor(Method))
		{
			return Json::dict({{"jsonrpc", "2.0"}, {"id", Id}, {"error", *Refusal}});
		}
		Result<Json> Got = Answer(Method);
		if (!Got.ok())
		{
			return Got;
		}
		return Json::dict({{"jsonrpc", "2.0"}, {"id", Id}, {"result", Got.value()}});
	}

	struct FHeld
	{
		scfg::Promise<Json> Promise;
		Result<Json> Answer;
		int32 FramesLeft;
	};

	TSharedRef<FStats> Stats;
	std::deque<FHeld> Held;
	bool bShutDown = false;
};

// A worker over a fake transport and a private ticker, with a clock and an error log the test controls.
struct FWorld
{
	FTSTicker Ticker;
	TSharedRef<FStats> Stats = MakeShared<FStats>();
	FFakeTransport* Fake = nullptr;  // owned by Worker
	TUniquePtr<FScfgWorker> Worker;
	TSharedRef<int64_t> Now = MakeShared<int64_t>(kNow);
	TSharedRef<double> Interval = MakeShared<double>(FScfgWorker::POLL_INTERVAL);
	TSharedRef<TArray<FString>> Errors = MakeShared<TArray<FString>>();

	FWorld() = default;
	FWorld(const FWorld&) = delete;
	FWorld& operator=(const FWorld&) = delete;
	~FWorld() { Worker.Reset(); }  // before the ticker it is registered with

	Result<void> Make(Json Pending = Json(), Json StoredReceipts = Json())
	{
		TUniquePtr<FFakeTransport> Transport = MakeUnique<FFakeTransport>(Stats);
		Fake = Transport.Get();
		FScfgWorker::FOptions Options;
		Options.Ticker = &Ticker;
		Options.Clock = [NowRef = Now]() { return *NowRef; };
		Options.Interval = [IntervalRef = Interval]() { return *IntervalRef; };
		Options.OnError = [ErrorsRef = Errors](const scfg::Err& E) { ErrorsRef->Add(ScfgString::FromUtf8(E.str())); };
		Result<TUniquePtr<FScfgWorker>> Made = FScfgWorker::Create(
			BaseConfig(), MoveTemp(Transport), std::move(Pending), std::move(StoredReceipts), MoveTemp(Options));
		if (!Made.ok())
		{
			Fake = nullptr;
			return Made.err();
		}
		Worker = Made.take();
		return scfg::ok();
	}

	// One frame: the fake's due answers, then the worker's ticker.
	void Frame(float Seconds = 0.1f)
	{
		if (Worker && Fake)
		{
			Fake->Pump();
		}
		Ticker.Tick(Seconds);
	}

	void Frames(int32 N, float Seconds = 0.1f)
	{
		for (int32 I = 0; I < N; ++I)
		{
			Frame(Seconds);
		}
	}

	int32 Count(const TCHAR* Method) const
	{
		return Stats->Methods.FilterByPredicate([Method](const FString& M) { return M == Method; }).Num();
	}
};

// One command's callback result, kept for the test.
template <class T>
struct TGot
{
	std::optional<Result<T>> Value;
	FScfgWorker::TOnDone<T> Callback()
	{
		return [this](Result<T> R) { Value.emplace(std::move(R)); };
	}
	bool Ok() const { return Value && Value->ok(); }
	FString Kind() const { return Value && !Value->ok() ? ScfgString::FromUtf8(Value->err().kind) : FString(); }
};

FString Statuses(const FScfgWorker::FOutcomes& Outcomes)
{
	FString Out;
	for (const scfg::watcher::StepOutcome& O : Outcomes)
	{
		Out += (Out.IsEmpty() ? TEXT("") : TEXT(",")) + ScfgString::FromUtf8(O.status) +
			(O.error ? TEXT("/") + ScfgString::FromUtf8(*O.error) : FString());
	}
	return Out;
}

}  // namespace ScfgWorkerTests

#define SCFG_WORKER_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Worker." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

SCFG_WORKER_TEST(FScfgWorkerFirstSnapshotTest, "TheFirstSnapshotIsTakenAtCreation")

// test_initial_snapshot_taken_at_construction; nothing runs before Start().
bool FScfgWorkerFirstSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	const Result<void> Made = W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}}), Receipts());
	if (!TestTrue(TEXT("created"), Made.ok()))
	{
		return false;
	}
	TestEqual(TEXT("pending in the first snapshot"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 1);
	TestEqual(TEXT("owned in the first snapshot"), static_cast<int32>(W.Worker->GetSnapshot().owned.size()), 1);
	W.Frames(5, 100.0f);
	TestEqual(TEXT("no request before Start()"), W.Stats->Methods.Num(), 0);
	TestFalse(TEXT("not running"), W.Worker->IsRunning());
	TestFalse(TEXT("no tick before Start()"), W.Worker->IsTickPending());
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerRefusesBadStoresTest, "AStoreThatIsNotADictIsRefused")

// A state.json whose pending or receipts is not an object is refused before the Engine sees it (Engine::make).
bool FScfgWorkerRefusesBadStoresTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	{
		FWorld W;
		const Result<void> Made = W.Make(Json::list({1, 2}), Json());
		TestTrue(TEXT("pending list refused"), !Made.ok() && Made.err().kind == "TypeError");
		TestFalse(TEXT("no worker"), W.Worker.IsValid());
		TestEqual(TEXT("its transport was shut down"), W.Stats->Shutdowns, 1);
	}
	{
		FWorld W;
		const Result<void> Made = W.Make(Json(), Json("receipts"));
		TestTrue(TEXT("receipts str refused"), !Made.ok() && Made.err().kind == "TypeError");
		if (!Made.ok())
		{
			TestEqual(TEXT("message"), ScfgString::FromUtf8(Made.err().message),
				TEXT("an Engine's receipts must be a dict or None, not str"));
		}
	}
	{
		FWorld W;
		TestTrue(TEXT("None and {} are empty stores"), W.Make(Json(), Json::dict()).ok());
	}
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerReverifyOnceTest, "TheFirstTickReverifiesOnlyOnce")

// test_run_once_reverifies_only_first_time_and_publishes_snapshot, test_first_run_reverifies_once: the first tick
// runs Engine::reverify() (one eth_getLogs in the receipt's block) before Engine::tick(); later ticks do not.
bool FScfgWorkerReverifyOnceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json(), Receipts()).ok()))
	{
		return false;
	}
	// the node refuses eth_getLogs: an RpcError keeps the receipt (an empty answer would revoke it)
	W.Fake->RpcErrorFor = [](const FString& Method) -> std::optional<Json>
	{
		if (Method == TEXT("eth_getLogs"))
		{
			return Json::dict({{"code", -32000}, {"message", "internal error"}});
		}
		return std::nullopt;
	};
	TGot<FScfgWorker::FOutcomes> First;
	W.Worker->Tick(First.Callback());
	W.Worker->Start();
	W.Frames(20);
	TestTrue(TEXT("first tick done"), First.Ok());
	const int32 AfterFirst = W.Stats->Methods.Num();
	TestTrue(TEXT("the reverify asked the node"), AfterFirst > 0);
	TestEqual(TEXT("the receipt is kept"), static_cast<int32>(W.Worker->GetSnapshot().receipts.as_dict().size()), 1);
	TGot<FScfgWorker::FOutcomes> Second;
	W.Worker->Tick(Second.Callback());
	W.Frames(20);
	TestTrue(TEXT("second tick done"), Second.Ok());
	TestEqual(TEXT("no reverify on the second tick (and no session to step)"), W.Stats->Methods.Num(), AfterFirst);
	TestEqual(TEXT("errors"), W.Errors->Num(), 0);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerOutcomesTest, "OnlyNewsIsPublishedAsAnOutcome")

// test_waiting_outcomes_without_error_are_not_published, test_waiting_outcome_with_error_is_published: a session that
// is still waiting is not news; one that expired is; a session whose step crashed is "waiting" with an error, and the
// crash goes to OnError (the Engine's contained error).
bool FScfgWorkerOutcomesTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	Json Pending = Json::dict({{"ps_wait", Awaiting("ps_wait", kNow - 60)}, {"ps_old", Awaiting("ps_old", kNow - 31 * 60)},
		{"ps_bad", Json::dict({{"chain", "base"}, {"state", "awaiting"}, {"ui_status", "cancelled"}})}});
	if (!TestTrue(TEXT("created"), W.Make(std::move(Pending)).ok()))
	{
		return false;
	}
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Tick(Tick.Callback());
	W.Worker->Start();
	W.Frames(30);
	if (!TestTrue(TEXT("tick done"), Tick.Ok()))
	{
		return false;
	}
	TestEqual(TEXT("every outcome reaches Tick()'s callback"), Statuses(Tick.Value->value()),
		TEXT("waiting,waiting,waiting/internal_error,expired"));
	TestEqual(TEXT("only news is drained"), Statuses(W.Worker->DrainOutcomes()), TEXT("waiting/internal_error,expired"));
	TestEqual(TEXT("drained once"), static_cast<int32>(W.Worker->DrainOutcomes().size()), 0);
	TestEqual(TEXT("the crash was reported once"), W.Errors->Num(), 1);
	if (W.Errors->Num() == 1)
	{
		TestEqual(TEXT("the crash"), (*W.Errors)[0], TEXT("KeyError: 'created_at'"));
	}
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerCommandsBeforeTickTest, "CommandsRunBeforeTheTickInOrder")

// test_submitted_commands_run_before_tick_and_resolve_futures, test_pump_commands_run_before_a_due_tick: the queued
// commands run in order, then the tick they asked for; a failing command gets its Err; nothing is delivered inside
// the call that submitted it.
bool FScfgWorkerCommandsBeforeTickTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	W.Fake->Delay = 0;  // answers at once: the commands still come back from the ticker, not from the call
	TArray<FString> Order;
	std::string Sid;
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [&](Result<Json> R)
	{
		Order.Add(TEXT("start_session"));
		if (R.ok())
		{
			Sid = R.value().as_dict().get("id").as_str();
		}
	});
	W.Worker->ReportHint("ps_nope", Json(kTx), [&](scfg::Status S)
	{
		Order.Add(S.ok() ? TEXT("hint ok") : TEXT("hint ") + ScfgString::FromUtf8(S.err().kind));
	});
	W.Worker->Tick([&](Result<FScfgWorker::FOutcomes> R)
	{
		Order.Add(R.ok() ? TEXT("tick ") + Statuses(R.value()) : TEXT("tick failed"));
	});
	TestEqual(TEXT("nothing is delivered inside the calls"), Order.Num(), 0);
	TestEqual(TEXT("two queued"), W.Worker->NumQueued(), 2);
	W.Worker->Start();
	W.Frame();
	TestEqual(TEXT("order"), FString::Join(Order, TEXT("; ")), TEXT("start_session; hint KeyError; tick waiting"));
	TestTrue(TEXT("a session id"), Sid.rfind("ps_", 0) == 0);
	// start_session's eth_blockNumber, then the tick's step of the new session
	TestEqual(TEXT("requests"), FString::Join(W.Stats->Methods, TEXT(",")), TEXT("eth_blockNumber,eth_blockNumber,eth_getLogs"));
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerCommandFromCallbackTest, "ACommandSubmittedByACallbackRunsBeforeTheTick")

// test_a_command_submitted_by_a_command_runs_before_the_same_tick: a command submitted from a callback joins the
// queue ahead of the pending tick.
bool FScfgWorkerCommandFromCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	TGot<void> Discarded;
	FScfgWorker& Worker = *W.Worker;
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [&](Result<Json> R)
	{
		if (R.ok())
		{
			Worker.Discard(R.value().as_dict().get("id"), Discarded.Callback());  // abandoned before paying
		}
	});
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Tick(Tick.Callback());
	W.Worker->Start();
	W.Frames(10);
	TestTrue(TEXT("discarded"), Discarded.Ok());
	TestTrue(TEXT("tick done"), Tick.Ok());
	TestEqual(TEXT("the tick saw no session"), Tick.Value ? Statuses(Tick.Value->value()) : FString(TEXT("-")), TEXT(""));
	TestEqual(TEXT("only start_session's request"), FString::Join(W.Stats->Methods, TEXT(",")), TEXT("eth_blockNumber"));
	TestEqual(TEXT("no session left"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 0);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerNoInterleavingTest, "CommandsNeverInterleaveWithATick")

// test_commands_never_interleave_with_a_tick, test_pump_a_command_submitted_mid_tick_waits_for_the_tick,
// test_pump_a_submit_landing_mid_run_gets_its_follow_up_at_once: a command submitted while the tick waits for the
// network runs after it, then its own follow-up tick runs at once (not an interval later).
bool FScfgWorkerNoInterleavingTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}})).ok()))
	{
		return false;
	}
	*W.Interval = 1000;
	W.Fake->bHold = true;
	W.Worker->Start();
	W.Frame();
	TestTrue(TEXT("the first tick waits for eth_blockNumber"), W.Worker->IsBusy() && W.Fake->NumHeld() == 1);
	TGot<void> Hint;
	bool bBusyDuring = false;
	const scfg::engine::Engine& Engine = W.Worker->GetEngine();
	W.Worker->SetUiStatus("ps_a", "pending", [&](scfg::Status S)
	{
		bBusyDuring = Engine.running();
		Hint.Value.emplace(std::move(S));
	});
	W.Frames(5);
	TestFalse(TEXT("not run while the tick waits"), Hint.Value.has_value());
	TestEqual(TEXT("queued"), W.Worker->NumQueued(), 1);
	W.Fake->bHold = false;
	W.Frames(10);
	TestTrue(TEXT("ran after the tick"), Hint.Ok());
	TestFalse(TEXT("the Engine was idle when it ran"), bBusyDuring);
	// tick 1 (eth_blockNumber, eth_getLogs), then the follow-up tick at once, though the interval is 1000 s
	TestEqual(TEXT("requests"), FString::Join(W.Stats->Methods, TEXT(",")),
		TEXT("eth_blockNumber,eth_getLogs,eth_blockNumber,eth_getLogs"));
	TestEqual(TEXT("ui_status"), ScfgString::FromUtf8(W.Worker->GetSnapshot().pending.as_dict().get("ps_a").as_dict()
		.get("ui_status").as_str()), TEXT("pending"));
	W.Frames(20, 10.0f);
	TestEqual(TEXT("then the interval"), W.Stats->Methods.Num(), 4);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerOneFollowUpTest, "WakesDuringATickGiveExactlyOneFollowUp")

// test_wake_during_a_run_schedules_exactly_one_follow_up, test_wake_and_submit_from_a_command_give_one_follow_up:
// ticks are coalesced, and every Tick() caller is answered by the one that runs for it.
bool FScfgWorkerOneFollowUpTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}})).ok()))
	{
		return false;
	}
	*W.Interval = 1000;
	W.Fake->bHold = true;
	TGot<FScfgWorker::FOutcomes> First;
	W.Worker->Tick(First.Callback());
	W.Worker->Start();
	W.Frame();
	TGot<FScfgWorker::FOutcomes> A;
	TGot<FScfgWorker::FOutcomes> B;
	W.Worker->Tick(A.Callback());
	W.Worker->Tick(B.Callback());
	W.Worker->Tick();
	TestTrue(TEXT("one tick pending"), W.Worker->IsTickPending());
	W.Fake->bHold = false;
	W.Frames(20);
	TestTrue(TEXT("first answered"), First.Ok());
	TestTrue(TEXT("both joiners answered"), A.Ok() && B.Ok());
	TestEqual(TEXT("two ticks in all"), W.Count(TEXT("eth_getLogs")), 2);
	TestFalse(TEXT("nothing pending"), W.Worker->IsTickPending() || W.Worker->IsBusy());
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerIntervalTest, "TicksComeEveryInterval")

// test_timer_loop_wake_and_stop, test_pump_a_run_without_a_mid_run_wake_waits_the_interval: Start() ticks at once,
// then every Interval() seconds after a tick ended (read anew each time); a command wakes the worker sooner.
bool FScfgWorkerIntervalTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}})).ok()))
	{
		return false;
	}
	*W.Interval = 10;
	W.Fake->Delay = 0;
	W.Worker->Start();
	W.Frame(0.0f);
	TestEqual(TEXT("the first tick at once"), W.Count(TEXT("eth_getLogs")), 1);
	W.Frames(9, 1.0f);
	TestEqual(TEXT("not before the interval"), W.Count(TEXT("eth_getLogs")), 1);
	*W.Interval = 3;  // read when the next tick ends
	W.Frame(1.0f);
	TestEqual(TEXT("after the interval"), W.Count(TEXT("eth_getLogs")), 2);
	W.Frames(2, 1.0f);
	TestEqual(TEXT("then the new interval"), W.Count(TEXT("eth_getLogs")), 2);
	W.Frame(1.0f);
	TestEqual(TEXT("3 s later"), W.Count(TEXT("eth_getLogs")), 3);
	*W.Interval = 1000;
	W.Frames(3, 1.0f);
	TestEqual(TEXT("one more after 3 s"), W.Count(TEXT("eth_getLogs")), 4);
	const int32 Before = W.Count(TEXT("eth_getLogs"));
	W.Worker->SetUiStatus("ps_a", "pending", nullptr);
	W.Frame(0.0f);
	TestEqual(TEXT("a command brings a tick at once"), W.Count(TEXT("eth_getLogs")), Before + 1);
	TestTrue(TEXT("running"), W.Worker->IsRunning());
	W.Worker->Stop();
	TestFalse(TEXT("stopped"), W.Worker->IsRunning());
	W.Frames(5, 1000.0f);
	TestEqual(TEXT("no tick after Stop()"), W.Count(TEXT("eth_getLogs")), Before + 1);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerFailingTickTest, "AFailingTickIsReportedAndTheNextComesLater")

// test_loop_survives_exceptions, test_pump_a_failing_tick_reports_and_schedules_the_next,
// test_a_failing_reverify_ends_the_pass_and_is_not_retried: a BaseException from the network (no `except Exception`
// contains it) fails the reverify, which ends the first tick and is not retried; a failing tick is reported once and
// the next comes an interval later.
bool FScfgWorkerFailingTickTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}}), Receipts()).ok()))
	{
		return false;
	}
	*W.Interval = 10;
	W.Fake->Delay = 0;
	TSharedRef<bool> bBroken = MakeShared<bool>(true);
	W.Fake->Answer = [bBroken](const FString& Method) -> Result<Json>
	{
		if (*bBroken)
		{
			return scfg::Err::make("BaseException", "the node is down hard");
		}
		return Method == TEXT("eth_blockNumber") ? Result<Json>(Json("0x3e8")) : Result<Json>(Json::list());
	};
	TGot<FScfgWorker::FOutcomes> First;
	TGot<FScfgWorker::FOutcomes> AlsoWaiting;
	TGot<FScfgWorker::FOutcomes> AndAThird;
	W.Worker->Tick(First.Callback());
	W.Worker->Tick(AlsoWaiting.Callback());
	W.Worker->Tick(AndAThird.Callback());
	W.Worker->Start();
	W.Frame(0.0f);
	TestEqual(TEXT("the first tick failed with the reverify's Err"), First.Kind(), TEXT("BaseException"));
	TestTrue(TEXT("every waiter got it"), AlsoWaiting.Kind() == TEXT("BaseException") && AndAThird.Kind() == TEXT("BaseException"));
	TestEqual(TEXT("reported once, not once per waiter"), W.Errors->Num(), 1);
	TestEqual(TEXT("the session was not stepped"), W.Count(TEXT("eth_getLogs")), 0);
	const int32 ReverifyRequests = W.Stats->Methods.Num();
	TestEqual(TEXT("the reverify asked once (eth_blockNumber)"), ReverifyRequests, 1);
	W.Frames(9, 1.0f);
	TestEqual(TEXT("nothing before the interval"), W.Stats->Methods.Num(), ReverifyRequests);
	TGot<FScfgWorker::FOutcomes> Second;
	W.Frames(2, 1.0f);
	TestEqual(TEXT("the second tick failed too, and was reported"), W.Errors->Num(), 2);
	TestEqual(TEXT("no second reverify: the tick stepped the session"), W.Stats->Methods.Num(), ReverifyRequests + 1);
	*bBroken = false;
	W.Worker->Tick(Second.Callback());
	W.Frame(0.0f);
	TestTrue(TEXT("a tick on a healthy node"), Second.Ok());
	TestEqual(TEXT("no new error"), W.Errors->Num(), 2);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerStopTest, "StopCancelsEverything")

// test_pump_stop_cancels_queued_commands_and_runs_nothing_more, test_no_run_after_stop_even_if_a_run_is_in_flight,
// test_cancelled_job_is_skipped, test_submit_on_a_stopped_worker_is_cancelled_but_before_start_it_queues: Stop()
// cancels the running command and the queued ones (CancelledError, on the next frame), a later answer resumes nothing,
// a command submitted while stopped is cancelled, and Start() accepts work again.
bool FScfgWorkerStopTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	TGot<Json> Early;
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), Early.Callback());
	W.Frames(3);
	TestFalse(TEXT("before Start() it queues"), Early.Value.has_value());
	W.Fake->bHold = true;
	W.Worker->Start();
	W.Frame();
	TestTrue(TEXT("start_session waits for the node"), W.Worker->IsBusy() && W.Fake->NumHeld() == 1);
	TGot<void> Queued;
	W.Worker->SetUiStatus("ps_x", "pending", Queued.Callback());
	W.Worker->Stop();
	TestFalse(TEXT("not delivered inside Stop()"), Early.Value.has_value() || Queued.Value.has_value());
	TestFalse(TEXT("the Engine is free at once"), W.Worker->GetEngine().running());
	W.Frame();
	TestEqual(TEXT("the running command"), Early.Kind(), TEXT("CancelledError"));
	TestEqual(TEXT("the queued command"), Queued.Kind(), TEXT("CancelledError"));
	W.Fake->bHold = false;
	W.Frames(5, 100.0f);  // the late answer resumes nothing, and no tick comes
	TestEqual(TEXT("one request in all"), W.Stats->Methods.Num(), 1);
	TestEqual(TEXT("no session was stored"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 0);
	TGot<void> Late;
	TGot<FScfgWorker::FOutcomes> LateTick;
	W.Worker->Discard("ps_x", Late.Callback());
	W.Worker->Tick(LateTick.Callback());
	TestFalse(TEXT("not inside the call"), Late.Value.has_value());
	W.Frame();
	TestEqual(TEXT("a submit while stopped"), Late.Kind(), TEXT("CancelledError"));
	TestEqual(TEXT("a tick while stopped"), LateTick.Kind(), TEXT("CancelledError"));
	W.Worker->Start();
	TGot<void> Again;
	W.Worker->Discard("ps_x", Again.Callback());
	W.Frames(5);
	TestTrue(TEXT("Start() accepts work again"), Again.Ok());
	TestEqual(TEXT("errors"), W.Errors->Num(), 0);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerStopSnapshotTest, "StopTakesAFreshSnapshot")

// No Python or Godot twin (their worker lets the pass finish): an answer that arrived since the last frame has
// written its session into the Engine, and the command was not finished by the ticker yet; Stop() takes the snapshot
// from the Engine, so GetSnapshot() (what the facade's Shutdown saves) has the session (final review F4).
bool FScfgWorkerStopSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	W.Worker->Start();
	W.Frames(10);  // the startup tick
	TGot<Json> Started;
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), Started.Callback());
	W.Ticker.Tick(0.1f);  // the command starts and asks the node
	TestEqual(TEXT("no session yet"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 0);
	W.Fake->Pump();  // the answer arrives: the Engine stores the session; no worker frame since
	TestEqual(TEXT("the Engine has it"), static_cast<int32>(W.Worker->GetEngine().pending().as_dict().size()), 1);
	TestEqual(TEXT("the last snapshot does not"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 0);
	W.Worker->Stop();
	TestEqual(TEXT("Stop() took a fresh snapshot"), static_cast<int32>(W.Worker->GetSnapshot().pending.as_dict().size()), 1);
	W.Frame();
	TestTrue(TEXT("the command's callback still comes, on the next frame"), Started.Value.has_value());
	TestEqual(TEXT("errors"), W.Errors->Num(), 0);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerStateChangedTest, "OnStateChangedComesBeforeTheCallback")

// The store (task 4) saves from OnStateChanged: after every command the Snapshot is refreshed, then OnStateChanged
// fires, then the command's callback; a command cancelled before it started changes nothing.
bool FScfgWorkerStateChangedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	TArray<FString> Order;
	FScfgWorker& Worker = *W.Worker;
	W.Worker->OnStateChanged->AddLambda([&]()
	{
		Order.Add(FString::Printf(TEXT("changed(%d)"), static_cast<int32>(Worker.GetSnapshot().pending.as_dict().size())));
	});
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [&](Result<Json> R)
	{
		Order.Add(FString::Printf(TEXT("started(%d)"), static_cast<int32>(Worker.GetSnapshot().pending.as_dict().size())));
	});
	W.Worker->Start();
	W.Frames(10);
	TestEqual(TEXT("order"), FString::Join(Order, TEXT(" ")), TEXT("changed(1) started(1) changed(1)"));  // then the tick
	Order.Reset();
	W.Worker->Stop();
	W.Worker->Discard("ps_y", [&](scfg::Status) { Order.Add(TEXT("cancelled")); });
	W.Frame();
	TestEqual(TEXT("a cancelled command"), FString::Join(Order, TEXT(" ")), TEXT("cancelled"));
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerTeardownTest, "DestroyingTheWorkerTearsDownInOrder")

// Teardown: the running Job is destroyed first (no callback), then the Engine, then the transport is shut down
// once, dropping the request still held; a callback may destroy the worker itself.
bool FScfgWorkerTeardownTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	{
		FWorld W;
		if (!TestTrue(TEXT("created"), W.Make(Json::dict({{"ps_a", Awaiting("ps_a", kNow)}})).ok()))
		{
			return false;
		}
		W.Fake->bHold = true;
		TGot<Json> Pending;
		W.Worker->Start();
		W.Frame();
		W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), Pending.Callback());
		TestTrue(TEXT("the tick waits"), W.Worker->IsBusy() && W.Fake->NumHeld() == 1);
		W.Fake = nullptr;
		W.Worker.Reset();
		TestEqual(TEXT("shut down once"), W.Stats->Shutdowns, 1);
		TestEqual(TEXT("the held request was dropped"), W.Stats->DroppedAtShutdown, 1);
		W.Frames(3);
		TestFalse(TEXT("no callback after destruction"), Pending.Value.has_value());
	}
	{
		FWorld W;
		if (!TestTrue(TEXT("created"), W.Make().ok()))
		{
			return false;
		}
		int32 Calls = 0;
		W.Worker->Discard("ps_a", [&](scfg::Status)
		{
			++Calls;
			W.Fake = nullptr;
			W.Worker.Reset();  // from inside the worker's own ticker
		});
		W.Worker->Discard("ps_b", [&](scfg::Status) { ++Calls; });
		W.Worker->Start();
		W.Frames(3);
		TestEqual(TEXT("the callback destroyed the worker, and nothing ran after"), Calls, 1);
		TestEqual(TEXT("shut down once"), W.Stats->Shutdowns, 1);
	}
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerStopFromOnErrorTest, "AStopFromInsideTheEngineIsDeferred")

// OnError runs inside the tick's coroutine (the Engine's contained error): a Stop() there would destroy the frame
// that is running, so it waits for the next frame, which cancels the tick.
bool FScfgWorkerStopFromOnErrorTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	Json Bad = Json::dict({{"chain", "base"}, {"state", "awaiting"}, {"ui_status", "cancelled"}});
	Json Pending = Json::dict({{"ps_bad", std::move(Bad)}, {"ps_a", Awaiting("ps_a", kNow)}});
	TUniquePtr<FFakeTransport> Transport = MakeUnique<FFakeTransport>(W.Stats);
	W.Fake = Transport.Get();
	FScfgWorker* Raw = nullptr;
	int32 Reported = 0;
	FScfgWorker::FOptions Options;
	Options.Ticker = &W.Ticker;
	Options.Clock = []() { return kNow; };
	Options.OnError = [&](const scfg::Err&)
	{
		++Reported;
		Raw->Stop();
	};
	Result<TUniquePtr<FScfgWorker>> Made =
		FScfgWorker::Create(BaseConfig(), MoveTemp(Transport), std::move(Pending), Json(), MoveTemp(Options));
	if (!TestTrue(TEXT("created"), Made.ok()))
	{
		return false;
	}
	W.Worker = Made.take();
	Raw = W.Worker.Get();
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Tick(Tick.Callback());
	W.Worker->Start();
	W.Frame();
	TestEqual(TEXT("ps_bad's crash reported"), Reported, 1);
	TestTrue(TEXT("still running inside the frame: the stop is deferred"), W.Worker->IsBusy());
	W.Frames(3);
	TestEqual(TEXT("then the tick was cancelled"), Tick.Kind(), TEXT("CancelledError"));
	TestFalse(TEXT("stopped"), W.Worker->IsRunning());
	TestEqual(TEXT("reported once"), Reported, 1);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerErrorsLoggedOnceTest, "EveryErrIsReportedOnce")

// The worker owns "an Err is logged once": a command's Err goes to OnError whether it has a callback or not (the
// callback gets the Result too, and must not log it again); a cancelled command is not an error.
bool FScfgWorkerErrorsLoggedOnceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	W.Worker->ReportHint("ps_gone", Json(kTx), nullptr);  // no callback: still reported
	TGot<void> WithCallback;
	W.Worker->ReportHint("ps_other", Json(kTx), WithCallback.Callback());
	W.Worker->Start();
	W.Frames(5);
	TestEqual(TEXT("the callback got the Err"), WithCallback.Kind(), TEXT("KeyError"));
	TestEqual(TEXT("both reported, once each"), FString::Join(*W.Errors, TEXT(" | ")),
		TEXT("KeyError: 'ps_gone' | KeyError: 'ps_other'"));
	W.Worker->Stop();
	W.Worker->ReportHint("ps_late", Json(kTx), nullptr);  // cancelled: not an error
	W.Frames(3);
	TestEqual(TEXT("a cancellation is not reported"), W.Errors->Num(), 2);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerDestroyFromHandlersTest, "AHandlerMayDestroyTheWorker")

// OnStateChanged and OnError (called from the ticker) may destroy the worker: it broadcasts from a copy of its
// handler list, calls a copy of OnError, and touches nothing after, not even the next handler. (The installed editor
// is not an ASan build: this shows the path runs; the CMake asan preset does not compile the glue.)
bool FScfgWorkerDestroyFromHandlersTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	{
		FWorld W;
		if (!TestTrue(TEXT("created"), W.Make().ok()))
		{
			return false;
		}
		int32 Calls = 0;
		TGot<void> Never;
		W.Worker->OnStateChanged->AddLambda([&]()
		{
			++Calls;
			W.Fake = nullptr;
			W.Worker.Reset();
		});
		W.Worker->OnStateChanged->AddLambda([&]() { Calls += 100; });  // after the destroy: never called
		W.Worker->Discard("ps_a", Never.Callback());
		W.Worker->Start();
		W.Frames(3);
		TestFalse(TEXT("destroyed from OnStateChanged"), W.Worker.IsValid());
		TestEqual(TEXT("no handler ran after the one that destroyed the worker"), Calls, 1);
		TestFalse(TEXT("no callback after"), Never.Value.has_value());
		TestEqual(TEXT("shut down once"), W.Stats->Shutdowns, 1);
	}
	{
		FWorld W;
		TUniquePtr<FFakeTransport> Transport = MakeUnique<FFakeTransport>(W.Stats);
		FScfgWorker::FOptions Options;
		Options.Ticker = &W.Ticker;
		int32 Reported = 0;
		Options.OnError = [&](const scfg::Err&)
		{
			++Reported;
			W.Worker.Reset();  // from the ticker, outside the Engine
		};
		Result<TUniquePtr<FScfgWorker>> Made =
			FScfgWorker::Create(BaseConfig(), MoveTemp(Transport), Json(), Json(), MoveTemp(Options));
		if (!TestTrue(TEXT("created"), Made.ok()))
		{
			return false;
		}
		W.Worker = Made.take();
		TGot<void> Hint;
		W.Worker->ReportHint("ps_gone", Json(kTx), Hint.Callback());
		W.Worker->Start();
		W.Frames(3);
		TestFalse(TEXT("destroyed from OnError"), W.Worker.IsValid());
		TestEqual(TEXT("reported once"), Reported, 1);
		TestFalse(TEXT("its callback never ran"), Hint.Value.has_value());
		TestEqual(TEXT("shut down once"), W.Stats->Shutdowns, 1);
	}
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerCancelledReverifyTest, "ACancelledStartupReverifyRunsAgain")

// Stop() while the startup reverify waits for the node, then Start(): the reverify is owed, so the next tick runs it
// (a failed one is not retried; a cancelled one is).
bool FScfgWorkerCancelledReverifyTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json(), Receipts()).ok()))
	{
		return false;
	}
	W.Fake->RpcErrorFor = [](const FString& Method) -> std::optional<Json>
	{
		if (Method == TEXT("eth_getLogs"))
		{
			return Json::dict({{"code", -32000}, {"message", "internal error"}});  // keep the receipt
		}
		return std::nullopt;
	};
	W.Fake->bHold = true;
	W.Worker->Start();
	W.Frame();
	TestTrue(TEXT("the reverify waits for the node"), W.Worker->IsBusy() && W.Fake->NumHeld() == 1);
	W.Worker->Stop();
	W.Frame();
	W.Fake->bHold = false;
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Start();
	W.Worker->Tick(Tick.Callback());
	W.Frames(20);
	TestTrue(TEXT("the tick after the restart"), Tick.Ok());
	TestTrue(TEXT("it ran the reverify (its eth_getLogs in the receipt's block)"), W.Count(TEXT("eth_getLogs")) >= 1);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerStopThenStartInOnErrorTest, "AStartAfterADeferredStopWins")

// Inside the Engine (a contained error), Stop() is deferred: IsRunning() is false at once, and a Start() after it
// wins, so the next frame does not stop the worker.
bool FScfgWorkerStopThenStartInOnErrorTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	Json Bad = Json::dict({{"chain", "base"}, {"state", "awaiting"}, {"ui_status", "cancelled"}});
	TUniquePtr<FFakeTransport> Transport = MakeUnique<FFakeTransport>(W.Stats);
	FScfgWorker* Raw = nullptr;
	bool bRunningAfterStop = true;
	FScfgWorker::FOptions Options;
	Options.Ticker = &W.Ticker;
	Options.Clock = []() { return kNow; };
	Options.OnError = [&](const scfg::Err&)
	{
		Raw->Stop();
		bRunningAfterStop = Raw->IsRunning();
		Raw->Start();
	};
	Result<TUniquePtr<FScfgWorker>> Made = FScfgWorker::Create(
		BaseConfig(), MoveTemp(Transport), Json::dict({{"ps_bad", std::move(Bad)}}), Json(), MoveTemp(Options));
	if (!TestTrue(TEXT("created"), Made.ok()))
	{
		return false;
	}
	W.Worker = Made.take();
	Raw = W.Worker.Get();
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Tick(Tick.Callback());
	W.Worker->Start();
	W.Frames(5);
	TestFalse(TEXT("IsRunning() is false once a stop is wanted"), bRunningAfterStop);
	TestTrue(TEXT("the Start() after it won"), W.Worker->IsRunning());
	TestTrue(TEXT("the tick finished, not cancelled"), Tick.Ok());
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerReceiptCommandsTest, "TheReceiptAndSolanaCommandsRunThroughTheWorker")

// Reverify, RestoreLookup, RestoreApply and BuildSolanaPayment, each through the worker's queue over the fake node.
bool FScfgWorkerReceiptCommandsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json(), Receipts()).ok()))
	{
		return false;
	}
	TSharedRef<bool> bKeep = MakeShared<bool>(true);
	W.Fake->RpcErrorFor = [bKeep](const FString& Method) -> std::optional<Json>
	{
		if (*bKeep && Method == TEXT("eth_getLogs"))
		{
			return Json::dict({{"code", -32000}, {"message", "internal error"}});
		}
		return std::nullopt;
	};
	W.Fake->Answer = [Fake = W.Fake](const FString& Method) -> Result<Json>
	{
		if (Method == TEXT("eth_blockNumber"))
		{
			return Json(ScfgString::ToUtf8(FString::Printf(TEXT("0x%llx"), static_cast<unsigned long long>(Fake->Head++))));
		}
		if (Method == TEXT("eth_getLogs"))
		{
			return Json::list();
		}
		return Json();  // "not found" for every lookup
	};
	W.Worker->Start();
	W.Frames(20);  // the startup reverify keeps the receipt (the node refuses eth_getLogs)
	TestEqual(TEXT("kept at startup"), static_cast<int32>(W.Worker->GetSnapshot().receipts.as_dict().size()), 1);

	*bKeep = false;  // now the node finds no such log: an explicit Reverify revokes the receipt
	TGot<Json> Removed;
	W.Worker->Reverify(Removed.Callback());
	W.Frames(20);
	if (TestTrue(TEXT("reverify"), Removed.Ok()))
	{
		TestTrue(TEXT("the receipt's key is returned"), Removed.Value->value().is_list() &&
			Removed.Value->value().as_list().size() == 1);
	}
	TestEqual(TEXT("revoked"), static_cast<int32>(W.Worker->GetSnapshot().receipts.as_dict().size()), 0);

	TGot<std::vector<scfg::restore::RestoreCandidate>> Lookup;
	W.Worker->RestoreLookup(Json(kTx), Lookup.Callback());
	W.Frames(200);
	TestTrue(TEXT("restore_lookup answered"), Lookup.Value.has_value());
	if (Lookup.Value && !Lookup.Value->ok())
	{
		AddInfo(FString::Printf(TEXT("restore_lookup: %s"), *ScfgString::FromUtf8(Lookup.Value->err().str())));
	}
	TestEqual(TEXT("an unknown transaction"), Lookup.Value && !Lookup.Value->ok() ?
		ScfgString::FromUtf8(Lookup.Value->err().code) : FString(TEXT("-")), TEXT("tx_not_found"));

	TGot<Json> Applied;
	W.Worker->RestoreApply({}, std::nullopt, Applied.Callback());
	W.Frames(3);
	TestTrue(TEXT("restore_apply of nothing adds nothing"), Applied.Ok() && Applied.Value->value().is_list() &&
		Applied.Value->value().as_list().empty());

	std::string Sid;
	W.Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [&Sid](Result<Json> R)
	{
		if (R.ok())
		{
			Sid = R.value().as_dict().get("id").as_str();
		}
	});
	W.Frames(10);
	const int32 ErrorsBefore = W.Errors->Num();
	TGot<Json> Payment;
	W.Worker->BuildSolanaPayment(Json(Sid), Json("4Nd1mBQtrMJVYVfKf2PJy9NZUZdTAsp7D4xWLs4gDB4T"), Payment.Callback());
	W.Frames(10);
	TestTrue(TEXT("an EVM session has no Solana payment"), Payment.Value.has_value() && !Payment.Value->ok());
	if (Payment.Value && !Payment.Value->ok())
	{
		AddInfo(FString::Printf(TEXT("build_solana_payment: %s"), *ScfgString::FromUtf8(Payment.Value->err().str())));
	}
	TestEqual(TEXT("and its Err is reported once"), W.Errors->Num(), ErrorsBefore + 1);
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerHandlersTest, "OnStateChangedSkipsRemovedAndDeadHandlers")

// A handler removed by an earlier one in the same broadcast is skipped (the engine's own Broadcast does the same), one
// added during it waits for the next broadcast, and one bound to a shared owner that has died is not called.
bool FScfgWorkerHandlersTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	TArray<FString> Calls;
	FDelegateHandle Second;
	TSharedRef<FScfgStateChanged> Event = W.Worker->OnStateChanged;
	Event->AddLambda([&]()
	{
		Calls.Add(TEXT("first"));
		if (Second.IsValid())
		{
			TestTrue(TEXT("removed"), Event->Remove(Second));
			Second.Reset();
			Event->AddLambda([&]() { Calls.Add(TEXT("added")); });
		}
	});
	Second = Event->AddLambda([&]() { Calls.Add(TEXT("second")); });
	struct FOwner
	{
		int32 Calls = 0;
		void OnChanged() { ++Calls; }
	};
	TSharedPtr<FOwner> Owner = MakeShared<FOwner>();
	Event->AddSP(Owner.ToSharedRef(), &FOwner::OnChanged);
	int32 LambdaOwnerCalls = 0;
	Event->AddSPLambda(Owner.ToSharedRef(), [&LambdaOwnerCalls]() { ++LambdaOwnerCalls; });
	int32 KeptState = 0;
	Event->AddLambda([Count = 0, &KeptState]() mutable { KeptState = ++Count; });  // not copied per broadcast
	TestEqual(TEXT("five handlers"), Event->Num(), 5);
	W.Worker->Discard("ps_a", nullptr);
	W.Worker->Start();
	W.Frame();
	// Two broadcasts in that frame: the Discard's, then its tick's (which asks the fake node nothing).
	TestEqual(TEXT("the removed one was skipped, the added one waited for the next broadcast"),
		FString::Join(Calls, TEXT(" ")), TEXT("first first added"));
	TestTrue(TEXT("the shared owner's handlers ran"), Owner->Calls == 2 && LambdaOwnerCalls == 2);
	Calls.Reset();
	const TWeakPtr<FOwner> Gone = Owner;
	Owner.Reset();
	TestFalse(TEXT("the owner is gone"), Gone.IsValid());
	W.Worker->Discard("ps_b", nullptr);
	W.Frames(3);
	TestEqual(TEXT("the other handlers still run"), FString::Join(Calls, TEXT(" ")), TEXT("first added first added"));
	TestEqual(TEXT("the dead owner's lambda was not called"), LambdaOwnerCalls, 2);
	TestEqual(TEXT("a mutable lambda keeps its state"), KeptState, 4);
	TestFalse(TEXT("removing twice"), Event->Remove(FDelegateHandle()));
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerErrBeforeHandlersTest, "TheErrIsReportedBeforeAHandlerCanDestroyTheWorker")

// A command's Err is reported before the snapshot is refreshed and OnStateChanged runs, so a handler that destroys
// the worker (the facade tearing everything down) does not lose it.
bool FScfgWorkerErrBeforeHandlersTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make().ok()))
	{
		return false;
	}
	FString ErrorsWhenChanged = TEXT("-");
	W.Worker->OnStateChanged->AddLambda([&]()
	{
		ErrorsWhenChanged = FString::Join(*W.Errors, TEXT(" | "));
		W.Fake = nullptr;
		W.Worker.Reset();
	});
	TGot<void> Never;
	W.Worker->SetUiStatus("ps_gone", "cancelled", nullptr);  // unknown sid: no Err
	W.Worker->ReportHint("ps_gone", Json(kTx), Never.Callback());
	W.Worker->Start();
	W.Frames(3);
	TestFalse(TEXT("destroyed by the handler"), W.Worker.IsValid());
	TestEqual(TEXT("the first command had no Err"), ErrorsWhenChanged, TEXT(""));
	TestEqual(TEXT("nothing reported"), W.Errors->Num(), 0);
	FWorld V;
	if (!TestTrue(TEXT("created"), V.Make().ok()))
	{
		return false;
	}
	V.Worker->OnStateChanged->AddLambda([&]()
	{
		ErrorsWhenChanged = FString::Join(*V.Errors, TEXT(" | "));
		V.Fake = nullptr;
		V.Worker.Reset();
	});
	V.Worker->ReportHint("ps_gone", Json(kTx), Never.Callback());
	V.Worker->Start();
	V.Frames(3);
	TestFalse(TEXT("destroyed by the handler"), V.Worker.IsValid());
	TestEqual(TEXT("the Err was reported before the handler ran"), ErrorsWhenChanged, TEXT("KeyError: 'ps_gone'"));
	TestEqual(TEXT("once"), V.Errors->Num(), 1);
	TestFalse(TEXT("its callback never ran"), Never.Value.has_value());
	return true;
}

SCFG_WORKER_TEST(FScfgWorkerFailedReverifyThenStopTest, "AFailedReverifyKeepsItsErrThroughStop")

// The startup reverify has failed (its answer came this frame) but the worker has not polled it yet when Stop() runs:
// the tick ends with the reverify's Err, which is reported once, not with CancelledError; and it is not retried.
bool FScfgWorkerFailedReverifyThenStopTest::RunTest(const FString& Parameters)
{
	using namespace ScfgWorkerTests;
	FWorld W;
	if (!TestTrue(TEXT("created"), W.Make(Json(), Receipts()).ok()))
	{
		return false;
	}
	W.Fake->Answer = [](const FString&) -> Result<Json>
	{
		return scfg::Err::make("BaseException", "the node is down hard");
	};
	TGot<FScfgWorker::FOutcomes> Tick;
	W.Worker->Tick(Tick.Callback());
	W.Worker->Start();
	W.Frame();
	TestTrue(TEXT("the reverify waits for the node"), W.Worker->IsBusy() && W.Fake->NumHeld() == 1);
	W.Fake->Pump();  // the answer: the reverify fails now, before the worker's ticker runs
	W.Worker->Stop();
	W.Ticker.Tick(0.1f);
	TestEqual(TEXT("the tick got the reverify's Err"), Tick.Kind(), TEXT("BaseException"));
	TestTrue(TEXT("reported once"), W.Errors->Num() == 1 && (*W.Errors)[0].StartsWith(TEXT("BaseException")));
	const int32 Requests = W.Stats->Methods.Num();
	W.Fake->Answer = [](const FString& Method) -> Result<Json>
	{
		return Method == TEXT("eth_blockNumber") ? Result<Json>(Json("0x3e8")) : Result<Json>(Json::list());
	};
	TGot<FScfgWorker::FOutcomes> After;
	W.Worker->Start();
	W.Worker->Tick(After.Callback());
	W.Frames(10);
	TestTrue(TEXT("the next tick"), After.Ok());
	TestEqual(TEXT("no second reverify (no pending session: nothing asked)"), W.Stats->Methods.Num(), Requests);
	return true;
}

#undef SCFG_WORKER_TEST

#endif  // WITH_DEV_AUTOMATION_TESTS
