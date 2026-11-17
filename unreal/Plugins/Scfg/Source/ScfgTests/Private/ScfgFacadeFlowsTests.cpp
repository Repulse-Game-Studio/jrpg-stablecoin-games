// Twin of godot/tests/test_scfg_flows.gd: Pay and Restore through the sheet host, background "keep playing" payments,
// callbacks versus OnPaymentResolved, notifications, wallet-app links and the clipboard, test for test
// (Scfg.Glue.Facade.Flows.<the Godot name>), through FScfgFacade with the headless host, the fake platform and the fake
// nodes (ScfgFacadeTestKit.h). A sheet closed with Close(how) ends its flow at the next pump (Godot: inside the
// signal), so each close is followed by a Pump().
//
// Mapping (Godot test -> this file):
//   test_pay_already_owned_short_circuits                    -> same name
//   test_pay_cancel_returns_cancelled_and_calls_callback     -> same name
//   test_keep_playing_delivers_later_result_via_signal       -> same name
//   test_paid_tip_notifies_without_a_title_and_respects_notify_off -> same name
//   test_closing_the_sheet_releases_the_page_first           -> same name, with a recording page; its HTTP requests
//       to the page (200 for the page, 404 for /build, the status route) are in TheBrowserMethodSendsAHintToTheEngine,
//       over the real localhost server
//   test_no_sheet_ui_fails_clearly                           -> same name (the bare IScfgSheetHost)
//   test_misused_pay_resolves_failed_with_code               -> same name. Godot only: the unknown option ("colour")
//       and the callback that is not a Callable, which typed requests and callbacks cannot express; "gift" is an
//       EScfgPayKind value outside the enum
//   test_interval_is_3s_while_a_sheet_is_open                -> same name: the worker's own interval (GetInterval()),
//       with the facade's wiring kept (no ManualTimer stand-in)
//   test_a_sheet_freed_without_closing_counts_as_cancel      -> same name (the host releases the screen)
//   test_a_sheet_never_in_the_tree_or_reparented             -> Godot only: the scene tree (a screen outside it, a
//       reparented one); a screen released without Close is the test above
//   test_quitting_with_the_sheet_open_closes_without_delivery -> same name (Shutdown)
//   test_awaiters_are_released_when_the_autoload_is_freed    -> same name (the facade destroyed, never pumped)
//   test_pay_before_configure_reports_the_require_config_error -> same name
//   test_restore_flow_restores_and_notifies                  -> same name
//   test_two_pay_calls_at_once_resolve_independently         -> same name
//   test_restore_during_pay                                  -> same name
//   test_mobile_offers_wallet_app_link                       -> same name
//   test_copy_and_paste_use_the_clipboard                    -> same name (a restore moved to "checking" by check_tx:
//       its state is read-only here)
//   test_clean_paste_matches_renpy_and_stops_at_90           -> same name (no null case: an FString is never null)
//   test_a_delivered_paid_result_drops_the_callback          -> same name
//   test_an_expired_session_keeps_its_callback_until_it_is_paid -> same name
//   test_an_open_sheet_past_the_scan_horizon_does_not_register_its_callback_again -> same name
//   test_a_discarded_session_drops_its_callback              -> same name
//   test_a_session_paid_before_any_snapshot_listed_it_releases_its_callback -> same name (SnapshotForTests freezes
//       what the pump reads, as Godot's test sets worker.snapshot)
//   test_the_snapshot_is_read_before_the_outcomes_are_drained -> same name (DrainForTests plays RacingWorker)
// C++ only:
//   TheBrowserMethodSendsAHintToTheEngine                    -> the default page server (the localhost checkout
//       server, task 6) on a real socket: the page served, no /build on EVM, the page's POST /tx reaching the Engine as
//       the session's hint, the page stopped with the sheet
//   TheDesktopPathWorksWhenThePageCannotStart                -> a page that cannot listen: reported by the page, the
//       browser method shows "browser_unavailable" and drops out; the QR link and the manual hash work
//   TheBrowserFallsBackWhenThePageFilesAreMissing            -> no Resources/Web/ files: the page server is not made,
//       browser_unavailable, QR and manual remain
//   AnItemAddedAfterTheStartCanBeSoldAndOwned                -> AddItem after the worker started reaches the Engine
//       (Python's Engine shares the game's Config): paid on chain, then owned
//   ClearLocalDataWithAPaymentSheetOpen                      -> the wipe while a sheet watches its session
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#include "ScfgConvert.h"
#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgLocalServer.h"
#include "ScfgStore.h"
#include "ScfgString.h"
#include "ScfgTestHttpClient.h"
#include "ScfgWorker.h"

#include "Scfg/Engine.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/Receipts.h"
#include "Scfg/Watcher.h"

#include <string>

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_FACADE_FLOWS_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Facade.Flows." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgFacadeFlowsTests
{

using namespace ScfgFacadeTests;

using FResults = TArray<FScfgPaymentResult>;

TSharedRef<FResults> NewResults()
{
	return MakeShared<FResults>();
}

FString Statuses(const FResults& Results)
{
	TArray<FString> Out;
	for (const FScfgPaymentResult& Each : Results)
	{
		Out.Add(UEnum::GetValueAsString(Each.Status).RightChop(FString(TEXT("EScfgPaymentStatus::")).Len()));
	}
	return FString::Join(Out, TEXT(","));
}

bool Is(const TOptional<FScfgPaymentResult>& R, EScfgPaymentStatus Status)
{
	return R.IsSet() && R->Status == Status;
}

Json PendingRecord(FScfgFacade& S, const FString& Sid)
{
	const Json All = S.Pending();
	return All.as_dict().get(ScfgString::ToUtf8(Sid));
}

FString UiStatus(FScfgFacade& S, const FString& Sid)
{
	const Json R = PendingRecord(S, Sid);
	return R.is_dict() ? ScfgConvert::Str(R.as_dict().get("ui_status")) : FString();
}

// Shown on Arbitrum: one Transfer to the developer of a restore candidate, in an old block.
void PutOldPayment(FFakeChain& Chain, const std::string& Tx)
{
	Chain.TxReceipts[Tx] = Json::dict({{"status", "0x1"},
		{"logs", Json::list({TransferLog(UsdcArbitrum(), kPayer, kDev, 4993742, 10, Tx)})}});
}

}  // namespace ScfgFacadeFlowsTests

// --- pay ------------------------------------------------------------------------------------------------------------

SCFG_FACADE_FLOWS_TEST(FScfgFlowsAlreadyOwnedTest, "test_pay_already_owned_short_circuits")

bool FScfgFlowsAlreadyOwnedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	W.Pass();
	const scfg::Result<Json> Added = W.AddReceipt(kTx);
	TestTrue(TEXT("a receipt for chapter2"), Added.ok() && Added.value().as_bool());
	const TSharedRef<FResults> Called = NewResults();
	const TSharedRef<TOptional<FScfgPaymentResult>> R = W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	TestTrue(TEXT("already_owned, at once"), Is(*R, EScfgPaymentStatus::AlreadyOwned) &&
		(*R)->Kind == EScfgPayKind::Unlock && (*R)->Item == TEXT("chapter2") && (*R)->Token == TEXT("USDC") &&
		(*R)->SessionId.IsEmpty());
	TestEqual(TEXT("no sheet was shown"), W.Host->Sheets.Num(), 0);
	TestEqual(TEXT("no checkout"), S.NumCheckouts(), 0);
	TestTrue(TEXT("the callback gets it too (as Ren'Py's ScfgPay)"),
		Called->Num() == 1 && (*Called)[0].Status == EScfgPaymentStatus::AlreadyOwned);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsCancelTest, "test_pay_cancel_returns_cancelled_and_calls_callback")

