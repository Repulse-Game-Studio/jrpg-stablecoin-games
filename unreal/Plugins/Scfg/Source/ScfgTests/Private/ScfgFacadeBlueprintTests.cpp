// Copyright 2026 Repulse
// No Python twin: the Unreal layer around the facade (Scfg.Glue.Facade.Blueprint.*): UScfgSubsystem's functions,
// dynamic delegates and events, the Pay and Restore async nodes (each output pin once), the Blueprint library, the
// value conversions, and the Submitter adapter's lifetime rules (ScfgSubmitter.h). Godot has no counterpart (its API
// is the autoload itself).
#include "HAL/PlatformTime.h"
#include "Misc/AssertionMacros.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"

#include "ScfgBlueprintLibrary.h"
#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgPayAsyncAction.h"
#include "ScfgString.h"
#include "ScfgSubmitter.h"
#include "ScfgSubsystem.h"
#include "ScfgSubsystemAccess.h"
#include "ScfgSubsystemTestAccess.h"
#include "ScfgTestReceiver.h"

#include "Scfg/Checkout.h"
#include "Scfg/Payments.h"

#include <memory>
#include <thread>

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_FACADE_BP_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Facade.Blueprint." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgFacadeBlueprintTests
{

using namespace ScfgFacadeTests;

// A game instance (never initialised) holding a subsystem whose facade has the world's hooks; FWorld's helpers then
// drive that facade.
struct FGame
{
	TStrongObjectPtr<UGameInstance> Instance;
	TStrongObjectPtr<UScfgSubsystem> Scfg;
	TStrongObjectPtr<UScfgTestReceiver> Receiver;

	explicit FGame(FWorld& W, bool bQuiet = true, bool bPumpOnTicker = false)
	{
		Instance.Reset(NewObject<UGameInstance>());
		Scfg.Reset(NewObject<UScfgSubsystem>(Instance.Get()));
		Receiver.Reset(NewObject<UScfgTestReceiver>());
		FScfgFacadeOptions Options = W.Options();
		Options.bQuiet = bQuiet;
		Options.bPumpOnTicker = bPumpOnTicker;
		FScfgSubsystemTestAccess::Start(*Scfg, Options);
		W.Facade = FScfgSubsystemAccess::Facade(*Scfg)->AsShared();
		Scfg->SetSheetHost(W.Host);
	}
	FGame(const FGame&) = delete;
	FGame& operator=(const FGame&) = delete;
	~FGame()
	{
		Scfg->Deinitialize();
	}

	void Configure(FAutomationTestBase& Test)
	{
		FScfgOptions Options;
		Options.Wallet = UTF8_TO_TCHAR(kDev);
		Test.TestEqual(TEXT("configure"), Scfg->Configure(Options), FString());
		FScfgItem Item;
		Item.Code = 37;
		Item.Price = TEXT("4.99");
		Item.Token = TEXT("USDC");
		Item.Title = TEXT("Chapter 2");
		Test.TestEqual(TEXT("add_item"), Scfg->AddItem(TEXT("chapter2"), Item), FString());
	}
};

}  // namespace ScfgFacadeBlueprintTests

SCFG_FACADE_BP_TEST(FScfgBlueprintSubsystemTest, "TheSubsystemConfiguresQueriesAndRaisesItsEvents")

