// Copyright 2026 Repulse
// Twin of godot/addons/scfg/scfg.gd's API (the Scfg autoload) and Unity's SCFG facade: the SCFG game-instance
// subsystem, one per game instance, created and torn down with it (it survives level loads).
//
//     UScfgSubsystem* Scfg = GetGameInstance()->GetSubsystem<UScfgSubsystem>();
//     if (!Scfg) { return; }                                      // a dedicated server has none (WHERE IT EXISTS)
//     Scfg->AddChain(TEXT("myl2"), Chain);                        // optional: your own EVM chain, before Configure
//     FScfgOptions Options;
//     Options.Wallet = TEXT("0x...");
//     Options.bTestnet = true;
//     Scfg->Configure(Options);
//     FScfgItem Item;
//     Item.Code = 1; Item.Price = TEXT("4.99"); Item.Token = TEXT("USDC"); Item.Title = TEXT("Chapter 2");
//     Scfg->AddItem(TEXT("chapter2"), Item);
//
//     if (Scfg->Owns(TEXT("chapter2"))) { ... }
//     FScfgPayRequest Request;
//     Request.Item = TEXT("chapter2");
//     Scfg->PayAsync(EScfgPayKind::Unlock, Request, [](const FScfgPaymentResult& R) { ... });   // shows the sheet
//
// Blueprint: the Pay and Restore async nodes (ScfgPayAsyncAction.h), the pure nodes of UScfgBlueprintLibrary, and the
// BlueprintCallable functions below.
//
// The behaviour is ScfgFacade.h's (the engine-object-free part this wraps): misuse never crashes (one "LogScfg: Error:
// scfg: ..." line and a Failed result with the code), AlreadyOwned comes back at once, keep playing resolves Pending,
// callbacks and the event fire once per payment (handlers must be idempotent), and a game-instance shutdown with a
// sheet open resolves the call without delivering anything: no callback (PayAsync's and RestoreAsync's OnResolved
// included, as the async nodes' pins), no event, no notification. The sheet is the plugin's own Slate sheet on the game
// viewport (FScfgDefaultSheetHost, ScfgDefaultSheetHost.h) unless SetSheetHost() gave another; a game instance
// without a viewport shows none, and Pay and Restore resolve Failed "sheet.unavailable".
//
// CONFIGURATION WITHOUT CODE. Initialize applies the project settings (UScfgSettings, Project Settings > Plugins >
// SCFG, saved to DefaultGame.ini) when they have a wallet, through the same AddChain / Configure / AddItem calls; a
// Configure() from code made after it replaces them (ScfgSettings.h, "A CODE CALL AFTER IT WINS"). Override them
// before the first frame (the game instance's Init() after Super::Init(), or the first level's BeginPlay): payments
// start on the first frame after a configuration, and a Configure() after that is configure.after_start.
//
// WHERE IT EXISTS. Every game instance gets one, except on a dedicated server (ShouldCreateSubsystem: a server process,
// or a play-in-editor server instance), which can never show a sheet: there GetSubsystem<UScfgSubsystem>() returns
// null, so code that also runs on a server checks it, as the sample above does (the Blueprint nodes then log once per
// node kind that SCFG does not run on a dedicated server, and answer their neutral value or resolve Failed). Every
// subsystem of a process reads and writes the one Saved/Scfg/state.json: several game instances in one process
// (play-in-editor with two or more clients) share it, each saving from its own snapshot, so a receipt one client
// stored can be dropped by the other's next save until a restart (which reads the file again) or a Restore brings it
// back. Test payments with one client.
//
// Deinitialize (or BeginDestroy, if Deinitialize never ran) tears down in ScfgFacade.h's TEARDOWN order, exactly:
// 1. the open sheets and controllers are closed; 2. the worker is stopped and destroyed (its destructor destroys its
// Jobs, then the Engine, then the transport it owns); 3. the store flushes what it owes and is destroyed (its OnError,
// which that flush may call, only logs); 4. the pump leaves the ticker.
//
// Game thread only. A call from another thread logs "scfg: <Function>() must be called on the game thread" (every
// function its own line; the ensure beside it reports once per process, with its callstack) and touches nothing:
// Pay, PayAsync, Restore and RestoreAsync deliver their Failed result ("RuntimeError") on the game thread, at the next
// tick of the core ticker, never on the calling thread; the queries return their neutral value.
//
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"

