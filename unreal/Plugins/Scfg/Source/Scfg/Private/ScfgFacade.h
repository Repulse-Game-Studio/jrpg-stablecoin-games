// Copyright 2026 Repulse
// Twin of godot/addons/scfg/scfg.gd (the Scfg autoload) and of Unity's SCFG facade, itself the twin of Ren'Py's
// scfg_api.rpy: where this file is silent, scfg.gd's behavior is the reference. UScfgSubsystem (ScfgSubsystem.h) is
// the Blueprint and C++ face of it; this is the engine-object-free part, so the tests drive it directly.
//
// DEVELOPER ERRORS. A misused call reports "scfg: <message>" once (LogScfg Error) and returns the Err (Configure,
// AddChain, AddItem); a query reports the same way and returns its neutral value; Pay / Restore resolve Failed with the
// ConfigError code (the kind, such as "TypeError", for an Err without one). Nothing here crashes the game.
//
// ERROR OWNERSHIP. The worker reports every Err of its commands (a CancelledError excepted) to its OnError, which is
// this facade's reporter ("scfg: worker error: ..."), once; the store reports its own to its OnError ("scfg: cannot
// read the stored receipts: ..." while loading, "scfg: cannot save the payment state: ..." after). The facade never
// logs those again: it only turns them into Failed results. It logs FScfgWorker::Create's Err itself ("scfg: cannot
// start the payment engine: ..."), the one Err nobody else reports.
//
// STATE. The store (FScfgStore, Saved/Scfg/state.json) is read on first use. The worker starts on the first pump frame
// after Configure() (EnsureStarted), or at the first Pay / Restore. The facade binds its own handler to the worker's
// OnStateChanged (weakly, AddSPLambda), mirrors each changed snapshot into the store (Save() true means written or
// owed: the store throttles scan progress and retries a failed save itself), and notes a receipts change for the
// next pump.
//
// THE PUMP (Pump, every frame from the ticker), in Godot's order: screens that were closed (or released) end their
// flow; the worker's outcomes are drained and given to every open and background checkout controller (on_outcome),
// and each result is delivered; then every controller is polled and its callback registered; callbacks of sessions
// that left the Engine are dropped; background controllers whose session is final or older than the browser timeout
// are closed; the restore controllers are polled and a finished one delivered; unready routes are warned about once;
// OnReceiptsChanged fires if the receipts changed.
//
// FLOWS. Pay and Restore build a core controller (CheckoutController / RestoreController) over the shared Submitter
// adapter (ScfgSubmitter.h), show it through the sheet host and wait for its screen (IScfgSheetHost.h). Closing a
// payment sheet closes its controller first (cancel / keep_playing / finish), then the screen; "keep_playing" moves
// the controller to the background, where its page keeps answering until the session is final or the browser timeout
// passes.
//
// CALLBACKS. A Pay callback gets this call's own result when it is already_owned, failed or cancelled, and every later
// result of the payment; without one, a cancel and the later results reach OnPaymentResolved, also when the call's
// Done gets the same result. Done gets the call's result once, when the screen closes (or at once). Handlers must be
// idempotent.
//
// TEARDOWN (Shutdown, from UScfgSubsystem::Deinitialize, BeginDestroy, or the destructor; game thread only, checked)
// is, exactly: controllers; the worker, which owns the transport and destroys it last in its own destructor; the
// store; the pump. Step by step:
//   1. Every flow still waiting on a screen ends: its controller is closed (finish()), the host's Close() is called for
//      a screen that still exists, and Done gets the result; no callback, event or notification is delivered, and the
//      session is not marked cancelled (the Engine watches it again on the next launch). Then every open and
//      background controller is closed and released. Their commands still go to the live worker.
//   2. The worker is stopped (its running and queued commands are cancelled, and no callback comes any more), the
//      last snapshot is handed to the store, and the worker is destroyed. Its destructor destroys its Jobs first, then
//      the Engine, then shuts down and destroys its transport (the worker owns the transport, so the transport goes
//      here, inside step 2, after the Engine; the store never touches it). The Submitter adapter outlives it and
//      drops every later command (a controller a host still holds).
//   3. The store flushes what it still owes (scan progress inside the interval, a failed save) and is destroyed. That
//      flush may call its OnError: the handler holds only the shared reporter, never the facade, so it is safe there.
//   4. The pump leaves the ticker.
// A command the worker never answers would leave a checkout in "starting" or a restore in "checking": that is why
// every flow is closed in step 1, before the worker goes.
//
// DIFFERENCES FROM GODOT (scfg.gd), all deliberate:
// - No coroutines: Pay / Restore take a Done callback (Godot's awaited return value); the subsystem's PayAsync and the
//   async nodes are their faces.
// - A screen's Close(how) ends its flow at the next pump (Unity's rule), not inside the call: no re-entrancy.
// - The options are typed structs: an unknown option or a callback that is not callable cannot be written (Godot's
//   TypeError cases). The logo is a path relative to the project directory (Godot's res://).
// - Persistence is the store's (FScfgStore): its retry and deferred scan-progress write run from its own ticker
//   element (Godot's pump checks the interval), and its destructor writes what it owes (Godot's _exit_tree save).
// - At teardown the worker destroys a pass still in flight (Godot lets it finish after the node is freed and
//   detaches its handlers): nothing calls back afterwards.
// - No web build: no IndexedDB warning, no receipts mirror in localStorage, no page clipboard bridge.
//
// Game thread only: a public call from another thread is refused by ScfgGuard::OffGameThread (ScfgGuard.h: logged,
// counted, one ensure) with a Failed result or a neutral value, never a crash; this facade calls a Pay or Restore
// callback and Done back on the calling thread then. That guard is the tests' only: UScfgSubsystem checks the thread
// with the same guard before it calls the facade, and delivers on the game thread (ScfgSubsystem.h). Shutdown and the
// destructor stay checked: the engine runs them.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include "IScfgSheetHost.h"
#include "ScfgControllers.h"
#include "ScfgPlatform.h"
#include "ScfgSettings.h"
#include "ScfgStore.h"
#include "ScfgTransport.h"
#include "ScfgTypes.h"
#include "ScfgWorker.h"