bool FScfgFlowsCancelTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	const TSharedRef<FResults> Resolved = NewResults();
	S.OnPaymentResolved = [Resolved](const FScfgPaymentResult& R) { Resolved->Add(R); };
	TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	SameLines(*this, TEXT("the payment sheet is shown"), W.Host->Kinds, TArray<FString>{TEXT("payment")});
	TestFalse(TEXT("pay() waits for the sheet"), Box->IsSet());
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	TestTrue(TEXT("the callback is registered for the session"), S.CallbackSessionIds().Contains(Sid));
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("pay() resolves when the sheet closes"), Box->IsSet());
	TestTrue(TEXT("cancelled"), Is(*Box, EScfgPaymentStatus::Cancelled) && (*Box)->Kind == EScfgPayKind::Unlock &&
		(*Box)->Item == TEXT("chapter2") && (*Box)->SessionId == Sid);
	TestTrue(TEXT("the callback got the same result"),
		Called->Num() == 1 && (*Called)[0].Status == EScfgPaymentStatus::Cancelled && (*Called)[0].SessionId == Sid);
	TestEqual(TEXT("not payment_resolved: the callback had it"), Resolved->Num(), 0);
	TestTrue(TEXT("the sheet is closed and the checkout gone"),
		W.Host->Closes == 1 && S.NumCheckouts() == 0 && S.NumBackground() == 0);
	W.Pass();
	TestEqual(TEXT("the session is marked cancelled"), UiStatus(S, Sid), TEXT("cancelled"));
	// without a callback, the cancel reaches payment_resolved (Ren'Py's on_result)
	Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("cancelled before any session"), Is(*Box, EScfgPaymentStatus::Cancelled));
	TestTrue(TEXT("and emitted"), Resolved->Num() == 1 && (*Resolved)[0].Status == EScfgPaymentStatus::Cancelled);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsKeepPlayingTest, "test_keep_playing_delivers_later_result_via_signal")

bool FScfgFlowsKeepPlayingTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	S.bNotify = true;
	const TSharedRef<FResults> Resolved = NewResults();
	S.OnPaymentResolved = [Resolved](const FScfgPaymentResult& R) { Resolved->Add(R); };
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	TestTrue(TEXT("pending, while the payment confirms"), Is(*Box, EScfgPaymentStatus::Pending) &&
		(*Box)->SessionId == Sid);
	TestTrue(TEXT("the controller moved to the background"), S.NumCheckouts() == 0 && S.NumBackground() == 1);
	TestEqual(TEXT("nothing resolved yet"), Resolved->Num(), 0);
	W.Pass();
	TestEqual(TEXT("the session is marked pending"), UiStatus(S, Sid), TEXT("pending"));
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the final result reaches payment_resolved"), Resolved->Num(), 1);
	if (Resolved->Num() == 1)
	{
		const FScfgPaymentResult& Paid = (*Resolved)[0];
		TestTrue(TEXT("paid"), Paid.Status == EScfgPaymentStatus::Paid && Paid.Item == TEXT("chapter2") &&
			Paid.SessionId == Sid && Paid.TxHash == ScfgString::FromUtf8(kTx));
	}
	SameLines(*this, TEXT("a notification (notify is on)"), W.Host->Notes,
		TArray<FString>{TEXT("Payment confirmed: Chapter 2. Thank you!")});
	TestEqual(TEXT("a final background controller is closed and dropped"), S.NumBackground(), 0);
	TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsTipNotifyTest, "test_paid_tip_notifies_without_a_title_and_respects_notify_off")

bool FScfgFlowsTipNotifyTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), Called);
	const scfg::Result<bool> Next = W.Host->CheckoutOf(-1).Core().next_step();
	TestTrue(TEXT("past the amount step"), Next.ok() && Next.value());
	FScfgCheckout& Ctl = W.Session(*this);
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the callback gets the paid result"), Statuses(*Called), TEXT("Paid"));
	TestEqual(TEXT("no notification: notify is off by default"), W.Host->Notes.Num(), 0);
	S.bNotify = true;
	scfg::models::PaymentResult Paid;
	Paid.status = "paid";
	Paid.kind = "tip";
	Paid.session_id = "ps_x";
	S.DeliverForTests(Paid);
	SameLines(*this, TEXT("a tip's notification"), W.Host->Notes,
		TArray<FString>{TEXT("Payment confirmed. Thank you for your support!")});
	scfg::models::PaymentResult Expired;
	Expired.status = "expired";
	Expired.kind = "tip";
	Expired.session_id = "ps_y";
	S.DeliverForTests(Expired);
	TestEqual(TEXT("only paid results notify"), W.Host->Notes.Num(), 1);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsReleasesPageTest, "test_closing_the_sheet_releases_the_page_first")

bool FScfgFlowsReleasesPageTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	W.bRecordingServer = true;
	FScfgFacade& S = W.Started(*this);
	W.Pay(EScfgPayKind::Unlock, Unlock());
	TWeakPtr<FScfgCheckout> Weak;
	{
		FScfgCheckout& Ctl = W.Session(*this);
		Ctl.OpenBrowser();
		Weak = W.Host->Sheet(-1)->Checkout;
	}
	TestEqual(TEXT("the browser checkout started the desktop page"), W.Servers->Num(), 1);
	if (W.Servers->Num() != 1)
	{
		return true;
	}
	SameLines(*this, TEXT("and opened the browser on it"), W.Platform->Opened,
		TArray<FString>{ScfgString::FromUtf8(kServerUrl)});
	TestFalse(TEXT("an EVM page gets no build_tx (no /build route, as in Python)"), (*W.Servers)[0]->bHasBuildTx);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	SameLines(*this, TEXT("the page is stopped while the controller still exists, and only then is the sheet closed"),
		*W.Journal, TArray<FString>{TEXT("page stopped"), TEXT("sheet closed")});
	TestTrue(TEXT("not running"), (*W.Servers)[0]->bStopped);
	TestFalse(TEXT("the controller is released"), Weak.IsValid());
	TestTrue(TEXT("the page, and the callbacks it held, are gone"), (*W.Servers)[0]->bDestroyed);
	for (int32 I = 0; I < 3; ++I)
	{
		W.Pump();  // nothing left to pump the page or call into the controller
	}
	// keep playing: the page keeps answering until the session is final or the browser timeout
	W.Journal->Reset();
	W.Pay(EScfgPayKind::Unlock, Unlock());
	{
		FScfgCheckout& Ctl = W.Session(*this);
		Ctl.OpenBrowser();
		Weak = W.Host->Sheet(-1)->Checkout;
	}
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	SameLines(*this, TEXT("keep playing leaves the page running"), *W.Journal, TArray<FString>{TEXT("sheet closed")});
	TestFalse(TEXT("and answering"), W.Servers->Last()->bStopped);
	W.Advance(30.0 * 60.0 + 1.0);  // the browser timeout_min, and a second
	W.Pump();
	SameLines(*this, TEXT("past the browser timeout the page is stopped first"), *W.Journal,
		TArray<FString>{TEXT("sheet closed"), TEXT("page stopped")});
	TestEqual(TEXT("and the controller dropped"), S.NumBackground(), 0);
	TestFalse(TEXT("and released"), Weak.IsValid());
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsNoSheetTest, "test_no_sheet_ui_fails_clearly")

bool FScfgFlowsNoSheetTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	S.SheetHost = MakeShared<IScfgSheetHost>();  // the bare interface: it shows nothing
	const TSharedRef<TOptional<FScfgPaymentResult>> R = W.Pay(EScfgPayKind::Unlock, Unlock());
	TestTrue(TEXT("a failed result"), Is(*R, EScfgPaymentStatus::Failed) && (*R)->Error == TEXT("sheet.unavailable"));
	TestEqual(TEXT("no checkout left open"), S.NumCheckouts(), 0);
	TestTrue(TEXT("reported"), W.Reports().Errors.Num() == 1 && W.Reports().Errors[0].StartsWith(TEXT("scfg: ")));
	const TSharedRef<TOptional<FScfgRestoreResult>> RR = W.Restore();
	TestTrue(TEXT("restore too"), RR->IsSet() && (*RR)->Status == EScfgRestoreStatus::Failed &&
		(*RR)->Error == TEXT("sheet.unavailable"));
	TestEqual(TEXT("no restore left open"), S.NumRestores(), 0);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsMisusedPayTest, "test_misused_pay_resolves_failed_with_code")

bool FScfgFlowsMisusedPayTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	{
		FWorld Bare;
		Bare.Make();
		const TSharedRef<TOptional<FScfgPaymentResult>> R = Bare.Pay(EScfgPayKind::Unlock, Unlock());
		TestTrue(TEXT("before configure"), Is(*R, EScfgPaymentStatus::Failed) &&
			(*R)->Error == TEXT("configure.not_called") && (*R)->Kind == EScfgPayKind::Unlock &&
			(*R)->Item == TEXT("chapter2"));
		const TSharedRef<TOptional<FScfgRestoreResult>> RR = Bare.Restore();
		TestTrue(TEXT("restore before configure"), RR->IsSet() && (*RR)->Status == EScfgRestoreStatus::Failed &&
			(*RR)->Error == TEXT("configure.not_called"));
		TestEqual(TEXT("both reported"), Bare.Reports().Errors.Num(), 2);
	}
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	struct FCase
	{
		EScfgPayKind Kind;
		FScfgPayRequest Request;
		const TCHAR* Code;
	};
	FScfgPayRequest DaiToken = Unlock();
	DaiToken.Token = TEXT("DAI");
	const TArray<FCase> Cases = {
		{EScfgPayKind::Unlock, Unlock(TEXT("nope")), TEXT("item.unknown")},
		{EScfgPayKind::Unlock, FScfgPayRequest(), TEXT("item.unknown")},
		{static_cast<EScfgPayKind>(7), Unlock(), TEXT("payment.bad_kind")},  // Godot's "gift"
		{EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("")), TEXT("tip.needs_amount_and_token")},
		{EScfgPayKind::Tip, Tip(TEXT("0.10"), TEXT("USDC")), TEXT("tip.below_minimum")},
		{EScfgPayKind::Unlock, DaiToken, TEXT("token.not_enabled")},
	};
	const TSharedRef<TArray<FString>> Called = MakeShared<TArray<FString>>();
	for (const FCase& Each : Cases)
	{
		TSharedRef<TOptional<FScfgPaymentResult>> Box = MakeShared<TOptional<FScfgPaymentResult>>();
		FScfgPayCallback Callback;
		Callback.Fn = [Called](const FScfgPaymentResult& R) { Called->Add(R.Error); };
		S.Pay(Each.Kind, Each.Request, Callback, [Box](const FScfgPaymentResult& R) { *Box = R; });
		TestTrue(*FString::Printf(TEXT("%s"), Each.Code),
			Is(*Box, EScfgPaymentStatus::Failed) && (*Box)->Error == Each.Code);
	}
	SameLines(*this, TEXT("a valid callback gets the failure too"), *Called,
		TArray<FString>{TEXT("item.unknown"), TEXT("item.unknown"), TEXT("payment.bad_kind"),
			TEXT("tip.needs_amount_and_token"), TEXT("tip.below_minimum"), TEXT("token.not_enabled")});
	TestEqual(TEXT("each reported once"), W.Reports().Errors.Num(), Cases.Num());
	TestTrue(TEXT("no sheet was shown"),
		W.Host->Sheets.Num() == 0 && S.NumCheckouts() == 0 && S.NumRestores() == 0);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsIntervalTest, "test_interval_is_3s_while_a_sheet_is_open")

bool FScfgFlowsIntervalTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	W.bManualWorker = false;  // the worker reads the facade's PollInterval(); frames with no time pass tick nothing
	W.Started(*this);
	FScfgWorker& Worker = W.Worker();
	TestEqual(TEXT("15 s with nothing open"), Worker.GetInterval(), 15.0);
	W.Pay(EScfgPayKind::Unlock, Unlock());
	TestEqual(TEXT("3 s while the payment sheet is open"), Worker.GetInterval(), 3.0);
	W.Session(*this);
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	TestEqual(TEXT("15 s again for a background payment"), Worker.GetInterval(), 15.0);
	W.Restore();
	TestEqual(TEXT("3 s while the restore screen is open"), Worker.GetInterval(), 3.0);
	W.Host->CloseSheet(-1, TEXT("close"));
	W.Pump();
	TestEqual(TEXT("15 s once it is closed"), Worker.GetInterval(), 15.0);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsFreedSheetTest, "test_a_sheet_freed_without_closing_counts_as_cancel")

bool FScfgFlowsFreedSheetTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	W.Host->Release(-1);  // e.g. the game destroyed it with its level
	W.Pump();             // noticed within a frame
	TestTrue(TEXT("pay() resolves cancelled"), Is(*Box, EScfgPaymentStatus::Cancelled));
	TestEqual(TEXT("the checkout is closed"), S.NumCheckouts(), 0);
	TestEqual(TEXT("a freed sheet is not closed again"), W.Host->Closes, 0);
	// a second one, released after a session started
	Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	W.Session(*this);
	W.Host->Release(-1);
	W.Pump();
	TestTrue(TEXT("a released sheet with a session counts as cancel too"), Is(*Box, EScfgPaymentStatus::Cancelled));
	TestTrue(TEXT("closed, and the sheet not closed again"), S.NumCheckouts() == 0 && W.Host->Closes == 0);
	return true;
}

// --- quitting (Ren'Py's _shutdown) ------------------------------------------------------------------------------------

SCFG_FACADE_FLOWS_TEST(FScfgFlowsQuitTest, "test_quitting_with_the_sheet_open_closes_without_delivery")

