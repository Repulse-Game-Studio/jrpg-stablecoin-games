// No Python twin: the doubles of godot/tests/test_scfg_api.gd and test_scfg_flows.gd for the facade tests
// (Scfg.Glue.Facade.*): fake EVM nodes behind the worker's own transport (FakeChain), a headless sheet host that
// records what the flows ask for and lets a test script the player (FakeHost and FakeSheet), a fake platform (no
// browser opened, no real clipboard touched), a recording page server (RecordingServer), and FWorld, which builds a
// facade the way Godot's _make / _configured do: its own state directory under Saved/ScfgTests/<random>, a fake
// clock, a private ticker, a worker that never ticks by itself (Godot's ManualTimer: a pass runs only in RunOnce()),
// and quiet reports.
//
// Godot's pump runs from _process; here the tests call Pump() themselves (the facade's ticker element is off), as
// Godot's tests call s._pump(). A screen closed with Close(how) ends its flow in the next Pump(), where Godot resumes
// inside the signal: the tests pump once after each close.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Misc/AutomationTest.h"

#include "IScfgSheetHost.h"
#include "ScfgControllers.h"
#include "ScfgFacade.h"
#include "ScfgPlatform.h"
#include "ScfgTransport.h"
#include "ScfgTypes.h"

#include "Scfg/Checkout.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgFacadeTests
{

using scfg::Json;

extern const char* const kDev;
extern const char* const kPayer;
// "0x" + "ab" * 32.
extern const std::string kTx;

// "0x" + Pair * 32.
std::string Hash(const char* Pair);
// The mainnet Arbitrum USDC contract.
std::string UsdcArbitrum();
// test_scfg_api.gd's _receipt(tx): an arbitrum unlock receipt for chapter2.
Json ReceiptFor(const std::string& Tx);
// fakes.gd's transfer_log(token, sender, recipient, value, {block, tx_hash}).
Json TransferLog(const std::string& Token, const std::string& Sender, const std::string& Recipient, int64_t Value,
	int64_t Block, const std::string& TxHash);
// Two JSON values print the same with sorted keys (key order ignored, true is not 1).
bool Same(const Json& A, const Json& B);
FString Text(const Json& V);

// fakes.gd's FakeChain: an EVM node (head, logs, receipts). bHold keeps every request until Release(); BoomAfter > 0
// fails every request with a RuntimeError after that many frames (test_scfg_api.gd's BoomChain).
struct FFakeChain
{
	int64_t Head = 1000;
	std::vector<Json> Logs;
	std::map<std::string, Json> TxReceipts;
	TArray<FString> Calls;
};

// The fake nodes, by chain key, shared by the test and the transport the worker owns.
struct FFakeChains
{
	std::map<std::string, FFakeChain> Evm;
	bool bHold = false;
	int32 BoomAfter = 0;
	int32 Shutdowns = 0;
	int32 DroppedAtShutdown = 0;
	int32 Posts = 0;
	TArray<FString> Problems;

	FFakeChain& Chain(const char* Key) { return Evm[Key]; }
};

// The worker's transport: answers each JSON-RPC request as the fake chains would (posted by the RPC clients of
// RpcFactory, at fake://evm/<key>), at once, or held (bHold, BoomAfter) until Frame().
class FFakeNodeTransport final : public FScfgTransport
{
public:
	explicit FFakeNodeTransport(TSharedRef<FFakeChains> InChains) : Chains(InChains) {}
	virtual scfg::Future<Json> post_json(std::string Url, Json Payload, double Timeout) override;
	virtual void Shutdown() override;
	// One frame: held requests due now are answered (bHold off), a boom fails them.
	void Frame();

private:
	struct FHeld
	{
		scfg::Promise<Json> Promise;
		scfg::Result<Json> Answer;
		int32 FramesLeft;
	};
	scfg::Result<Json> Answer(const std::string& Url, const Json& Payload);

	TSharedRef<FFakeChains> Chains;
	std::deque<FHeld> Held;
	bool bShutDown = false;
};

// A sheet the test closes with Close(how), holding its controller (Godot's FakeSheet.controller).
class FFakeSheet final : public FScfgScreen
{
public:
	TSharedPtr<FScfgCheckout> Checkout;
	TSharedPtr<FScfgRestore> Restore;
};

// The sheet host the flows show their screens through, recording what they ask for (test_scfg_flows.gd's FakeHost).
// Shown keeps each open sheet alive (the scene tree); Close() drops it (queue_free); Release() drops it without
// closing (a sheet freed by the game).
class FFakeHost final : public IScfgSheetHost
{
public:
	TArray<TWeakPtr<FFakeSheet>> Sheets;  // every sheet opened, newest last
	TArray<TSharedPtr<FFakeSheet>> Shown;
	TArray<FString> Kinds;  // "payment" / "restore", per sheet
	int32 Closes = 0;
	TArray<FString> Notes;
	TSharedRef<TArray<FString>> Journal = MakeShared<TArray<FString>>();  // shared with the recording servers

	virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override;
	virtual TSharedPtr<FScfgScreen> OpenRestore(const TSharedRef<FScfgRestore>& Restore) override;
	virtual void Close(const TSharedRef<FScfgScreen>& Screen) override;
	virtual void Notify(const FString& Text) override { Notes.Add(Text); }

	// Sheets[I] (negative: from the end), while it exists.
	TSharedPtr<FFakeSheet> Sheet(int32 I) const;
	// The player closes sheet I.
	void CloseSheet(int32 I, const FString& How);
	// The game frees sheet I without closing it.
	void Release(int32 I);
	// The checkout or restore controller of sheet I.
	FScfgCheckout& CheckoutOf(int32 I) const;
	FScfgRestore& RestoreOf(int32 I) const;
};

// test_scfg_flows.gd's FakePlatform.
class FFakePlatform final : public IScfgPlatform
{
public:
	bool bMobile = false;
	TArray<FString> Opened;
	FString Clipboard;

	virtual bool IsMobile() const override { return bMobile; }
	virtual void OpenUrl(const FString& Url) override { Opened.Add(Url); }
	virtual void ClipboardSet(const FString& Text) override { Clipboard = Text; }
	virtual FString ClipboardGet() override { return Clipboard; }
};

// test_scfg_flows.gd's RecordingServer: a page that notes in the journal when it is stopped and whether the
// controller still answered then (its get_status), and that it was destroyed.
struct FServerLog
{
	bool bStarted = false;
	bool bStopped = false;
	bool bDestroyed = false;
	bool bHasBuildTx = false;
};
scfg::checkout::ServerFactory RecordingFactory(
	TSharedRef<TArray<FString>> Journal, TSharedRef<TArray<TSharedRef<FServerLog>>> Logs);
extern const char* const kServerUrl;

// A facade as Godot's _make builds the autoload, with the doubles above.
struct FWorld
{
	FString Dir;
	FTSTicker Ticker;
	TSharedRef<double> Now = MakeShared<double>(1000.0);
	TSharedRef<FFakeChains> Chains = MakeShared<FFakeChains>();
	TSharedRef<FFakePlatform> Platform = MakeShared<FFakePlatform>();
	TSharedRef<FFakeHost> Host = MakeShared<FFakeHost>();
	TSharedRef<TArray<FString>> Journal = Host->Journal;
	TSharedRef<TArray<TSharedRef<FServerLog>>> Servers = MakeShared<TArray<TSharedRef<FServerLog>>>();
	FFakeNodeTransport* Transport = nullptr;  // owned by the worker
	TSharedPtr<FScfgFacade> Facade;
	bool bRecordingServer = false;
	// The worker never ticks by itself (Godot's ManualTimer); false keeps the facade's own interval wiring.
	bool bManualWorker = true;
	double TickerOwed = 0;  // seconds the clock moved that the ticker has not yet

	// A new directory under Saved/ScfgTests (Dir); no facade yet.
	FWorld();
	FWorld(const FWorld&) = delete;
	FWorld& operator=(const FWorld&) = delete;
	// The facade first (before the ticker it is registered with), then the directory.
	~FWorld();

	FString StatePath() const { return Dir / TEXT("state.json"); }
	FScfgFacadeOptions Options();
	// _make: a new facade (any earlier one is shut down first), not configured; the host is set.
	FScfgFacade& Make();
	// _configured: Make(), then configure({"wallet": DEV}) and chapter2.
	FScfgFacade& Configured(FAutomationTestBase& Test);
	// test_scfg_flows.gd's _make: configured, the first pump, a worker pass, a pump.
	FScfgFacade& Started(FAutomationTestBase& Test);
	// _drop: the facade goes (Shutdown and release).
	void Drop();

	FScfgFacade& F() const { return *Facade; }
	FScfgWorker& Worker() const { return *Facade->GetWorker(); }
	FScfgReporter& Reports() const { return Facade->GetReporter(); }
	// One frame of the ticker (the worker, the store's retries) and of the fake nodes.
	void Frame();
	// The fake clock moves; the ticker (the store's retry timer) follows at the next Pump(), after the facade's pump,
	// as Godot's pump checks the save interval after it synced the newest snapshot. Pump() ticks the ticker only then:
	// a tick also runs the worker's queued commands, which Godot's manual scheduler runs only in run_once().
	void Advance(double Seconds);
	// worker.run_once(): one pass (the queued commands, then a tick), frames until it ended; false if it never did.
	bool RunOnce();
	// _pass: RunOnce(), then a pump.
	void Pass();
	void Pump();
	// _start: the first pump starts the worker; its first pass; a pump.
	void Start();
	// _run(fn): Fn runs on the Engine in one pass, then a pump; its result.
	scfg::Result<Json> Run(TUniqueFunction<scfg::Result<Json>(scfg::engine::Engine&)> Fn);
	// e.start_session("unlock", "arbitrum", {"item": "chapter2"}) in one pass: the session record.
	Json StartSession();
	scfg::Result<Json> AddReceipt(const std::string& Tx);
	// ScfgStore.load(path).
	Json OnDiskPending() const;
	Json OnDiskReceipts() const;

	// _pay: Pay without waiting; the result lands in the box. Callback: none when null.
	TSharedRef<TOptional<FScfgPaymentResult>> Pay(EScfgPayKind Kind, const FScfgPayRequest& Request,
		TSharedPtr<TArray<FScfgPaymentResult>> Called = nullptr);
	TSharedRef<TOptional<FScfgRestoreResult>> Restore(TSharedPtr<TArray<FScfgRestoreResult>> Called = nullptr);
	// _session: the newest sheet's controller chooses Arbitrum, one pass; a session started.
	FScfgCheckout& Session(FAutomationTestBase& Test);
	// _pay_on_chain: the session paid in a new block on Arbitrum, then 19 more blocks (20 confirmations).
	void PayOnChain(FScfgCheckout& Checkout);
};

// Test.TestEqual of the lines joined, so a failure shows both lists.
bool SameLines(FAutomationTestBase& Test, const TCHAR* What, const TArray<FString>& Actual,
	const TArray<FString>& Expected);

FScfgPayRequest Unlock(const FString& Item = TEXT("chapter2"));
FScfgPayRequest Tip(const FString& Amount, const FString& Token);
FString SessionIdOf(const FScfgCheckout& Checkout);

}  // namespace ScfgFacadeTests

#endif  // WITH_DEV_AUTOMATION_TESTS