#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/Payments.h"
#include "Scfg/Receipts.h"
#include "Scfg/Result.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class FScfgSubmitter;

// What the facade reports: logged to LogScfg, or (quiet, for the tests) recorded. Shared with the store's and the
// worker's handlers, so they never hold the facade (see TEARDOWN).
struct SCFG_API FScfgReporter
{
	bool bQuiet = false;
	// Quiet only: every error and warning line, newest last ("scfg: ...").
	TArray<FString> Errors;
	TArray<FString> Warnings;

	void Error(const FString& Line);
	void Warning(const FString& Line);
};

// The facade's defaults, and the test hooks that replace them (Godot's _setup). A game uses the defaults.
struct FScfgFacadeOptions
{
	// state.json's directory. Default: FPaths::ProjectSavedDir()/Scfg.
	FString StoreDirectory;
	// Seconds: the store's save throttle, the Engine's clock (whole seconds) and the browser timeout. Default: each its
	// own (FPlatformTime::Seconds() for the store, Unix time for the Engine).
	TFunction<double()> Clock;
	// The ticker of the pump, the worker and the store's retries. Default: FTSTicker::GetCoreTicker().
	FTSTicker* Ticker = nullptr;
	// The worker's transport. Default: FScfgHttpTransport.
	TFunction<TUniquePtr<FScfgTransport>()> MakeTransport;
	// The Engine's RPC objects (Engine.h). Default: each chain's over the transport.
	scfg::engine::RpcFactory RpcFactory;
	// Replaces PollInterval() as the worker's interval (the tests: a worker that never ticks by itself).
	TFunction<double()> WorkerInterval;
	// Record reports in the reporter instead of logging them.
	bool bQuiet = false;
	// Default: FScfgEnginePlatform (mobile on Android and iOS, or with -scfg-mobile outside Shipping).
	TSharedPtr<IScfgPlatform> Platform;
	// The checkout controllers' server_factory (the browser page). Default: the localhost checkout server
	// (FScfgLocalServer::Factory: 127.0.0.1 on a random port, the plugin's Resources/Web/ page, the configured logo);
	// a page that cannot start is reported once per attempt and shows "browser_unavailable".
	scfg::checkout::ServerFactory ServerFactory;
	// The directory a configured logo is relative to. Default: FPaths::ProjectDir().
	FString ProjectDir;
	// The checkout page's game name. Default: FApp::GetProjectName().
	FString GameName;
	// The pump runs from the ticker every frame. The tests turn it off and call Pump() themselves.
	bool bPumpOnTicker = true;
};