bool FScfgFlowsQuitTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	W.bRecordingServer = true;
	FScfgFacade& S = W.Started(*this);
	S.bNotify = true;
	const TSharedRef<FResults> Called = NewResults();
	const TSharedRef<FResults> Resolved = NewResults();
	S.OnPaymentResolved = [Resolved](const FScfgPaymentResult& R) { Resolved->Add(R); };
	// one payment kept playing in the background, one with its sheet open, and the restore screen
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	W.Session(*this).OpenBrowser();
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	Ctl.OpenBrowser();
	const TSharedRef<TArray<FScfgRestoreResult>> Restored = MakeShared<TArray<FScfgRestoreResult>>();
	const TSharedRef<TOptional<FScfgRestoreResult>> RBox = W.Restore(Restored);
	TestEqual(TEXT("nothing delivered yet"), Called->Num() + Resolved->Num() + Restored->Num(), 0);
	W.Journal->Reset();
	TSharedPtr<FScfgFacade> Kept = W.Facade;
	W.Drop();  // the game instance shuts down
	TestTrue(TEXT("pay() is released, cancelled"), Is(*Box, EScfgPaymentStatus::Cancelled) && (*Box)->SessionId == Sid);
	TestTrue(TEXT("restore() is released, cancelled"),
		RBox->IsSet() && (*RBox)->Status == EScfgRestoreStatus::Cancelled);
	TestTrue(TEXT("nothing is delivered or announced"),
		Called->Num() == 0 && Resolved->Num() == 0 && Restored->Num() == 0 && W.Host->Notes.Num() == 0);
	SameLines(*this, TEXT("both pages are stopped while their controllers exist"), *W.Journal,
		TArray<FString>{TEXT("page stopped"), TEXT("sheet closed"), TEXT("sheet closed"),
			TEXT("page stopped")});
	TestTrue(TEXT("every controller is closed"),
		Kept->NumCheckouts() == 0 && Kept->NumBackground() == 0 && Kept->NumRestores() == 0);
	const Json OnDisk = W.OnDiskPending().as_dict().get(ScfgString::ToUtf8(Sid));
	TestTrue(TEXT("the session is not cancelled: it is still watched on the next launch"),
		OnDisk.is_dict() && ScfgConvert::Str(OnDisk.as_dict().get("ui_status")) != TEXT("cancelled"));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsAwaitersReleasedTest, "test_awaiters_are_released_when_the_autoload_is_freed")

bool FScfgFlowsAwaitersReleasedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	W.Started(*this);
	W.Drop();
	// a facade that is never pumped (the worker starts with the first pay())
	W.Configured(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	const TSharedRef<TOptional<FScfgRestoreResult>> RBox = W.Restore();
	TestTrue(TEXT("both wait"), !Box->IsSet() && !RBox->IsSet());
	W.Facade.Reset();  // destroyed: its destructor runs the teardown
	W.Transport = nullptr;
	TestTrue(TEXT("freeing the facade releases pay() and restore() (cancelled), so nothing is left waiting"),
		Is(*Box, EScfgPaymentStatus::Cancelled) && RBox->IsSet() &&
			(*RBox)->Status == EScfgRestoreStatus::Cancelled);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsRequireConfigTest, "test_pay_before_configure_reports_the_require_config_error")

bool FScfgFlowsRequireConfigTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& Bare = W.Make();
	TestFalse(TEXT("owns() before configure"), Bare.Owns(TEXT("chapter2")));
	const TSharedRef<TOptional<FScfgPaymentResult>> R = W.Pay(EScfgPayKind::Unlock, Unlock());
	const TSharedRef<TOptional<FScfgRestoreResult>> RR = W.Restore();
	TestTrue(TEXT("both fail with the code"), R->IsSet() && (*R)->Error == TEXT("configure.not_called") &&
		RR->IsSet() && (*RR)->Error == TEXT("configure.not_called"));
	const FString Line = TEXT("scfg: call Configure(...) first");
	SameLines(*this, TEXT("each reported once, with RequireConfig's error"), W.Reports().Errors,
		TArray<FString>{Line, Line, Line});
	return true;
}

// --- restore --------------------------------------------------------------------------------------------------------

SCFG_FACADE_FLOWS_TEST(FScfgFlowsRestoreTest, "test_restore_flow_restores_and_notifies")

bool FScfgFlowsRestoreTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	S.bNotify = true;
	W.Chains->Evm["arbitrum"] = FFakeChain();
	PutOldPayment(W.Chains->Chain("arbitrum"), kTx);
	const TSharedRef<TArray<FScfgRestoreResult>> Called = MakeShared<TArray<FScfgRestoreResult>>();
	TSharedRef<TOptional<FScfgRestoreResult>> Box = W.Restore(Called);
	SameLines(*this, TEXT("the restore screen is shown"), W.Host->Kinds, TArray<FString>{TEXT("restore")});
	scfg::restore_flow::RestoreController& Ctl = W.Host->RestoreOf(-1).Core();
	const scfg::Result<bool> Checked = Ctl.check_tx(kTx);
	TestTrue(TEXT("a transaction hash"), Checked.ok() && Checked.value());
	W.Pass();  // the lookup; one candidate, classified: saved at once
	W.Pass();  // the save
	TestEqual(TEXT("restored"), FString(UTF8_TO_TCHAR(Ctl.state().c_str())), TEXT("done"));
	TestEqual(TEXT("the callback gets the result when it is done"), Called->Num(), 1);
	if (Called->Num() == 1)
	{
		TestTrue(TEXT("one receipt restored"),
			(*Called)[0].Status == EScfgRestoreStatus::Restored && (*Called)[0].Restored.Num() == 1);
	}
	SameLines(*this, TEXT("a notification"), W.Host->Notes, TArray<FString>{TEXT("Purchases restored.")});
	TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
	TestFalse(TEXT("restore() waits for the screen to close"), Box->IsSet());
	W.Host->CloseSheet(-1, TEXT("close"));
	W.Pump();
	TestTrue(TEXT("restore() returns the result"),
		Box->IsSet() && (*Box)->Status == EScfgRestoreStatus::Restored && (*Box)->Restored.Num() == 1);
	TestTrue(TEXT("the screen is closed; the callback was called once"),
		S.NumRestores() == 0 && W.Host->Closes == 1 && Called->Num() == 1);
	// closing before anything is done: cancelled, and the callback is dropped
	Box = W.Restore(Called);
	W.Host->CloseSheet(-1, TEXT("close"));
	W.Pump();
	TestTrue(TEXT("closed at once"), Box->IsSet() && (*Box)->Status == EScfgRestoreStatus::Cancelled);
	TestTrue(TEXT("no callback for a cancelled restore"), Called->Num() == 1 && S.NumRestoreCallbacks() == 0);
	return true;
}

// --- concurrent flows -----------------------------------------------------------------------------------------------

// Two Pay calls at once (an unlock, then a tip while its sheet is still open): two sheets, two sessions, and each call
// resolves from its own sheet, with its own callback, in any order.
SCFG_FACADE_FLOWS_TEST(FScfgFlowsTwoPaysTest, "test_two_pay_calls_at_once_resolve_independently")

bool FScfgFlowsTwoPaysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> UnlockCalls = NewResults();
	const TSharedRef<FResults> TipCalls = NewResults();
	const TSharedRef<TOptional<FScfgPaymentResult>> UnlockBox = W.Pay(EScfgPayKind::Unlock, Unlock(), UnlockCalls);
	FScfgCheckout& First = W.Session(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> TipBox =
		W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), TipCalls);
	FScfgCheckout& Second = W.Session(*this);
	SameLines(*this, TEXT("a sheet each"), W.Host->Kinds, TArray<FString>{TEXT("payment"), TEXT("payment")});
	TestEqual(TEXT("two checkouts"), S.NumCheckouts(), 2);
	TestNotEqual(TEXT("two sessions"), SessionIdOf(First), SessionIdOf(Second));
	W.PayOnChain(Second);
	TestEqual(TEXT("the tip is paid"), FString(UTF8_TO_TCHAR(Second.Core().state().c_str())), TEXT("paid"));
	TestNotEqual(TEXT("the unlock is not"), FString(UTF8_TO_TCHAR(First.Core().state().c_str())), FString(TEXT("paid")));
	const FString FirstId = First.GetId();
	const FString FirstSid = SessionIdOf(First);
	const FString SecondSid = SessionIdOf(Second);
	W.Host->CloseSheet(1, TEXT("close"));
	W.Pump();
	TestTrue(TEXT("the tip resolves paid"), Is(*TipBox, EScfgPaymentStatus::Paid) &&
		(*TipBox)->Kind == EScfgPayKind::Tip && (*TipBox)->SessionId == SecondSid);
	TestFalse(TEXT("the unlock still waits for its sheet"), UnlockBox->IsSet());
	SameLines(*this, TEXT("only its checkout is left"), S.CheckoutIds(), TArray<FString>{FirstId});
	W.Host->CloseSheet(0, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("the unlock resolves cancelled"), Is(*UnlockBox, EScfgPaymentStatus::Cancelled) &&
		(*UnlockBox)->Kind == EScfgPayKind::Unlock && (*UnlockBox)->SessionId == FirstSid);
	TestTrue(TEXT("each callback once, with its own result"), UnlockCalls->Num() == 1 && TipCalls->Num() == 1);
	TestFalse(TEXT("the tip did not unlock chapter2"), S.Owns(TEXT("chapter2")));
	return true;
}

// Restore while a Pay sheet is open: the restore screen opens over it, restores and closes, and the payment carries on
// and resolves from its own sheet.
SCFG_FACADE_FLOWS_TEST(FScfgFlowsRestoreDuringPayTest, "test_restore_during_pay")

bool FScfgFlowsRestoreDuringPayTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Paying = W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	FScfgCheckout& Ctl = W.Session(*this);
	const std::string OldTx = Hash("cd");
	PutOldPayment(W.Chains->Chain("arbitrum"), OldTx);
	const TSharedRef<TOptional<FScfgRestoreResult>> Restoring = W.Restore();
	SameLines(*this, TEXT("the restore screen opens while the sheet is open"), W.Host->Kinds,
		TArray<FString>{TEXT("payment"), TEXT("restore")});
	scfg::restore_flow::RestoreController& RCtl = W.Host->RestoreOf(1).Core();
	const scfg::Result<bool> Checked = RCtl.check_tx(OldTx);
	TestTrue(TEXT("a transaction hash"), Checked.ok() && Checked.value());
	W.Pass();  // the lookup
	W.Pass();  // the save
	TestEqual(TEXT("restored"), FString(UTF8_TO_TCHAR(RCtl.state().c_str())), TEXT("done"));
	W.Host->CloseSheet(1, TEXT("close"));
	W.Pump();
	TestTrue(TEXT("restore() resolves restored"), Restoring->IsSet() &&
		(*Restoring)->Status == EScfgRestoreStatus::Restored && (*Restoring)->Restored.Num() == 1);
	TestTrue(TEXT("chapter2 is owned"), S.Owns(TEXT("chapter2")));
	TestFalse(TEXT("pay() still waits for its sheet"), Paying->IsSet());
	TestTrue(TEXT("the checkout is still open"), S.NumCheckouts() == 1 && S.NumRestores() == 0);
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the tip confirms meanwhile"), FString(UTF8_TO_TCHAR(Ctl.Core().state().c_str())), TEXT("paid"));
	const FString Sid = SessionIdOf(Ctl);
	W.Host->CloseSheet(0, TEXT("close"));
	W.Pump();
	TestTrue(TEXT("pay() resolves paid"), Is(*Paying, EScfgPaymentStatus::Paid) &&
		(*Paying)->Kind == EScfgPayKind::Tip && (*Paying)->SessionId == Sid);
	return true;
}

// --- actions for the sheet ------------------------------------------------------------------------------------------

SCFG_FACADE_FLOWS_TEST(FScfgFlowsMobileTest, "test_mobile_offers_wallet_app_link")

bool FScfgFlowsMobileTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	W.Started(*this);
	W.Platform->bMobile = true;
	W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Before = W.Host->CheckoutOf(-1);
	TestTrue(TEXT("a mobile checkout"), Before.Core().mobile);
	Before.OpenWalletApp();
	TestEqual(TEXT("nothing to open before a session"), W.Platform->Opened.Num(), 0);
	FScfgCheckout& Ctl = W.Session(*this);
	const std::vector<std::string> Methods = Ctl.Core().methods();
	TestFalse(TEXT("no browser checkout on a phone"),
		std::find(Methods.begin(), Methods.end(), "browser") != Methods.end());
	const scfg::Result<std::optional<std::string>> Uri = Ctl.Core().payment_uri();
	const FString Link = Uri.ok() && Uri.value() ? ScfgString::FromUtf8(*Uri.value()) : FString();
	TestTrue(*FString::Printf(TEXT("an EIP-681 link (%s)"), *Link), Link.StartsWith(TEXT("ethereum:")));
	Ctl.OpenWalletApp();
	SameLines(*this, TEXT("Open wallet app opens it"), W.Platform->Opened, TArray<FString>{Link});
	Ctl.OpenExplorer(TEXT("https://arbiscan.io/tx/") + ScfgString::FromUtf8(kTx));
	Ctl.OpenExplorer(TEXT(""));
	TestEqual(TEXT("open_explorer opens a URL, and nothing for none"), W.Platform->Opened.Num(), 2);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsClipboardTest, "test_copy_and_paste_use_the_clipboard")

bool FScfgFlowsClipboardTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	S.Copy(ScfgString::FromUtf8(kTx));
	TestEqual(TEXT("copy"), W.Platform->Clipboard, ScfgString::FromUtf8(kTx));
	W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	Ctl.Core().error = "invalid_hash";
	W.Platform->Clipboard = TEXT("  0x12ab-CD!ef  and more words\n");
	Ctl.Paste();
	TestTrue(TEXT("the first word, hash characters only (Ren'Py's _clean_paste)"),
		Ctl.Core().tx_input == "0x12abCDef" && !Ctl.Core().error);
	W.Platform->Clipboard = FString::ChrN(100, TEXT('x'));
	Ctl.Paste();
	TestEqual(TEXT("at most 90 characters"), static_cast<int32>(Ctl.Core().tx_input.size()), 90);
	W.Platform->Clipboard = TEXT("   ");
	Ctl.Paste();
	TestTrue(TEXT("an empty clipboard pastes nothing"), Ctl.Core().tx_input.size() == 90 && !Ctl.Core().error);
	W.Restore();
	FScfgRestore& RC = W.Host->RestoreOf(-1);
	W.Platform->Clipboard = ScfgString::FromUtf8(kTx);
	RC.Paste();
	TestTrue(TEXT("into the restore screen's field"), RC.Core().tx_input == kTx);
	RC.Core().tx_input = "";
	TestTrue(TEXT("Check is pressed"), RC.Core().check_tx(kTx).ok() && RC.Core().state() == "checking");
	RC.Paste();
	TestTrue(TEXT("dropped once the player pressed Check"), RC.Core().tx_input.empty());
	return true;
}