bool FScfgBlueprintSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	{
		FGame G(W, /*bQuiet=*/false, /*bPumpOnTicker=*/true);
		UScfgSubsystem& S = *G.Scfg;
		// A misuse is logged once, "LogScfg: Error: scfg: ...", and its code returned.
		AddExpectedMessagePlain(TEXT("scfg: wallet: "), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
		FScfgOptions Bad;
		Bad.Wallet = TEXT("0x123");
		TestEqual(TEXT("Configure returns the ConfigError code"), S.Configure(Bad), TEXT("address.invalid"));
		G.Configure(*this);
		TestTrue(TEXT("supported"), S.Supported() && UScfgBlueprintLibrary::Supported());
		S.OnReceiptsChanged.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnReceipts);
		S.OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
		W.Frame();  // the pump runs from the ticker: it starts the worker
		TestNotNull(TEXT("the ticker's pump started the worker"), W.F().GetWorker());
		W.RunOnce();
		(void)W.AddReceipt(kTx);
		W.Frame();
		TestEqual(TEXT("OnReceiptsChanged once"), G.Receiver->ReceiptsChanged, 1);
		TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
		const TMap<FString, FScfgReceipt> Receipts = S.Receipts();
		TestEqual(TEXT("one receipt"), Receipts.Num(), 1);
		for (const TPair<FString, FScfgReceipt>& Each : Receipts)
		{
			TestTrue(TEXT("its fields"), Each.Value.Chain == TEXT("arbitrum") &&
				Each.Value.TxHash == ScfgString::FromUtf8(kTx) && Each.Value.LogIndex == 3 && Each.Value.Block == 990 &&
				Each.Value.Kind == TEXT("unlock") && Each.Value.Item == TEXT("chapter2") &&
				Each.Value.Amount == TEXT("4.99") && Each.Value.Source == TEXT("payment"));
		}
		W.StartSession();
		const TMap<FString, FScfgPendingPayment> Pending = S.Pending();
		TestEqual(TEXT("one pending payment"), Pending.Num(), 1);
		for (const TPair<FString, FScfgPendingPayment>& Each : Pending)
		{
			TestTrue(TEXT("its fields"), Each.Key == Each.Value.Id && Each.Value.Kind == TEXT("unlock") &&
				Each.Value.Item == TEXT("chapter2") && Each.Value.Chain == TEXT("arbitrum") &&
				Each.Value.State == TEXT("awaiting") && Each.Value.UiStatus == TEXT("open") &&
				Each.Value.CreatedAt == 1000.0 && Each.Value.Cursor > 0);
		}
		TestEqual(TEXT("no tip"), S.TipTotal(), TEXT("0"));
		TestFalse(TEXT("HasTipped"), S.HasTipped());
		S.SetNotify(true);
		TestTrue(TEXT("Notify is a property of the facade"), S.GetNotify() && W.F().bNotify);
		const FString Exported = S.ExportReceipts(W.Dir / TEXT("receipts.json"));
		TestEqual(TEXT("ExportReceipts returns the path"), Exported, W.Dir / TEXT("receipts.json"));
		S.ClearLocalData();
		TestEqual(TEXT("ClearLocalData raises OnReceiptsChanged"), G.Receiver->ReceiptsChanged, 2);
		TestEqual(TEXT("and forgets them"), S.Receipts().Num(), 0);
	}
	// Without a game instance the library's nodes answer their neutral value and log once per node kind.
	UScfgBlueprintLibrary::ResetLogOnceForTests();
	AddExpectedMessagePlain(TEXT("scfg: Owns: no SCFG subsystem here"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("scfg: TipTotal: no SCFG subsystem here"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Owns without a subsystem"), UScfgBlueprintLibrary::Owns(nullptr, TEXT("chapter2")));
	TestFalse(TEXT("again (not logged again)"), UScfgBlueprintLibrary::Owns(nullptr, TEXT("chapter2")));
	TestEqual(TEXT("TipTotal without a subsystem"), UScfgBlueprintLibrary::TipTotal(nullptr), TEXT("0"));
	TestNull(TEXT("GetSubsystem without a world"), UScfgBlueprintLibrary::GetSubsystem(nullptr));
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintPayNodeTest, "ThePayNodeResolvesOnce")

bool FScfgBlueprintPayNodeTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	G.Scfg->OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
	W.Start();
	// A sheet the player cancels: OnResolved fires once, when the sheet closes.
	TStrongObjectPtr<UScfgPayAsyncAction> Node(UScfgPayAsyncAction::PayWith(G.Scfg.Get(), EScfgPayKind::Unlock, Unlock()));
	Node->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	Node->Activate();
	TestEqual(TEXT("the sheet is shown"), W.Host->Kinds.Num(), 1);
	TestEqual(TEXT("nothing yet"), G.Receiver->Paid.Num(), 0);
	W.Session(*this);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	W.Pump();
	TestEqual(TEXT("OnResolved once"), G.Receiver->Paid.Num(), 1);
	TestTrue(TEXT("cancelled"), G.Receiver->Paid.Num() == 1 && G.Receiver->Paid[0].Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("a node has no callback: the cancel reaches OnPaymentResolved too"), G.Receiver->Resolved.Num(), 1);
	// Owned already: OnResolved fires at once, inside Activate.
	(void)W.AddReceipt(kTx);
	TStrongObjectPtr<UScfgPayAsyncAction> Owned(UScfgPayAsyncAction::PayWith(G.Scfg.Get(), EScfgPayKind::Unlock, Unlock()));
	Owned->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	Owned->Activate();
	TestTrue(TEXT("already owned, at once"),
		G.Receiver->Paid.Num() == 2 && G.Receiver->Paid[1].Status == EScfgPaymentStatus::AlreadyOwned);
	// A node with no subsystem resolves Failed (logged once).
	AddExpectedMessagePlain(TEXT("no SCFG subsystem here"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	TStrongObjectPtr<UScfgPayAsyncAction> Lost(UScfgPayAsyncAction::PayWith(nullptr, EScfgPayKind::Unlock, Unlock()));
	Lost->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	Lost->Activate();
	TestTrue(TEXT("failed without a subsystem"), G.Receiver->Paid.Num() == 3 &&
		G.Receiver->Paid[2].Status == EScfgPaymentStatus::Failed && G.Receiver->Paid[2].Error == TEXT("RuntimeError"));
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintRestoreNodeTest, "TheRestoreNodeResolvesOnce")

bool FScfgBlueprintRestoreNodeTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	W.Start();
	TStrongObjectPtr<UScfgRestoreAsyncAction> Node(UScfgRestoreAsyncAction::RestoreWith(G.Scfg.Get()));
	Node->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnRestored);
	Node->Activate();
	TestEqual(TEXT("the restore screen is shown"), W.Host->Kinds, TArray<FString>{TEXT("restore")});
	W.Host->CloseSheet(-1, TEXT("close"));
	W.Pump();
	W.Pump();
	TestEqual(TEXT("OnResolved once"), G.Receiver->Restored.Num(), 1);
	TestTrue(TEXT("cancelled (closed before a restore finished)"),
		G.Receiver->Restored.Num() == 1 && G.Receiver->Restored[0].Status == EScfgRestoreStatus::Cancelled);
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintDynamicCallbackTest, "PayWithADynamicCallback")

bool FScfgBlueprintDynamicCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	G.Scfg->OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
	W.Start();
	FOnScfgPaid OnPaid;
	OnPaid.BindDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
	W.Session(*this);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("the callback gets the cancel"),
		G.Receiver->Paid.Num() == 1 && G.Receiver->Paid[0].Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("OnPaymentResolved is not raised for it"), G.Receiver->Resolved.Num(), 0);
	// An unbound delegate is no callback: the cancel reaches OnPaymentResolved.
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), FOnScfgPaid());
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestEqual(TEXT("without a bound callback, OnPaymentResolved"), G.Receiver->Resolved.Num(), 1);
	// The C++ callback form.
	TSharedRef<int32> Calls = MakeShared<int32>(0);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(TEXT("nope")), [Calls](const FScfgPaymentResult& R) { ++*Calls; });
	TestEqual(TEXT("a misused call reaches the C++ callback"), *Calls, 1);
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintShutdownTest, "DeinitializeReleasesTheNodesWithoutDelivering")

bool FScfgBlueprintShutdownTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	G.Scfg->OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
	W.Start();
	TStrongObjectPtr<UScfgPayAsyncAction> Pay(UScfgPayAsyncAction::PayWith(G.Scfg.Get(), EScfgPayKind::Unlock, Unlock()));
	Pay->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	Pay->Activate();
	W.Session(*this);
	TStrongObjectPtr<UScfgRestoreAsyncAction> Restore(UScfgRestoreAsyncAction::RestoreWith(G.Scfg.Get()));
	Restore->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnRestored);
	Restore->Activate();
	G.Scfg->Deinitialize();  // the game instance shuts down with both screens open
	W.Facade.Reset();
	W.Transport = nullptr;
	TestTrue(TEXT("nothing is delivered: no pin fires, no event"),
		G.Receiver->Paid.Num() == 0 && G.Receiver->Restored.Num() == 0 && G.Receiver->Resolved.Num() == 0);
	TestNull(TEXT("the facade is gone"), FScfgSubsystemAccess::Facade(*G.Scfg));
	TestEqual(TEXT("both screens were closed"), W.Host->Closes, 2);
	// After Deinitialize, a call fails and never crashes; each call site logs once for this subsystem.
	AddExpectedMessagePlain(TEXT("scfg: Pay(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("scfg: Restore(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	FOnScfgPaid OnPaid;
	OnPaid.BindDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
	TestTrue(TEXT("a Pay after Deinitialize fails, every time"), G.Receiver->Paid.Num() == 2 &&
		G.Receiver->Paid[0].Status == EScfgPaymentStatus::Failed && G.Receiver->Paid[0].Error == TEXT("RuntimeError"));
	FOnScfgRestored OnRestored;
	OnRestored.BindDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnRestored);
	G.Scfg->Restore(OnRestored);
	TestTrue(TEXT("so does a Restore: its callback gets Failed"), G.Receiver->Restored.Num() == 1 &&
		G.Receiver->Restored[0].Status == EScfgRestoreStatus::Failed &&
		G.Receiver->Restored[0].Error == TEXT("RuntimeError"));
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintStaleCallbackTest, "ADynamicCallbackWhoseObjectIsGoneFallsBackToTheEvent")

bool FScfgBlueprintStaleCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	G.Scfg->OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
	W.Start();
	UScfgTestReceiver* Gone = NewObject<UScfgTestReceiver>();
	FOnScfgPaid OnPaid;
	OnPaid.BindDynamic(Gone, &UScfgTestReceiver::OnPaid);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
	W.Session(*this);
	Gone->MarkAsGarbage();  // the widget that asked is destroyed while the sheet is open
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestEqual(TEXT("the stale callback is not called"), Gone->Paid.Num(), 0);
	TestTrue(TEXT("the cancel reaches OnPaymentResolved instead (Godot's invalid Callable)"),
		G.Receiver->Resolved.Num() == 1 && G.Receiver->Resolved[0].Status == EScfgPaymentStatus::Cancelled);
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintRestoreCallbackTest, "RestoreWithADynamicCallback")

bool FScfgBlueprintRestoreCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	W.Start();
	const std::string Tx = Hash("cd");
	W.Chains->Chain("arbitrum").TxReceipts[Tx] = Json::dict({{"status", "0x1"},
		{"logs", Json::list({TransferLog(UsdcArbitrum(), kPayer, kDev, 4993742, 10, Tx)})}});
	FOnScfgRestored OnRestored;
	OnRestored.BindDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnRestored);
	G.Scfg->Restore(OnRestored);
	TestTrue(TEXT("a transaction hash"), W.Host->RestoreOf(-1).Core().check_tx(Tx).ok());
	W.Pass();
	W.Pass();
	TestTrue(TEXT("the callback gets the restore, the screen still open"), G.Receiver->Restored.Num() == 1 &&
		G.Receiver->Restored[0].Status == EScfgRestoreStatus::Restored && G.Receiver->Restored[0].Restored.Num() == 1);
	W.Host->CloseSheet(-1, TEXT("close"));
	W.Pump();
	TestEqual(TEXT("once"), G.Receiver->Restored.Num(), 1);
	TestTrue(TEXT("owned"), G.Scfg->Owns(TEXT("chapter2")));
	return true;
}

// The Blueprint entry points: a real game instance (InitializeStandalone: its world, its subsystem collection, so
// UScfgSubsystem::Initialize runs), the nodes found from a world context, kept alive by RegisterWithGameInstance
// while they wait, and released by SetReadyToDestroy.
SCFG_FACADE_BP_TEST(FScfgBlueprintWorldContextTest, "TheNodesRunFromAWorldContextAndAreReleased")

bool FScfgBlueprintWorldContextTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	const FScfgBlankProjectSettings BlankSettings;  // the instance's own facade is not configured by the project's
	FWorld W;
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
	Instance->InitializeStandalone();
	UWorld* World = Instance->GetWorld();
	UScfgSubsystem* Scfg = Instance->GetSubsystem<UScfgSubsystem>();
	TestNotNull(TEXT("the collection made the subsystem"), Scfg);
	TestTrue(TEXT("its Initialize started a facade"), Scfg && FScfgSubsystemAccess::Facade(*Scfg) != nullptr);
	TStrongObjectPtr<UScfgTestReceiver> Receiver(NewObject<UScfgTestReceiver>());
	if (Scfg && World)
	{
		TestTrue(TEXT("the library finds it from the world"), UScfgBlueprintLibrary::GetSubsystem(World) == Scfg);
		FScfgSubsystemTestAccess::Start(*Scfg, W.Options());  // the fakes from here on
		W.Facade = FScfgSubsystemAccess::Facade(*Scfg)->AsShared();
		Scfg->SetSheetHost(W.Host);
		FScfgOptions Options;
		Options.Wallet = UTF8_TO_TCHAR(kDev);
		TestEqual(TEXT("configure"), Scfg->Configure(Options), FString());
		W.Start();
		TWeakObjectPtr<UScfgPayAsyncAction> Pay(UScfgPayAsyncAction::Pay(World, EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC"))));
		TWeakObjectPtr<UScfgRestoreAsyncAction> Restore(UScfgRestoreAsyncAction::Restore(World));
		Pay->OnResolved.AddDynamic(Receiver.Get(), &UScfgTestReceiver::OnPaid);
		Restore->OnResolved.AddDynamic(Receiver.Get(), &UScfgTestReceiver::OnRestored);
		Pay->Activate();
		Restore->Activate();
		TestEqual(TEXT("both screens open"), W.Host->Kinds.Num(), 2);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("a waiting node is kept alive by its game instance"), Pay.IsValid() && Restore.IsValid());
		W.Host->CloseSheet(0, TEXT("cancel"));
		W.Host->CloseSheet(1, TEXT("close"));
		W.Pump();
		TestTrue(TEXT("each pin once"), Receiver->Paid.Num() == 1 && Receiver->Restored.Num() == 1);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("a resolved node is released"), !Pay.IsValid() && !Restore.IsValid());
	}
	W.Facade.Reset();
	W.Transport = nullptr;
	Instance->Shutdown();  // Deinitialize
	TestTrue(TEXT("Shutdown deinitialized it"), !Scfg || FScfgSubsystemAccess::Facade(*Scfg) == nullptr);
	if (World)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintSubmitterTest, "TheSubmitterDropsCommandsOnceTheWorkerIsGone")

bool FScfgBlueprintSubmitterTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	W.Pay(EScfgPayKind::Unlock, Unlock());
	const std::shared_ptr<scfg::worker::Submitter> Submit = W.Host->CheckoutOf(-1).Core().submit();
	FScfgSubmitter* Adapter = static_cast<FScfgSubmitter*>(Submit.get());
	TestTrue(TEXT("the controllers share the facade's adapter"), Adapter == S.GetSubmitter());
	// While the worker lives: the command goes to it, and its done comes from the ticker, never inside the call.
	TSharedRef<int32> Done = MakeShared<int32>(0);
	Submit->set_ui_status("ps_none", "open", [Done](scfg::Status) { ++*Done; });
	TestEqual(TEXT("not inside the call"), *Done, 0);
	W.RunOnce();
	TestEqual(TEXT("from the worker's ticker"), *Done, 1);
	// A controller made over the adapter whose session start is delivered but never polled: destroyed after the
	// worker, its destructor's close() submits the discard to a worker that is gone.
	scfg::checkout::CheckoutOptions Options;
	Options.config = *S.GetConfig();
	Options.spec = scfg::payments::prepare_payment(*S.GetConfig(), "unlock", Json::dict({{"item", "chapter2"}})).value();
	Options.submit = Submit;
	scfg::Result<std::unique_ptr<scfg::checkout::CheckoutController>> Made =
		scfg::checkout::CheckoutController::make(std::move(Options));
	TestTrue(TEXT("a controller"), Made.ok());
	std::unique_ptr<scfg::checkout::CheckoutController> Orphan = Made.ok() ? Made.take() : nullptr;
	if (Orphan)
	{
		TestTrue(TEXT("choose a network"), Orphan->choose_network("arbitrum").ok());
		W.RunOnce();  // the start is delivered to the controller, which nobody polls
	}
	W.Drop();
	TestNull(TEXT("the worker is gone"), Adapter->Live());
	const int32 Before = Adapter->NumDropped();
	Orphan.reset();  // close(): the abandoned start's discard, submitted from the destructor
	TestEqual(TEXT("the discard from the destructor is dropped, no crash"), Adapter->NumDropped(), Before + 1);
	TSharedRef<int32> Late = MakeShared<int32>(0);
	Submit->start_session("unlock", "arbitrum", Json::dict({{"item", "chapter2"}}),
		[Late](scfg::Result<Json>) { ++*Late; });
	Submit->discard_session("ps_none", nullptr);
	TestEqual(TEXT("later commands are dropped too"), Adapter->NumDropped(), Before + 3);
	for (int32 I = 0; I < 3; ++I)
	{
		W.Ticker.Tick(0.0f);
	}
	TestEqual(TEXT("and their done is never called"), *Late, 0);
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintBeginDestroyTest, "BeginDestroyWithoutDeinitializeReleasesTheNodes")