class SCFG_API FScfgFacade : public TSharedFromThis<FScfgFacade>
{
public:
	// The worker's poll interval in seconds, with a screen open and otherwise (scfg.gd's _POLL_OPEN_S, _POLL_IDLE_S).
	static constexpr double PollOpenSeconds = 3.0;
	static constexpr double PollIdleSeconds = 15.0;
	// The most characters a hash field takes (a pasted clipboard is cut there too).
	static constexpr int32 TxMaxLength = 90;

	using FPayDone = TFunction<void(const FScfgPaymentResult&)>;
	using FRestoreDone = TFunction<void(const FScfgRestoreResult&)>;

	explicit FScfgFacade(FScfgFacadeOptions InOptions = FScfgFacadeOptions());
	FScfgFacade(const FScfgFacade&) = delete;
	FScfgFacade& operator=(const FScfgFacade&) = delete;
	// Shutdown(), if it has not run.
	~FScfgFacade();

	// Puts the pump on the ticker (after construction: it binds a weak reference).
	void Start();
	// The teardown (see TEARDOWN). Idempotent; afterwards every call is a no-op or fails.
	void Shutdown();
	bool IsShuttingDown() const { return bShuttingDown; }

	// --- configuration (each returns the Err it reported) ---

	// An EVM chain that isn't built in; before Configure().
	TOptional<scfg::Err> AddChain(const FString& Key, const FScfgChain& Chain);
	// The payout wallet and options; again until payments have started (the pump after a successful one).
	TOptional<scfg::Err> Configure(const FScfgOptions& InOptions);
	// A paid unlock, at any time after Configure(): once the worker runs, the item is added to the Engine's config
	// too, in the worker's turn (Python's Engine shares the game's Config object), so it can be sold and owned. Until
	// that command ran (the next worker frame), Owns() of it reads the snapshot taken before.
	TOptional<scfg::Err> AddItem(const FString& ItemId, const FScfgItem& Item);
	// The project settings (ScfgSettings.h), on a new facade: AddChain() for each chain, Configure(), AddItem() for each
	// item, each reported as a misuse and then once more naming the settings. A failing chain or Configure() applies
	// nothing; a failing item keeps what came before. Afterwards the configuration is the settings' until a call from
	// code wins (ScfgSettings.h, "A CODE CALL AFTER IT WINS"): Configure() replaces it, the settings' chains included;
	// AddChain() sets it aside (with a warning) or, once payments started, is refused. Returns the first Err.
	TOptional<scfg::Err> ApplySettings(
		const FScfgOptions& InOptions, const TArray<FScfgSettingsChain>& Chains, const TArray<FScfgSettingsItem>& Items);
	// UScfgSubsystem::ConfigureWithSettings: these settings rows with options given by code (the settings' own with a
	// runtime wallet). Checked first, touching nothing: the chains are built, the configuration with them, its logo
	// file and every item row on a copy, and a failure (wallet.missing from a wallet file not read yet, a mistyped
	// wallet, a missing logo, an item the wallets cannot sell) is reported and leaves everything as it was, so the call
	// can be retried: it applies entirely or not at all. Then, as a code call, it replaces any configuration (the
	// settings' applied at start, or an earlier call's) with no AddChain() warning: the chains are exactly these rows,
	// then Configure(Options), then AddItem() for each item. Returns the first Err.
	TOptional<scfg::Err> ConfigureWithSettings(
		const FScfgOptions& InOptions, const TArray<FScfgSettingsChain>& Chains, const TArray<FScfgSettingsItem>& Items);
	// Line goes to the reporter as a warning at the end of the first frame (the facade's ticker), unless a
	// configuration exists by then: the settings' "no wallet" warning, which game code configuring at start makes moot.
	void WarnUnlessConfiguredAfterAFrame(FString Line);
	// The configuration is the settings' (ApplySettings), not one from code.
	bool IsConfigFromSettings() const { return Config.has_value() && bConfigFromSettings; }
	// A configured logo (relative to ProjectDir; empty: FPaths::ProjectDir()) as a full path that exists, "" for none,
	// or the ConfigError logo.file_missing. Configure() and UScfgSettings::Validate() both check it.
	static scfg::Result<FString> LogoFile(const FString& ProjectDir, const std::optional<std::string>& Logo);

