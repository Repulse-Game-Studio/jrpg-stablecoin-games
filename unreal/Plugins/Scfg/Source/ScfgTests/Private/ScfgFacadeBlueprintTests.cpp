// No Python twin: the Unreal layer around the facade (Scfg.Glue.Facade.Blueprint.*): UScfgSubsystem's functions,
// dynamic delegates and events, the Pay and Restore async nodes (each output pin once), the Blueprint library, the
// value conversions, and the Submitter adapter's lifetime rules (ScfgSubmitter.h). Godot has no counterpart (its API
// is the autoload itself).
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "Engine/GameInstance.h"

#include "ScfgBlueprintLibrary.h"
#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgPayAsyncAction.h"
#include "ScfgString.h"
#include "ScfgSubmitter.h"
#include "ScfgSubsystem.h"
#include "ScfgTestReceiver.h"

#include "Scfg/Checkout.h"
#include "Scfg/Payments.h"

#include <memory>

#if WITH_DEV_AUTOMATION_TESTS

// UScfgSubsystem's facade with the tests' hooks (the subsystem's friend).
struct FScfgSubsystemTestAccess
{
	static void Start(UScfgSubsystem& Subsystem, const FScfgFacadeOptions& Options) { Subsystem.StartFacade(Options); }
};

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
		W.Facade = Scfg->GetFacade()->AsShared();
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
	// Without a game instance the library's nodes answer their neutral value and log once.
	AddExpectedMessagePlain(TEXT("no SCFG subsystem here"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Owns without a subsystem"), UScfgBlueprintLibrary::Owns(nullptr, TEXT("chapter2")));
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
	TestNull(TEXT("the facade is gone"), G.Scfg->GetFacade());
	TestEqual(TEXT("both screens were closed"), W.Host->Closes, 2);
	// After Deinitialize, a call fails (logged once) and never crashes.
	AddExpectedMessagePlain(TEXT("the SCFG subsystem is not running"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	FOnScfgPaid OnPaid;
	OnPaid.BindDynamic(G.Receiver.Get(), &UScfgTestReceiver::OnPaid);
	G.Scfg->Pay(EScfgPayKind::Unlock, Unlock(), OnPaid);
	TestTrue(TEXT("a Pay after Deinitialize fails"), G.Receiver->Paid.Num() == 1 &&
		G.Receiver->Paid[0].Status == EScfgPaymentStatus::Failed && G.Receiver->Paid[0].Error == TEXT("RuntimeError"));
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

#endif  // WITH_DEV_AUTOMATION_TESTS