#include "IScfgSheetHost.h"
#include "ScfgTypes.h"

#include "ScfgSubsystem.generated.h"

class FScfgFacade;
class UScfgSettings;
struct FScfgFacadeOptions;

UCLASS()
class SCFG_API UScfgSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// False on a dedicated server (see WHERE IT EXISTS).
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// A subsystem destroyed without Deinitialize (its game instance collected without Shutdown) tears down here,
	// before garbage collection reaches the facade's destructor.
	virtual void BeginDestroy() override;

	// --- configuration: each returns "" or the ConfigError code (the Err kind for another error), logged once ---

	// Add an EVM chain that isn't built in (Optimism, Avalanche, your own L2). Call it before Configure(); the key can
	// then be used in Configure()'s Chains, RpcUrls and Confirmations.
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	FString AddChain(const FString& Key, const FScfgChain& Chain);
	// Set the payout wallet and options. Call it before the first frame that needs payments; it can be called again
	// until payments have started (the frame after a successful Configure()).
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	FString Configure(const FScfgOptions& Options);
	// Declare a paid unlock (after Configure()).
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	FString AddItem(const FString& ItemId, const FScfgItem& Item);
	// The project settings (Project Settings > Plugins > SCFG: their chains, options and items) with the payout wallets
	// given here instead of theirs: a wallet supplied at runtime (one per build, or read from a file). An empty wallet
	// is not set; at least one is required (wallet.missing). It checks the chain rows and the configuration first and
	// changes nothing when one fails, so a call that fails (a wallet file not read yet) can be retried. Then, as a code
	// call, it replaces any configuration (the settings' applied at start, or an earlier call's): the chains are the
	// settings' rows, then Configure(), then AddItem() for each item row (the first error code stops it). Call it when
	// Configure() may be called: before payments start (the game instance's Init() after Super::Init(), or the first
	// level's BeginPlay).
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	FString ConfigureWithSettings(const FString& Wallet, const FString& SolanaWallet);
	// ConfigureWithSettings() with these settings instead of the project's (C++ only).
	FString ApplySettingsWith(const UScfgSettings& Settings, const FString& Wallet, const FString& SolanaWallet);

	// --- payments ---

	// Show the payment sheet, with a callback: OnPaid gets this call's own result when it is AlreadyOwned, Failed or
	// Cancelled, and every later result of the payment (Paid, Underpaid, Expired), also one confirmed after the
	// player kept playing. OnPaymentResolved is not raised for it. Without OnPaid bound, a cancel and the later
	// results reach OnPaymentResolved. (The Pay async node is the form that resolves once, when the sheet closes.)
	UFUNCTION(BlueprintCallable, Category = "SCFG", meta = (AutoCreateRefTerm = "OnPaid"))
	void Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, const FOnScfgPaid& OnPaid);
	// The same with a C++ callback (null: none).
	void Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> Callback);
	// Show the payment sheet; OnResolved gets this call's result once, when the player closes the sheet (or at once:
	// AlreadyOwned, Failed). Without a callback, a cancel and the later results (a payment the player kept playing
	// through) also reach OnPaymentResolved: a handler of both must be idempotent. Unity's awaited Pay. A game-instance
	// shutdown with the sheet open drops OnResolved (it is not called), as it drops the Pay node's pin. The subsystem
	// stays stopped after its teardown (Deinitialize or BeginDestroy): a PayAsync made then logs "not running" once
	// per call site and drops OnResolved too, where Pay calls its callback with Failed "RuntimeError".
	void PayAsync(EScfgPayKind Kind, const FScfgPayRequest& Request,
		TFunction<void(const FScfgPaymentResult&)> OnResolved);
	// Show the restore screen (the player pastes a transaction hash); OnRestored gets the result once a restore
	// finishes, while the screen is still open.
	UFUNCTION(BlueprintCallable, Category = "SCFG", meta = (AutoCreateRefTerm = "OnRestored"))
	void Restore(const FOnScfgRestored& OnRestored);
	// Show the restore screen; OnResolved gets the call's result when the screen is closed: Restored, NothingFound,
	// Cancelled (closed before a restore finished) or Failed. Callback, if given, as Restore's OnRestored. A
	// game-instance shutdown with the screen open drops both (neither is called), and so does a RestoreAsync made after
	// the teardown (see PayAsync).
	void RestoreAsync(TFunction<void(const FScfgRestoreResult&)> OnResolved,
		TFunction<void(const FScfgRestoreResult&)> Callback = nullptr);

	// --- queries (the Blueprint library has them as pure nodes) ---

	// A verified receipt unlocks ItemId. Instant and offline. An unknown item is a developer error (logged; false).
	bool Owns(const FString& ItemId);
	bool HasTipped();
	// The sum of the player's tips as a decimal string ("0", "2.50").
	FString TipTotal();
	// The stored receipts, by receipt key.
	TMap<FString, FScfgReceipt> Receipts();
	// The payments still being watched for, by session id.
	TMap<FString, FScfgPendingPayment> Pending();
	// True where payments can run: every platform the plugin builds for.
	bool Supported() const;

	// Support helper: write the receipts as readable JSON (a list, sorted keys, indented) to Path (empty:
	// Saved/scfg-receipts.json). Returns the file's absolute path, or "" when it cannot be written (logged).
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	FString ExportReceipts(const FString& Path);
	// Testing helper: forget every receipt and pending payment on THIS device, to see your paywall again after a test
	// purchase. Real purchases are not lost: the payments are still on the blockchain and Restore brings them back.
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	void ClearLocalData();

	// Show a short notification (through the sheet host) when a payment confirms or purchases are restored.
	UFUNCTION(BlueprintCallable, Category = "SCFG")
	void SetNotify(bool bInNotify);
	UFUNCTION(BlueprintPure, Category = "SCFG")
	bool GetNotify() const;

	// How the payment sheet and the restore screen are shown (IScfgSheetHost.h); null: the default Slate sheet
	// (FScfgDefaultSheetHost). A screen already open is closed by the host that opened it.
	void SetSheetHost(TSharedPtr<IScfgSheetHost> Host);
	// The game's host, or null when the default one is used.
	TSharedPtr<IScfgSheetHost> GetSheetHost() const;

	// A final result of a payment that has no Pay callback (a cancel too).
	UPROPERTY(BlueprintAssignable, Category = "SCFG")
	FOnScfgPaymentResolved OnPaymentResolved;
	// The stored receipts changed (a payment confirmed, a restore, ClearLocalData()).
	UPROPERTY(BlueprintAssignable, Category = "SCFG")
	FOnScfgReceiptsChanged OnReceiptsChanged;

	// Deinitialize has begun: the flows resolve without delivering anything.
	bool IsShuttingDown() const;
	// How many calls, process-wide, were refused because they came from another thread (each logged once).
	static int32 NumOffThreadCalls();