	// --- flows ---

	// Show the payment sheet (see FLOWS and CALLBACKS). Done gets this call's result once.
	void Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, FScfgPayCallback Callback, FPayDone Done);
	// Show the restore screen. Callback gets the result once a restore finishes, while the screen is open; Done gets
	// the call's result when the screen closes: restored, nothing_found, cancelled (closed before a restore finished)
	// or failed.
	void Restore(FScfgRestoreCallback Callback, FRestoreDone Done);

	// --- queries ---

	// A verified receipt unlocks ItemId. Instant and offline. An unknown item is a developer error (reported; false).
	bool Owns(const FString& ItemId);
	bool HasTipped();
	// The sum of the tips, Python's str(Decimal) ("0", "2.50").
	FString TipTotal();
	// {receipt key: receipt}, a deep copy.
	scfg::Json Receipts();
	// {session id: record}: the payments still being watched for, a deep copy.
	scfg::Json Pending();
	// Payments run on every platform the plugin builds for (UScfgSubsystem::Supported() and the Blueprint node return
	// this).
	static constexpr bool Supported() { return true; }
	// Testing helper: forget every receipt and pending payment on this device (the payments are still on chain, and
	// Restore brings them back).
	void ClearLocalData();
	// Support helper: the receipts as readable JSON (a list of them, keys sorted, two spaces per level: Python's
	// json.dumps(values, indent=2, sort_keys=True)) at Path. Empty: Saved/scfg-receipts.json; a relative Path is under
	// Saved/ as well. Returns the file's absolute path, or the RuntimeError it reported ("cannot write <path>: <why>").
	scfg::Result<FString> ExportReceipts(const FString& Path = FString());

	// --- events, notifications, the host ---

	// Show a notice through the host when a payment confirms or purchases are restored.
	bool bNotify = false;
	// How the screens are shown: the game's host (SetSheetHost), else the default one (UScfgSubsystem sets the Slate
	// FScfgDefaultSheetHost); neither: Pay and Restore resolve Failed "sheet.unavailable". A screen is closed through
	// the host that opened it, even if the host changed meanwhile.
	TSharedPtr<IScfgSheetHost> SheetHost;
	TSharedPtr<IScfgSheetHost> DefaultSheetHost;
	// The host the next screen opens through.
	TSharedPtr<IScfgSheetHost> ActiveSheetHost() const { return SheetHost.IsValid() ? SheetHost : DefaultSheetHost; }
	// A final result of a payment that has no Pay callback (Godot's payment_resolved).
	TFunction<void(const FScfgPaymentResult&)> OnPaymentResolved;
	// The stored receipts changed (Godot's receipts_changed).
	TFunction<void()> OnReceiptsChanged;
	// How many times OnReceiptsChanged fired: a screen compares it instead of copying the receipts.
	int32 ReceiptsRevision() const { return ReceiptsRev; }

	// --- for the screens (FScfgCheckout / FScfgRestore forward their buttons here) ---

	void OpenBrowser(FScfgCheckout& Checkout);
	void OpenWalletApp(FScfgCheckout& Checkout);
	void OpenOnramp(FScfgCheckout& Checkout);
	void OpenExplorer(const FString& Url);
	void Copy(const FString& Text);
	void PasteInto(FScfgCheckout& Checkout);
	// Dropped once the screen moved past its input (Check was pressed).
	void PasteInto(FScfgRestore& Flow);
	// Ren'Py's _clean_paste: the first word, hash characters only, at most TxMaxLength (it stops there).
	static FString CleanPaste(const FString& Text);
	// The characters a hash field takes (hex or base58; Godot's TX_CHARS).
	static const FString& TxChars();
	// Godot's _report_error("screen:", err): an Err a screen's action got back (unreachable with the Engine's own
	// results), reported once like every other ("scfg: screen: ...").
	void ReportScreenError(const scfg::Err& Error) { ReportError(TEXT("screen:"), Error); }
	// A failing payment_uri, from the wallet-app button or a screen reading the link (the QR step reads it on every
	// rebuild): "scfg: cannot build the payment link: ..." once per controller, whoever asks first.
	void ReportPaymentLinkError(FScfgCheckout& Checkout, const scfg::Err& Error);
	// The declared title, else the id itself.
	FString ItemTitle(const FString& ItemId) const;
	// The chain's display name, else its key.
	FString ChainName(const FString& ChainKey) const;
	// configure's accent, else the default.
	FString Accent() const;
	bool IsMobile() const;

	// --- the pump ---

	void Pump();
	// The worker's interval: PollOpenSeconds while a sheet or the restore screen is open, else PollIdleSeconds.
	double PollInterval() const;

	// --- readers for the tests (not the API) ---

	FScfgReporter& GetReporter() const { return *Reporter; }
	FScfgWorker* GetWorker() const { return Worker.Get(); }
	FScfgStore* GetStore() const { return Store.Get(); }
	const FScfgSubmitter* GetSubmitter() const { return Submitter.get(); }
	const scfg::config::Config* GetConfig() const { return Config ? &*Config : nullptr; }
	const FString& GetLogoPath() const { return LogoPath; }
	int32 NumCheckouts() const { return Checkouts.Num(); }
	int32 NumBackground() const { return Background.Num(); }
	int32 NumRestores() const { return Restores.Num(); }
	int32 NumRestoreCallbacks() const;
	TArray<FString> CallbackSessionIds() const;
	TArray<FString> CheckoutIds() const;
	// Delivers a core result as the pump does (Godot's tests call _deliver).
	void DeliverForTests(const scfg::models::PaymentResult& Result) { Deliver(Result); }
	// Registers a Pay callback for a session (Godot's tests set _callbacks[sid]).
	void SetCallbackForTests(const FString& Sid, FScfgPayCallback Callback) { Callbacks.Add(Sid, MoveTemp(Callback)); }
	// Replace what the pump reads: the snapshot (Godot's tests freeze worker.snapshot) and the drained outcomes
	// (its RacingWorker). Empty: the worker's.
	TFunction<const scfg::engine::Snapshot*()> SnapshotForTests;
	TFunction<FScfgWorker::FOutcomes()> DrainForTests;