// A subsystem destroyed without Deinitialize (its game instance collected, ConditionalBeginDestroy): BeginDestroy runs
// the teardown, with both nodes waiting: no pin fires, no event, both screens are closed, nothing crashes.
bool FScfgBlueprintBeginDestroyTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	FWorld W;
	FGame G(W);
	G.Configure(*this);
	G.Scfg->OnPaymentResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnResolved);
	W.Start();
	TStrongObjectPtr<UScfgPayAsyncAction> Pay(UScfgPayAsyncAction::PayWith(G.Scfg.Get(), EScfgPayKind::Unlock, Unlock()));
	Pay->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	Pay->Activate();
	W.Session(*this);
	TStrongObjectPtr<UScfgRestoreAsyncAction> Restore(UScfgRestoreAsyncAction::RestoreWith(G.Scfg.Get()));
	Restore->OnResolved.AddDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnRestored);
	Restore->Activate();
	G.Scfg->ConditionalBeginDestroy();  // no Deinitialize
	W.Facade.Reset();
	W.Transport = nullptr;
	TestTrue(TEXT("BeginDestroy ran the teardown"), G.Scfg->HasAnyFlags(RF_BeginDestroyed) && !FScfgSubsystemAccess::Facade(*G.Scfg));
	TestTrue(TEXT("nothing is delivered: no pin fires, no event"),
		G.Receiver->Paid.Num() == 0 && G.Receiver->Restored.Num() == 0 && G.Receiver->Resolved.Num() == 0);
	TestEqual(TEXT("both screens were closed"), W.Host->Closes, 2);
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintCppTeardownTest, "TheCppAsyncCallbacksAreDroppedAtTeardown")

// The C++ PayAsync / RestoreAsync with their screens open when the game instance goes (final review F1), once by
// Deinitialize and once by BeginDestroy without it: the game's OnResolved (and RestoreAsync's Callback) are dropped as
// the nodes' pins are, so game code that would make a UObject there (a widget for the result) never runs, and the
// teardown itself creates none. Before the teardown the same callbacks are called. After it the subsystem stays
// stopped (final re-review N3): a PayAsync or RestoreAsync made then drops its OnResolved as well, where Pay's and
// Restore's callbacks get Failed; each call site logs "not running" once per subsystem (one per loop below).
bool FScfgBlueprintCppTeardownTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	AddExpectedMessagePlain(TEXT("scfg: PayAsync(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("scfg: RestoreAsync(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("scfg: Pay(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("scfg: Restore(): the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 2);
	for (const bool bBeginDestroy : {false, true})
	{
		const FString How = bBeginDestroy ? TEXT("BeginDestroy: ") : TEXT("Deinitialize: ");
		FWorld W;
		FGame G(W);
		G.Configure(*this);
		W.Start();
		struct FCalls
		{
			int32 Paid = 0;
			int32 Restored = 0;
			TArray<TStrongObjectPtr<UScfgTestReceiver>> Made;  // what the game's handlers made
		};
		const TSharedRef<FCalls> Calls = MakeShared<FCalls>();
		auto OnPaid = [Calls](const FScfgPaymentResult&)
		{
			++Calls->Paid;
			Calls->Made.Emplace(NewObject<UScfgTestReceiver>());
		};
		auto OnRestored = [Calls](const FScfgRestoreResult&)
		{
			++Calls->Restored;
			Calls->Made.Emplace(NewObject<UScfgTestReceiver>());
		};
		// While the game instance runs, a closed sheet reaches the callback.
		G.Scfg->PayAsync(EScfgPayKind::Unlock, Unlock(), OnPaid);
		W.Host->CloseSheet(-1, TEXT("cancel"));
		W.Pump();
		TestEqual(How + TEXT("before the teardown the callback is called"), Calls->Paid, 1);
		// Both screens open, then the teardown.
		G.Scfg->PayAsync(EScfgPayKind::Unlock, Unlock(), OnPaid);
		W.Session(*this);
		G.Scfg->RestoreAsync(OnRestored, OnRestored);
		const int32 ClosesBefore = W.Host->Closes;
		const int32 ObjectsBefore = GUObjectArray.GetObjectArrayNumMinusAvailable();
		if (bBeginDestroy)
		{
			G.Scfg->ConditionalBeginDestroy();  // no Deinitialize
		}
		else
		{
			G.Scfg->Deinitialize();
		}
		const int32 ObjectsAfter = GUObjectArray.GetObjectArrayNumMinusAvailable();
		W.Facade.Reset();
		W.Transport = nullptr;
		TestTrue(How + TEXT("the facade is gone"), !FScfgSubsystemAccess::Facade(*G.Scfg));
		TestEqual(How + TEXT("both screens were closed"), W.Host->Closes - ClosesBefore, 2);
		TestTrue(How + TEXT("nothing is delivered: no OnResolved, no restore Callback"),
			Calls->Paid == 1 && Calls->Restored == 0);
		TestEqual(How + TEXT("the teardown created no UObject"), ObjectsAfter, ObjectsBefore);
		G.Scfg->PayAsync(EScfgPayKind::Unlock, Unlock(), OnPaid);
		G.Scfg->RestoreAsync(OnRestored, OnRestored);
		TestTrue(How + TEXT("a PayAsync or RestoreAsync after the teardown drops its OnResolved too"),
			Calls->Paid == 1 && Calls->Restored == 0);
		TOptional<FScfgPaymentResult> Failed;
		G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), [&Failed](const FScfgPaymentResult& Result) { Failed = Result; });
		TestTrue(How + TEXT("where Pay's callback gets Failed RuntimeError"), Failed.IsSet() &&
			Failed->Status == EScfgPaymentStatus::Failed && Failed->Error == TEXT("RuntimeError"));
		// Restore too (phase 3 review P3-6): its bound callback gets Failed RuntimeError.
		FOnScfgRestored Restored;
		Restored.BindUFunction(G.Receiver.Get(), GET_FUNCTION_NAME_CHECKED(UScfgTestReceiver, OnRestored));
		G.Scfg->Restore(Restored);
		TestTrue(How + TEXT("and Restore's callback gets Failed RuntimeError"), G.Receiver->Restored.Num() == 1 &&
			G.Receiver->Restored[0].Status == EScfgRestoreStatus::Failed && G.Receiver->Restored[0].Error == TEXT("RuntimeError"));
	}
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintDedicatedServerTest, "NoSubsystemOnADedicatedServer")