private:
	friend struct FScfgSubsystemTestAccess;
	// The plugin's own access (Private/ScfgSubsystemAccess.h): the facade for the tests and the e2e driver, the
	// unguarded async starts for the nodes.
	friend struct FScfgSubsystemAccess;

	// PayAsync / RestoreAsync with OnResolved as given: the async nodes, which drop a teardown result themselves (and
	// are released by it).
	void StartPayAsync(EScfgPayKind Kind, const FScfgPayRequest& Request,
		TFunction<void(const FScfgPaymentResult&)> OnResolved);
	void StartRestoreAsync(TFunction<void(const FScfgRestoreResult&)> OnResolved,
		TFunction<void(const FScfgRestoreResult&)> Callback);

	// Builds the facade (Initialize: the defaults and the project settings; the tests: their hooks and settings, or
	// none) and binds the events, then applies Settings unless they are null or empty (no wallet: rows without one
	// get a warning).
	void StartFacade(const FScfgFacadeOptions& Options, const UScfgSettings* Settings);
	void StopFacade();
	// The facade, or null outside Initialize..Deinitialize: then one LogScfg error per call site (What) for this
	// subsystem, not one per call (a UMG binding would log every frame).
	FScfgFacade* Live(const TCHAR* What) const;

	TSharedPtr<FScfgFacade> Facade;
	bool bStopping = false;
	mutable TSet<FString> LoggedOutside;
};