// Ren'Py's _clean_paste: `(text or "").split()[0]`, then only TX_CHARS, then `[:90]` (90 hash characters, not the first
// 90 characters filtered), and it stops at 90.
SCFG_FACADE_FLOWS_TEST(FScfgFlowsCleanPasteTest, "test_clean_paste_matches_renpy_and_stops_at_90")

bool FScfgFlowsCleanPasteTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	const TArray<TPair<FString, FString>> Cases = {
		{TEXT("  0x12ab-CD!ef  and more words\n"), TEXT("0x12abCDef")},
		{FString::ChrN(100, TEXT('x')), FString::ChrN(90, TEXT('x'))},
		{FString::ChrN(50, TEXT('-')) + FString::ChrN(100, TEXT('a')), FString::ChrN(90, TEXT('a'))},
		{[] { FString S; for (int32 I = 0; I < 100; ++I) { S += TEXT("a-"); } return S; }(), FString::ChrN(90, TEXT('a'))},
		{TEXT("　abc def"), TEXT("abc")},
		{TEXT("ab cd"), TEXT("ab")},
		{TEXT("\n\t "), TEXT("")},
		{TEXT(""), TEXT("")},
	};
	for (const TPair<FString, FString>& Each : Cases)
	{
		TestEqual(*FString::Printf(TEXT("clean_paste(%s)"), *Each.Key.Left(40)), FScfgFacade::CleanPaste(Each.Key),
			Each.Value);
	}
	const FString Huge = FString::ChrN(100000, TEXT('a'));
	const double Started = FPlatformTime::Seconds();
	const FString Cleaned = FScfgFacade::CleanPaste(Huge);
	const double TookMs = (FPlatformTime::Seconds() - Started) * 1000.0;
	TestEqual(TEXT("a 100 kB word: 90 characters"), Cleaned.Len(), 90);
	TestTrue(*FString::Printf(TEXT("and it stops there instead of filtering the whole word (%.1f ms)"), TookMs),
		TookMs < 100.0);
	return true;
}

// --- the Pay callback map (Ren'Py's _state.callbacks; tests/unit/test_renpy_callbacks.py) -----------------------------

SCFG_FACADE_FLOWS_TEST(FScfgFlowsPaidDropsCallbackTest, "test_a_delivered_paid_result_drops_the_callback")

bool FScfgFlowsPaidDropsCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	W.Pass();
	TestTrue(TEXT("registered while the payment is pending"), S.CallbackSessionIds().Contains(Sid));
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the callback got the final result"), Statuses(*Called), TEXT("Paid"));
	TestEqual(TEXT("and is dropped: the session is over"), S.CallbackSessionIds().Num(), 0);
	W.Pass();
	TestEqual(TEXT("not registered again"), S.CallbackSessionIds().Num(), 0);
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsExpiredKeepsCallbackTest, "test_an_expired_session_keeps_its_callback_until_it_is_paid")

bool FScfgFlowsExpiredKeepsCallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	TWeakPtr<FScfgCheckout> Kept = W.Host->Sheet(-1)->Checkout;
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	W.Pass();
	*W.Now += 31 * 60;
	W.Pass();
	TestEqual(TEXT("the window closed"), Statuses(*Called), TEXT("Expired"));
	TestEqual(TEXT("its controller is closed"), S.NumBackground(), 0);
	TestTrue(TEXT("the scan goes on: a later payment still reaches the callback"), S.CallbackSessionIds().Contains(Sid));
	// _pay_on_chain(env, ctl) reads the session's units: the controller is released, so they come from the Engine
	const int64_t Units = W.Worker().GetSnapshot().pending.as_dict().get(ScfgString::ToUtf8(Sid)).as_dict()
		.get("units").as_int64();
	FFakeChain& Arbitrum = W.Chains->Chain("arbitrum");
	const int64_t Block = Arbitrum.Head + 1;
	Arbitrum.Logs.push_back(TransferLog(UsdcArbitrum(), kPayer, kDev, Units, Block, kTx));
	Arbitrum.Head = Block;
	W.Pass();
	Arbitrum.Head = Block + 19;
	W.Pass();
	TestEqual(TEXT("the payment found later"), Statuses(*Called), TEXT("Expired,Paid"));
	TestEqual(TEXT("dropped once paid"), S.CallbackSessionIds().Num(), 0);
	TestFalse(TEXT("(the background controller was released)"), Kept.IsValid());
	return true;
}

// A sheet left open on an expired session: once the session leaves the Engine at its scan horizon the callback is
// dropped, and the sheet's controller, still polled by every pump, does not register it again.
SCFG_FACADE_FLOWS_TEST(FScfgFlowsHorizonTest,
	"test_an_open_sheet_past_the_scan_horizon_does_not_register_its_callback_again")

bool FScfgFlowsHorizonTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	W.Pass();
	*W.Now += 31 * 60;
	W.Pass();
	TestEqual(TEXT("the open sheet shows the expired session"), FString(UTF8_TO_TCHAR(Ctl.Core().state().c_str())),
		TEXT("expired"));
	TestTrue(TEXT("the scan goes on"), S.CallbackSessionIds().Contains(Sid));
	*W.Now += 24 * 3600;
	W.Pass();
	W.Pass();
	TestFalse(TEXT("the session left the Engine"),
		W.Worker().GetSnapshot().pending.as_dict().contains(ScfgString::ToUtf8(Sid)));
	TestEqual(TEXT("its callback is dropped"), S.CallbackSessionIds().Num(), 0);
	W.Pass();
	W.Pass();
	TestEqual(TEXT("not registered again"), S.CallbackSessionIds().Num(), 0);
	TestEqual(TEXT("the sheet is still open"), S.NumCheckouts(), 1);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsDiscardedTest, "test_a_discarded_session_drops_its_callback")

bool FScfgFlowsDiscardedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	W.Pay(EScfgPayKind::Unlock, Unlock(), NewResults());
	FScfgCheckout& Ctl = W.Session(*this);
	const FString First = SessionIdOf(Ctl);
	W.Pass();
	TestTrue(TEXT("registered"), S.CallbackSessionIds().Contains(First));
	TestTrue(TEXT("the player switched networks: the unpaid session is discarded"),
		Ctl.Core().choose_network("base").ok());
	W.Pass();
	W.Pass();
	const FString Second = SessionIdOf(Ctl);
	TestTrue(TEXT("a new session"), !Second.IsEmpty() && Second != First);
	SameLines(*this, TEXT("only the live session keeps a callback"), S.CallbackSessionIds(), TArray<FString>{Second});
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	return true;
}