// ShouldCreateSubsystem (final review F8): a game instance gets one, a dedicated server instance (here a
// play-in-editor one, its world context's RunAsDedicated) does not.
bool FScfgBlueprintDedicatedServerTest::RunTest(const FString& Parameters)
{
	const FScfgBlankProjectSettings BlankSettings;  // the instance's own facade is not configured by the project's
	const UScfgSubsystem* Default = GetDefault<UScfgSubsystem>();
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
	TestTrue(TEXT("a game instance gets one"), Default->ShouldCreateSubsystem(Instance.Get()));
	Instance->InitializeStandalone();
	UWorld* World = Instance->GetWorld();
	TestTrue(TEXT("a standalone game too"), Default->ShouldCreateSubsystem(Instance.Get()));
	if (FWorldContext* Context = Instance->GetWorldContext())
	{
		Context->RunAsDedicated = true;
		TestFalse(TEXT("a dedicated server instance gets none"), Default->ShouldCreateSubsystem(Instance.Get()));
		Context->RunAsDedicated = false;
	}
	else
	{
		AddError(TEXT("InitializeStandalone made no world context"));
	}
	Instance->Shutdown();
	// Carried N2 of phase 2: the nodes say why there is no subsystem on a server. (After Shutdown the instance has no
	// subsystem; the engine still has the world's context, here a dedicated server's.)
	if (FWorldContext* Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr)
	{
		UScfgBlueprintLibrary::ResetLogOnceForTests();
		TestFalse(TEXT("a client is not a server"), UScfgBlueprintLibrary::OnDedicatedServer(World));
		Context->RunAsDedicated = true;
		TestTrue(TEXT("a dedicated server's world"), UScfgBlueprintLibrary::OnDedicatedServer(World));
		TestNull(TEXT("no subsystem there"), UScfgBlueprintLibrary::GetSubsystem(World));
		AddExpectedMessagePlain(TEXT("scfg: Owns: no SCFG subsystem on a dedicated server (SCFG runs on the players' game "
			"clients only)"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
		TestFalse(TEXT("Owns on a server"), UScfgBlueprintLibrary::Owns(World, TEXT("chapter2")));
		AddExpectedMessagePlain(TEXT("scfg: no SCFG subsystem on a dedicated server (SCFG runs on the players' game "
			"clients only)"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
		TStrongObjectPtr<UScfgPayAsyncAction> Node(
			UScfgPayAsyncAction::Pay(World, EScfgPayKind::Unlock, ScfgFacadeTests::Unlock()));
		Node->Activate();
		Context->RunAsDedicated = false;
		UScfgBlueprintLibrary::ResetLogOnceForTests();
	}
	if (World)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	return true;
}

SCFG_FACADE_BP_TEST(FScfgBlueprintOffThreadTest, "ACallFromAnotherThreadFailsOnTheGameThread")

// The subsystem called from another thread: every function logs its own line (its ensure is swallowed by this
// thread's ensure handler, so the test sees it without a crash report) and touches nothing; Pay, PayAsync and Restore
// get their Failed result later, on the game thread.
bool FScfgBlueprintOffThreadTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeBlueprintTests;
	struct FState
	{
		FWorld W;
		TUniquePtr<FGame> G;
		TArray<bool> AsyncOnGameThread;
		TArray<FScfgPaymentResult> Async;
		bool bOwns = true;
		int32 RefusedBefore = 0;
		double Started = 0;
	};
	const TSharedRef<FState> S = MakeShared<FState>();
	S->G = MakeUnique<FGame>(S->W);
	S->G->Configure(*this);
	S->W.Start();
	UScfgSubsystem* Scfg = S->G->Scfg.Get();
	const TSharedPtr<IScfgSheetHost> Host = Scfg->GetSheetHost();
	for (const TCHAR* What : {TEXT("Pay()"), TEXT("PayAsync()"), TEXT("Restore()"), TEXT("SetNotify()"), TEXT("Owns()"),
			 TEXT("SetSheetHost()")})
	{
		AddExpectedMessagePlain(FString::Printf(TEXT("scfg: %s must be called on the game thread"), What),
			ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	}
	FOnScfgPaid OnPaid;
	OnPaid.BindDynamic(S->G->Receiver.Get(), &UScfgTestReceiver::OnPaid);
	FOnScfgRestored OnRestored;
	OnRestored.BindDynamic(S->G->Receiver.Get(), &UScfgTestReceiver::OnRestored);
	S->RefusedBefore = UScfgSubsystem::NumOffThreadCalls();
	std::thread Other([S, Scfg, OnPaid, OnRestored]()
	{
		// thread_local: an ensure of this thread is skipped (no callstack, no crash report)
		const TFunction<bool(const FEnsureHandlerArgs&)> Previous = SetEnsureHandler([](const FEnsureHandlerArgs&)
		{
			return true;
		});
		Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
		Scfg->PayAsync(EScfgPayKind::Unlock, Unlock(), [S](const FScfgPaymentResult& R)
		{
			S->AsyncOnGameThread.Add(IsInGameThread());
			S->Async.Add(R);
		});
		Scfg->Restore(OnRestored);
		Scfg->SetNotify(true);
		S->bOwns = Scfg->Owns(TEXT("chapter2"));
		Scfg->SetSheetHost(nullptr);
		SetEnsureHandler(Previous);
	});
	Other.join();
	TestTrue(TEXT("nothing is delivered on the calling thread"),
		S->Async.Num() == 0 && S->G->Receiver->Paid.Num() == 0 && S->G->Receiver->Restored.Num() == 0);
	TestFalse(TEXT("a query answers its neutral value"), S->bOwns);
	TestTrue(TEXT("nothing of the subsystem changed"), !Scfg->GetNotify() && Scfg->GetSheetHost() == Host &&
		S->W.F().NumCheckouts() == 0 && S->W.F().NumRestores() == 0);
	// (the ensure itself fires once per process, so a second run of this test in the same editor would not see it)
	TestEqual(TEXT("each of the six calls was refused"), UScfgSubsystem::NumOffThreadCalls() - S->RefusedBefore, 6);
	S->Started = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, S]()
	{
		UScfgTestReceiver& R = *S->G->Receiver;
		const bool bAll = S->Async.Num() == 1 && R.Paid.Num() == 1 && R.Restored.Num() == 1;
		if (!bAll && FPlatformTime::Seconds() - S->Started < 5.0)
		{
			return false;  // the core ticker delivers them on a later frame
		}
		TestTrue(TEXT("PayAsync got Failed RuntimeError, on the game thread"), S->Async.Num() == 1 &&
			S->Async[0].Status == EScfgPaymentStatus::Failed && S->Async[0].Error == TEXT("RuntimeError") &&
			S->AsyncOnGameThread[0]);
		TestTrue(TEXT("so did Pay's dynamic callback"), R.Paid.Num() == 1 &&
			R.Paid[0].Status == EScfgPaymentStatus::Failed && R.Paid[0].Error == TEXT("RuntimeError"));
		TestTrue(TEXT("and Restore's"), R.Restored.Num() == 1 && R.Restored[0].Status == EScfgRestoreStatus::Failed);
		S->G.Reset();
		return true;
	}));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