private:
	struct FWait
	{
		TWeakPtr<FScfgScreen> Screen;
		TSharedPtr<FScfgCheckout> Checkout;
		TSharedPtr<FScfgRestore> Restore;
		// The host that opened the screen (it closes it).
		TSharedPtr<IScfgSheetHost> Host;
		FPayDone PayDone;
		FRestoreDone RestoreDone;
	};

	FTSTicker& Ticker() const;
	FString NewId(const TCHAR* Prefix);

	// Reports
	TOptional<scfg::Err> Misuse(const scfg::Err& Error);
	TOptional<scfg::Err> ConfigMisuse(const char* Code, const std::string& Message);
	void ReportError(const FString& Heading, const scfg::Err& Error);
	void Warn(const FString& Message);
	scfg::Result<const scfg::config::Config*> RequireConfig(bool bReport);
	// AddChain() from code after the settings: their configuration, chains and logo go.
	void SetSettingsAside();

	// Store and worker
	FScfgStore& Stored();
	FScfgWorker* EnsureStarted();
	scfg::Err StartFailure() const;
	void OnWorkerStateChanged();
	int64 EngineNow() const;
	const scfg::engine::Snapshot* CurrentSnapshot() const;
	// The worker's snapshot when readers should see it (started, not wiping), else null.
	const scfg::engine::Snapshot* LiveSnapshot() const;
	const scfg::Json& SourcePending();
	const scfg::Json& SourceReceipts();
	// (owned item ids, tip total), or none when the stored receipts cannot be read (reported).
	TOptional<scfg::receipts::Ownership> Ownership();

	// Flows
	scfg::Result<scfg::payments::PaymentSpec> PaymentSpec(EScfgPayKind Kind, const FScfgPayRequest& Request);
	void FailedPayment(EScfgPayKind Kind, const FScfgPayRequest& Request, const FScfgPayCallback& Callback,
		const FPayDone& Done, const scfg::Err& Error, bool bReported);
	void FailedRestore(
		const FScfgRestoreCallback& Callback, const FRestoreDone& Done, const scfg::Err& Error, bool bReported);
	static scfg::Err NoScreen(const TCHAR* Method);
	void AddWait(FWait Wait);
	void ProcessWaits();
	void EndWait(FWait& Wait, const FString& How, bool bGone);
	void CloseCheckout(FWait& Wait, const FString& How, bool bGone);
	void CloseRestore(FWait& Wait, bool bGone);
	void CloseScreen(const FWait& Wait, bool bGone);
	bool IsOpen(const FScfgCheckout& Checkout) const;
	bool IsOpen(const FScfgRestore& Flow) const;
	static void ApplyPaste(std::string& Field, std::optional<std::string>& Error, const FString& Text);

	// The pump's parts
	void RegisterCallback(FScfgCheckout& Checkout);
	void PruneCallbacks(const TOptional<TSet<FString>>& PendingAtDrain);
	void CloseFinishedBackground();
	void PollRestores();
	void WarnUnready(const std::vector<std::pair<std::string, std::string>>& Unready);
	void Deliver(const scfg::models::PaymentResult& Result);
	void DeliverRestore(FScfgRestore& Flow);
	void Resolve(const FScfgPayCallback* Callback, const FScfgPaymentResult& Result);
	static void Call(const FScfgPayCallback& Callback, const FScfgPaymentResult& Result);
	static void Call(const FScfgRestoreCallback& Callback, const FScfgRestoreResult& Result);
	void Notify(const FString& Text);
	void EmitReceiptsChanged();

	FScfgFacadeOptions Options;
	TSharedRef<FScfgReporter> Reporter = MakeShared<FScfgReporter>();
	// True while the store's constructor runs (its OnError then reports a file that cannot be read).
	TSharedRef<bool> StoreLoading = MakeShared<bool>(false);
	FTSTicker::FDelegateHandle PumpHandle;
	FTSTicker::FDelegateHandle NoticeHandle;  // WarnUnlessConfiguredAfterAFrame's, removed at shutdown

	std::optional<scfg::config::Config> Config;
	std::vector<scfg::config::CustomChain> CustomChains;
	bool bConfigFromSettings = false;  // Config (and CustomChains) came from ApplySettings
	bool bApplyingSettings = false;    // inside ApplySettings
	FString LogoPath;  // the configured logo's full path, or ""
	// The same, shared with the default page server's factory (read when a page server is made).
	TSharedRef<FString> PageLogo = MakeShared<FString>();

	TUniquePtr<FScfgStore> Store;
	// What was last mirrored (Godot's _persisted): what readers see before the worker starts and while wiping.
	scfg::Json PersistedPending = scfg::Json::dict();
	scfg::Json PersistedReceipts = scfg::Json::dict();
	TUniquePtr<FScfgWorker> Worker;
	std::shared_ptr<FScfgSubmitter> Submitter;
	TOptional<scfg::Err> StartError;  // the Engine could not be built (or the store cannot be read): reported once

	TArray<TSharedRef<FScfgCheckout>> Checkouts;   // with their sheet open
	TArray<TSharedRef<FScfgCheckout>> Background;  // "keep playing": their browser page still answers
	TArray<TSharedRef<FScfgRestore>> Restores;     // with their screen open
	TMap<FString, FScfgPayCallback> Callbacks;     // session id -> the Pay callback, until the session is over
	TSet<FString> SeenPending;                     // the callbacks' sessions the last snapshot listed as pending
	TArray<FWait> Waits;
	int32 NextId = 0;
	std::optional<std::vector<std::pair<std::string, std::string>>> WarnedUnready;
	bool bReceiptsChangedPending = false;
	int32 ReceiptsRev = 0;
	bool bWiping = false;  // ClearLocalData()'s wipe is queued: the worker's snapshots are older than the wipe
	int32 WipeSerial = 0;
	bool bShuttingDown = false;
	bool bShutDown = false;
};