// RegisterCallback's state guard: no snapshot ever lists the session (so PruneCallbacks cannot drop it), and the paid
// controller is still polled in the pump that delivered the result.
SCFG_FACADE_FLOWS_TEST(FScfgFlowsUnlistedTest,
	"test_a_session_paid_before_any_snapshot_listed_it_releases_its_callback")

bool FScfgFlowsUnlistedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<scfg::engine::Snapshot> Frozen = MakeShared<scfg::engine::Snapshot>(W.Worker().GetSnapshot());
	S.SnapshotForTests = [Frozen]() { return &*Frozen; };  // no snapshot the pump reads lists the session
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Host->CheckoutOf(-1);
	TestTrue(TEXT("choose a network"), Ctl.Core().choose_network("arbitrum").ok());
	W.Pass();
	const FString Sid = SessionIdOf(Ctl);
	TestTrue(TEXT("registered"), !Sid.IsEmpty() && S.CallbackSessionIds().Contains(Sid));
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the callback got the paid result"), Statuses(*Called), TEXT("Paid"));
	TestEqual(TEXT("released although no snapshot listed it"), S.CallbackSessionIds().Num(), 0);
	W.Pass();
	TestEqual(TEXT("not registered again"), S.CallbackSessionIds().Num(), 0);
	S.SnapshotForTests = nullptr;
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsSnapshotBeforeDrainTest, "test_the_snapshot_is_read_before_the_outcomes_are_drained")

bool FScfgFlowsSnapshotBeforeDrainTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<FResults> Called = NewResults();
	W.Pay(EScfgPayKind::Unlock, Unlock(), Called);
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	W.Host->CloseSheet(-1, TEXT("keep_playing"));
	W.Pump();
	W.Pass();
	W.Pass();  // listed by a snapshot: the callback could be pruned
	// RacingWorker: right after a drain that found nothing, a pass lands: the paid session leaves the snapshot and
	// its outcome is queued.
	struct FRace
	{
		scfg::engine::Snapshot Current;
		bool bArmed = true;
		FScfgWorker::FOutcomes Queued;
	};
	const TSharedRef<FRace> Race = MakeShared<FRace>();
	Race->Current = W.Worker().GetSnapshot();
	S.SnapshotForTests = [Race]() { return &Race->Current; };
	const std::string Session = ScfgString::ToUtf8(Sid);
	S.DrainForTests = [Race, Session]() -> FScfgWorker::FOutcomes
	{
		if (Race->bArmed)
		{
			Race->bArmed = false;
			Json Pending = scfg::deep_copy(Race->Current.pending);
			Pending.as_dict().erase(Session);
			Race->Current.pending = Pending;
			scfg::watcher::StepOutcome Paid;
			Paid.session_id = Session;
			Paid.status = "paid";
			scfg::models::PaymentResult Result;
			Result.status = "paid";
			Result.kind = "unlock";
			Result.item = "chapter2";
			Result.session_id = Session;
			Paid.result = Result;
			Race->Queued.push_back(Paid);
			return {};
		}
		FScfgWorker::FOutcomes Out = std::move(Race->Queued);
		Race->Queued.clear();
		return Out;
	};
	W.Pump();  // drains nothing, then the pass lands
	W.Pump();  // drains the paid outcome
	TestEqual(TEXT("the paid result reached the callback"), Statuses(*Called), TEXT("Paid"));
	TestEqual(TEXT("released"), S.CallbackSessionIds().Num(), 0);
	S.SnapshotForTests = nullptr;
	S.DrainForTests = nullptr;
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsBrowserHintTest, "TheBrowserMethodSendsAHintToTheEngine")

// The default page server (FScfgLocalServer, a real 127.0.0.1 socket) on the browser method: Open checkout in browser
// opens the page's URL; the page, its session and, on EVM, no /build route; the page's POST /tx reaches the Engine as
// the session's hint (pumped by the facade, as a browser wallet would post it); closing the sheet stops the page.
// The HTTP half of Godot's test_closing_the_sheet_releases_the_page_first is here too.
bool FScfgFlowsBrowserHintTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;  // no ServerFactory: the facade's default, the localhost checkout server
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	Ctl.OpenBrowser();
	if (!TestEqual(TEXT("the browser is opened on the page"), W.Platform->Opened.Num(), 1))
	{
		return true;
	}
	const FString Url = W.Platform->Opened[0];
	const int32 Port = ScfgTestHttp::PortOf(Url);
	TestTrue(TEXT("at 127.0.0.1, on a random port, behind the secret"),
		Port > 0 && Port != 8000 && Url.Contains(TEXT("/c/")) && Url.EndsWith(TEXT("/")));
	TestFalse(TEXT("the browser method stays available"), Ctl.Core().browser_failed());
	const std::string Base = ScfgString::ToUtf8(Url.RightChop(Url.Find(TEXT("/c/"))));
	const TFunction<void()> Pump = [&W] { W.Pump(); };  // the facade polls the controller, which pumps the page
	std::optional<ScfgTestHttp::FAnswer> Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("GET", Base, Port), Pump);
	TestTrue(TEXT("the page is served"), Got && Got->Status == 200 && Got->Body.find("<html") != std::string::npos);
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("POST", Base + "build", Port, "{}"), Pump);
	TestTrue(TEXT("/build is not a route on EVM"), Got && Got->Status == 404);
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("GET", Base + "session.json", Port), Pump);
	const scfg::Result<Json> Info = Got ? scfg::json::loads(Got->Body) : scfg::Result<Json>(scfg::Err::value_error("none"));
	TestTrue(TEXT("session.json is the controller's session"), Info.ok() && Info.value().is_dict() &&
		Same(Info.value().as_dict().get("chain_id"), Json(42161)) && Same(Info.value().as_dict().get("kind"), Json("unlock")));
	const std::string Hint = "{\"tx_hash\": \"" + kTx + "\"}";
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("POST", Base + "tx", Port, Hint), Pump);
	TestTrue(TEXT("the page's hint is taken"), Got && Got->Status == 202 && Got->Body == "{\"ok\": true}");
	TestTrue(TEXT("the controller has it"), Ctl.Core().hint_tx == std::optional<std::string>(kTx));
	W.Pass();
	const Json Record = S.Pending().as_dict().get(ScfgString::ToUtf8(SessionIdOf(Ctl)));
	TestTrue(TEXT("and the Engine has it as the session's hint"),
		Record.is_dict() && Same(Record.as_dict().get("hint_tx"), Json(kTx)));
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("GET", Base + "status", Port), Pump);
	const scfg::Result<Json> Status = Got ? scfg::json::loads(Got->Body) : scfg::Result<Json>(scfg::Err::value_error("none"));
	TestTrue(TEXT("the page's status names the hint"), Status.ok() && Status.value().is_dict() &&
		Same(Status.value().as_dict().get("tx_hash"), Json(kTx)));
	TestEqual(TEXT("nothing reported"), W.Reports().Errors.Num() + W.Reports().Warnings.Num(), 0);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("closing the sheet stops the page"), ScfgTestHttp::Refused(Port));
	TestTrue(TEXT("and the sheet closes as any other"), Is(*Box, EScfgPaymentStatus::Cancelled));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsNoPageServerTest, "TheDesktopPathWorksWhenThePageCannotStart")

bool FScfgFlowsNoPageServerTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	// The localhost page on an address that is not this machine's (TEST-NET-1): it cannot listen
	const TSharedRef<TArray<FString>> PageErrors = MakeShared<TArray<FString>>();
	FScfgLocalServerPage Page;
	Page.Host = TEXT("192.0.2.1");
	Page.OnError = [PageErrors](const FString& Line) { PageErrors->Add(Line); };
	W.ServerFactory = FScfgLocalServer::Factory(MoveTemp(Page));
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	const std::vector<std::string> Methods = Ctl.Core().methods();
	TestTrue(TEXT("the desktop rule: every method is offered until the page is tried"),
		Methods == std::vector<std::string>{"browser", "qr", "manual"});
	Ctl.OpenBrowser();
	TestTrue(TEXT("Open checkout in browser: the page cannot start, so the browser is unavailable"),
		W.Platform->Opened.Num() == 0 && Ctl.Core().browser_failed() &&
			Ctl.Core().error == std::optional<std::string>("browser_unavailable"));
	TestTrue(TEXT("and drops out of the methods"),
		Ctl.Core().methods() == std::vector<std::string>{"qr", "manual"});
	TestEqual(TEXT("the page's Err is reported by the page, once per attempt"), PageErrors->Num(), 1);
	TestTrue(TEXT("as the page's Err"), PageErrors->Num() == 1 &&
		(*PageErrors)[0].StartsWith(TEXT("scfg: cannot start the checkout page: RuntimeError: cannot listen on 192.0.2.1: ")));
	const scfg::Result<std::optional<std::string>> Uri = Ctl.Core().payment_uri();
	TestTrue(TEXT("the QR's payment link"), Uri.ok() && Uri.value() && Uri.value()->rfind("ethereum:", 0) == 0);
	const scfg::Result<bool> Sent = Ctl.Core().submit_hash(kTx);
	TestTrue(TEXT("a pasted hash is taken"), Sent.ok() && Sent.value());
	W.Pass();
	const Json Record = S.Pending().as_dict().get(ScfgString::ToUtf8(SessionIdOf(Ctl)));
	TestTrue(TEXT("and reported to the Engine"),
		Record.is_dict() && Same(Record.as_dict().get("hint_tx"), Json(kTx)));
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("and the sheet closes as any other"), Is(*Box, EScfgPaymentStatus::Cancelled));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsNoPageFilesTest, "TheBrowserFallsBackWhenThePageFilesAreMissing")

// A game shipped without the plugin's Resources/Web/ (a packaging mistake): the page server is not made, the browser
// method is unavailable and the player pays by QR or by hand, instead of a browser opened on an error page.
bool FScfgFlowsNoPageFilesTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	const FString Empty = W.Dir / TEXT("no-web");  // inside the world's own directory (removed with it)
	IFileManager::Get().MakeDirectory(*Empty, true);
	const TSharedRef<TArray<FString>> PageErrors = MakeShared<TArray<FString>>();
	FScfgLocalServerPage Page;
	Page.WebDirectory = Empty;
	Page.OnError = [PageErrors](const FString& Line) { PageErrors->Add(Line); };
	W.ServerFactory = FScfgLocalServer::Factory(MoveTemp(Page));
	W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	Ctl.OpenBrowser();
	TestTrue(TEXT("no browser is opened, and the browser method is unavailable"),
		W.Platform->Opened.Num() == 0 && Ctl.Core().browser_failed() &&
			Ctl.Core().error == std::optional<std::string>("browser_unavailable"));
	TestTrue(TEXT("QR and manual remain"), Ctl.Core().methods() == std::vector<std::string>{"qr", "manual"});
	TestTrue(TEXT("the missing files are reported once"), PageErrors->Num() == 1 &&
		(*PageErrors)[0].Contains(TEXT("the checkout page files are missing")));
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	TestTrue(TEXT("and the sheet closes as any other"), Is(*Box, EScfgPaymentStatus::Cancelled));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsLateItemTest, "AnItemAddedAfterTheStartCanBeSoldAndOwned")

bool FScfgFlowsLateItemTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);  // Configure, AddItem("chapter2"), the first pump: the worker runs
	FScfgItem Chapter3;
	Chapter3.Code = 38;
	Chapter3.Price = TEXT("2.00");
	Chapter3.Token = TEXT("USDC");
	Chapter3.Title = TEXT("Chapter 3");
	TestFalse(TEXT("AddItem after the start"), S.AddItem(TEXT("chapter3"), Chapter3).IsSet());
	const TSharedRef<FResults> Called = NewResults();
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock(TEXT("chapter3")), Called);
	FScfgCheckout& Ctl = W.Session(*this);  // the Engine's own prepare_payment knows chapter3
	TestEqual(TEXT("no worker error"), W.Reports().Errors.Num(), 0);
	W.PayOnChain(Ctl);
	TestEqual(TEXT("the payment is detected"), FString(UTF8_TO_TCHAR(Ctl.Core().state().c_str())), TEXT("paid"));
	TestEqual(TEXT("the callback gets it"), Statuses(*Called), TEXT("Paid"));
	TestTrue(TEXT("chapter3 is owned"), S.Owns(TEXT("chapter3")));
	TestFalse(TEXT("chapter2 is not"), S.Owns(TEXT("chapter2")));
	W.Host->CloseSheet(-1, TEXT("finish"));
	W.Pump();
	TestTrue(TEXT("pay() resolves paid"), Is(*Box, EScfgPaymentStatus::Paid) && (*Box)->Item == TEXT("chapter3"));
	const TSharedRef<TOptional<FScfgPaymentResult>> Again = W.Pay(EScfgPayKind::Unlock, Unlock(TEXT("chapter3")));
	TestTrue(TEXT("and a second Pay is already_owned"), Is(*Again, EScfgPaymentStatus::AlreadyOwned));
	return true;
}

SCFG_FACADE_FLOWS_TEST(FScfgFlowsClearWithSheetTest, "ClearLocalDataWithAPaymentSheetOpen")

bool FScfgFlowsClearWithSheetTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeFlowsTests;
	FWorld W;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	const FString Sid = SessionIdOf(Ctl);
	TestTrue(TEXT("a pending session"), S.Pending().as_dict().contains(ScfgString::ToUtf8(Sid)));
	S.ClearLocalData();
	TestTrue(TEXT("forgotten at once"), S.Pending().as_dict().empty());
	W.Pass();
	TestTrue(TEXT("the Engine is wiped too"), W.Worker().GetSnapshot().pending.as_dict().empty());
	TestTrue(TEXT("and the file"), Same(W.OnDiskPending(), Json::dict()));
	TestEqual(TEXT("the sheet is still open"), S.NumCheckouts(), 1);
	W.Host->CloseSheet(-1, TEXT("cancel"));
	W.Pump();
	W.Pass();
	TestTrue(TEXT("the cancel resolves as usual"), Is(*Box, EScfgPaymentStatus::Cancelled) && (*Box)->SessionId == Sid);
	TestTrue(TEXT("and marks nothing back (the session is gone)"), S.Pending().as_dict().empty());
	TestEqual(TEXT("no worker error"), W.Reports().Errors.Num(), 0);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
