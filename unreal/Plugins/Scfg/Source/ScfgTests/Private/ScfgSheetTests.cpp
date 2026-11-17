// Twin of godot/tests/test_sheet.gd and test_restore_screen.gd (and of the screens' parts of test_overlay.gd): the
// Slate payment sheet and restore screen of the default host, driven through FSlateApplication by simulated keys,
// gamepad buttons and sticks (ScfgSheetTestKit.h), Scfg.Glue.Sheet.<name>.
//
//   test_steps_follow_the_controller / test_keyboard_only_manual_payment -> TipWalksEveryStepByKeys,
//                                                                           UnlockWalksEveryStepByKeys
//   test_back_arrow_hidden_once_tx_seen       -> UnlockWalksEveryStepByKeys (back arrow, Escape is back, none once seen)
//   test_real_arrow_keys_move_through_the_fields, test_letters_bound_to_ui_up_and_down_are_typed_into_the_manual_hash_field
//                                             -> LettersAndArrowsInTheFields
//   test_focus_neighbours_stay_in_the_sheet   -> TheFocusStaysInTheSheetAndTheGameGetsNoKeys
//   test_qr_matches_core_matrix               -> TheQrViewDrawsTheCoreRuns
//   test_strings_are_translatable             -> TextsAreKeyedByTheSharedIds (no translations: every text by id)
//   test_a_rebuild_below_never_takes_the_focus_from_a_screen_on_top,
//   test_a_screen_closing_below_leaves_the_focus_on_the_screen_on_top -> AScreenOnTopKeepsTheFocus
//   test_restore_choose_and_confirm, test_restore_screen_shows_errors_and_closes_on_ui_cancel,
//   test_real_arrow_keys_leave_and_reenter_the_hash_field -> RestoreByKeys, RestoreErrorsAndEscape
//   Godot only: test_replacement_sheet_scene_is_bound (a PackedScene: here a custom host or UScfgSheetWidget, see
//   TheUmgWidgetShowsThePluginsSheet), test_theme_override_merges_into_the_margin_theme (a Theme: here the style
//   override, see TheStyleOverrideWins), test_the_focused_field_is_scrolled_into_view_after_a_rebuild (layout of a
//   rendered frame; the null renderer lays out but nothing scrolls visibly; FocusEntry asks the scroll box).
// Unreal only: SlateRunsHeadless, EscapeAndGamepadBack, GamepadConfirmAndStick, ToastsFadeInRealTime,
// WorksWhilePausedAndDilated, TheDefaultHostIsUsedWhenTheGameSetsNone, TheGameInstanceEndsWithASheetOpen,
// TheMouseIsTheSheetsWhileItIsOpen, TheMouseGoesBackAsTheGameWantsItNow, OtherWidgetsKeepTheirMouseAndFocus,
// ASheetTheGameRemovesCountsAsCancel.
#include "ScfgSheetTestKit.h"

#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Styling/SlateStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectArray.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgCheckoutController.h"
#include "ScfgDefaultSheetHost.h"
#include "ScfgFacade.h"
#include "ScfgSheetWidget.h"
#include "ScfgString.h"
#include "ScfgSubsystem.h"
#include "ScfgSubsystemTestAccess.h"
#include "UI/SScfgQrView.h"
#include "UI/SScfgToast.h"
#include "UI/ScfgStyle.h"
#include "UI/ScfgTexts.h"

#include "Scfg/Checkout.h"
#include "Scfg/Json.h"
#include "Scfg/Paylinks.h"
#include "Scfg/RestoreFlow.h"

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_SHEET_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Sheet." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgSheetTestsDetail
{

using namespace ScfgFacadeTests;
using namespace ScfgSheetTests;

FString TxText()
{
	return ScfgString::FromUtf8(kTx);
}

FScfgCheckout& CheckoutOf(const SScfgPaymentSheet& Sheet)
{
	return Sheet.GetController().GetCheckout().Get();
}

// Counts the UObjects created while it is listening.
class FCreations final : public FUObjectArray::FUObjectCreateListener
{
public:
	FCreations() { GUObjectArray.AddUObjectCreateListener(this); }
	virtual ~FCreations() override { Stop(); }
	void Stop()
	{
		if (bListening)
		{
			bListening = false;
			GUObjectArray.RemoveUObjectCreateListener(this);
		}
	}
	virtual void NotifyUObjectCreated(const UObjectBase* Object, int32 Index) override { ++Count; }
	virtual void OnUObjectArrayShutdown() override { Stop(); }
	int32 Count = 0;

private:
	bool bListening = true;
};

// A transfer of 5.00 USDC to the developer in an old block (test_restore_screen.gd's _round_payment): a whole-cent
// amount carries no item code, so the player says what it paid for (chapter2, whose price it covers, or a tip).
void PutRoundPayment(FWorld& W)
{
	W.Chains->Chain("arbitrum").TxReceipts[kTx] = scfg::Json::dict({{"status", "0x1"},
		{"logs", scfg::Json::list({TransferLog(UsdcArbitrum(), kPayer, kDev, 5000000, 10, kTx)})}});
}

// Every text of ScfgTexts, or a template of it, or one of Data.
bool IsKeyed(const FString& Text, const TArray<FString>& Data)
{
	if (Data.Contains(Text))
	{
		return true;
	}
	for (const TPair<FString, FString>& T : ScfgTexts::All())
	{
		if (T.Value == Text || (T.Value.Contains(TEXT("%s")) && MatchesTemplate(Text, T.Value)))
		{
			return true;
		}
	}
	return false;
}

// test_sheet.gd's _dynamic: texts that are data (amounts, addresses, names, the developer's titles).
TArray<FString> DataOf(const FScfgCheckoutController& C)
{
	TArray<FString> Out = {C.PriceText(), C.Token(), C.Title(), C.Recipient(), C.TokenAddress(), TxText(),
		C.PayAmountText() + TEXT(" ") + C.Token()};
	for (const FScfgChainOptionView& O : C.ChainOptions())
	{
		Out.Add(O.Name);
	}
	for (FString P : C.TipPresets())
	{
		Out.Add((P.EndsWith(TEXT(".00")) ? P.LeftChop(3) : P) + TEXT(" ") + C.Token());
	}
	return Out;
}

void CheckKeyed(FAutomationTestBase& Test, const SScfgScreen& Screen, const TArray<FString>& Data, const TCHAR* Where)
{
	for (const FString& Text : Screen.VisibleTexts())
	{
		Test.TestTrue(FString::Printf(TEXT("%s: \"%s\" is a ScfgTexts text (by id) or data"), Where, *Text), IsKeyed(Text, Data));
	}
}

}  // namespace ScfgSheetTestsDetail

// What runs under -nullrhi (engine-facts.md, "Slate under -nullrhi"): the Slate application with the null renderer and
// its font services, a virtual window that paints (and so ticks) its widgets, and FSlateApplication's key routing to
// the focused widget of that window.
SCFG_SHEET_TEST(FScfgSheetSlateHeadlessTest, "SlateRunsHeadless")

bool FScfgSheetSlateHeadlessTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	TestFalse(TEXT("-nullrhi: the process can never render"), FApp::CanEverRender());
	TestNotNull(TEXT("but Slate has a renderer (the null one)"), FSlateApplication::Get().GetRenderer());
	const TSharedRef<STextBlock> Text = SNew(STextBlock).Text(FText::FromString(TEXT("Restore purchases")));
	R.Layer->AddSlot(1)[Text];
	R.Frame();
	TestTrue(TEXT("fonts are measured (the null renderer's font cache)"), Text->GetDesiredSize().X > 10 && Text->GetDesiredSize().Y > 5);
	const TSharedRef<SScfgToast> Toast = SNew(SScfgToast, FScfgStyle::Create(TEXT("#0e7c74"), FString()));
	R.Layer->AddSlot(2)[Toast];
	R.Frame(1, 0.5f);
	TestEqual(TEXT("painting the virtual window ticks its widgets (real time)"), Toast->Age(), 0.5f);
	R.Layer->RemoveSlot(Toast);
	R.Layer->RemoveSlot(Text);
	TestTrue(TEXT("a key reaches the focused widget of the virtual window"), R.Key(EKeys::Q));
	TestEqual(TEXT("the game's widget got it"), FString::Join(R.Game->Got, TEXT(",")), FString(TEXT("Q")));
	return true;
}

// A tip by keys only: amount (a preset by the arrows, Enter), network, method, browser (the page opens), back to the
// method by Escape, phone wallet (the QR code), I've sent it, waiting, paid, done, Continue. The game gets no key.
SCFG_SHEET_TEST(FScfgSheetTipByKeysTest, "TipWalksEveryStepByKeys")

bool FScfgSheetTipByKeysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	FScfgFacade& S = R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the default host shows the plugin's sheet"), Sheet.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("one widget on the game viewport, above the game's"), R.NumLayered(), 1);
	const FScfgCheckoutController& C = Sheet->GetController();
	TestEqual(TEXT("a tip starts on the amount step"), Sheet->ShownStep(), FString(TEXT("amount")));
	TestTrue(TEXT("the selected preset has the focus"), R.HasFocus(*Sheet, TEXT("preset_3.00")));
	TestFalse(TEXT("no back arrow on the first step"), Sheet->IsShown(TEXT("back")));
	TestEqual(TEXT("forward: Next"), Sheet->TextOf(TEXT("forward")), ScfgTexts::Get(TEXT("next")));
	TestTrue(TEXT("the step dots"), Sheet->Dots() == C.StepDots());
	R.Key(EKeys::Right);
	TestTrue(TEXT("Right moves within the presets"), R.HasFocus(*Sheet, TEXT("preset_5.00")));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("Enter picks it: the price in the header"), Sheet->TextOf(TEXT("price")), FString(TEXT("5.00")));
	TestTrue(TEXT("Down reaches Next"), R.KeyTo(*Sheet, TEXT("forward"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the network step"), Sheet->ShownStep(), FString(TEXT("network")));
	TestTrue(TEXT("the back arrow"), Sheet->IsShown(TEXT("back")));
	TestFalse(TEXT("no forward button on the network step"), Sheet->IsShown(TEXT("forward")));
	TestTrue(TEXT("Down reaches Arbitrum"), R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Down));
	R.Key(EKeys::Enter);
	R.Pass();
	TestEqual(TEXT("the session started: the method step"), Sheet->ShownStep(), FString(TEXT("method")));
	TestTrue(TEXT("the method step has the focus"), R.FocusInside(*Sheet));
	TestTrue(TEXT("Up reaches the browser wallet"), R.KeyTo(*Sheet, TEXT("method_browser"), EKeys::Up));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the browser step"), Sheet->ShownStep(), FString(TEXT("browser")));
	TestTrue(TEXT("its forward button has the focus"), R.HasFocus(*Sheet, TEXT("forward")));
	R.Key(EKeys::Enter);
	TestTrue(TEXT("Open checkout in browser opens the localhost page"),
		R.W.Platform->Opened.Num() == 1 && R.W.Platform->Opened[0].StartsWith(TEXT("http://127.0.0.1:")));
	R.Frame(2);
	TestEqual(TEXT("and waits for it"), Sheet->ShownStep(), FString(TEXT("waiting")));
	TestTrue(TEXT("Reopen checkout page is offered"), Sheet->IsShown(TEXT("reopen")));
	for (int32 I = 0; I < 3 && Sheet->ShownStep() != TEXT("method"); ++I)
	{
		R.Key(EKeys::Escape);
	}
	TestEqual(TEXT("Escape goes back to the methods"), Sheet->ShownStep(), FString(TEXT("method")));
	TestTrue(TEXT("Down reaches the phone wallet"), R.KeyTo(*Sheet, TEXT("method_qr"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the phone wallet step"), Sheet->ShownStep(), FString(TEXT("qr")));
	TestTrue(TEXT("a QR code"), Sheet->Control(TEXT("qr")).IsValid());
	TestTrue(TEXT("the steps beside it"), Sheet->VisibleTexts().Contains(ScfgTexts::Get(TEXT("qr_step_open"))));
	TestTrue(TEXT("I've sent it has the focus"), R.HasFocus(*Sheet, TEXT("forward")));
	TestEqual(TEXT("forward: I've sent it"), Sheet->TextOf(TEXT("forward")), ScfgTexts::Get(TEXT("i_sent_it")));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the waiting step"), Sheet->ShownStep(), FString(TEXT("waiting")));
	R.W.PayOnChain(CheckoutOf(*Sheet));
	R.Frame(2);
	TestEqual(TEXT("paid: the done step"), Sheet->ShownStep(), FString(TEXT("done")));
	TestEqual(TEXT("the title"), Sheet->VisibleTexts().Contains(ScfgTexts::Get(TEXT("paid_title"))), true);
	TestTrue(TEXT("Continue has the focus"), R.HasFocus(*Sheet, TEXT("continue")));
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestTrue(TEXT("Pay resolves paid"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Paid);
	TestEqual(TEXT("the sheet left the viewport"), R.NumLayered(), 0);
	TestTrue(TEXT("the focus went back to the game"), R.Focused() == R.Game);
	TestEqual(TEXT("the game got no key while the sheet was open"), FString::Join(R.Game->Got, TEXT(",")), FString());
	TestTrue(TEXT("tipped"), S.HasTipped());
	return true;
}

// An unlock by keys: the network (its first step: no back arrow, Escape would cancel), manual transfer, Copy, the hash
// typed into its field and checked with Enter, waiting (no back arrow once the transaction is seen; Escape neither goes
// back nor closes), paid, the notification toast, done, Continue.
SCFG_SHEET_TEST(FScfgSheetUnlockByKeysTest, "UnlockWalksEveryStepByKeys")

bool FScfgSheetUnlockByKeysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	FScfgFacade& S = R.Started(*this);
	S.bNotify = true;
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	const FScfgCheckoutController& C = Sheet->GetController();
	TestEqual(TEXT("an unlock starts on the network step"), Sheet->ShownStep(), FString(TEXT("network")));
	TestFalse(TEXT("no back arrow"), Sheet->IsShown(TEXT("back")));
	TestEqual(TEXT("the item's title"), Sheet->TextOf(TEXT("title")), FString(TEXT("Chapter 2")));
	TestTrue(TEXT("Down reaches Arbitrum"), R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Down));
	R.Key(EKeys::Enter);
	R.Pass();
	TestEqual(TEXT("the method step"), Sheet->ShownStep(), FString(TEXT("method")));
	TestTrue(TEXT("Down reaches Manual transfer"), R.KeyTo(*Sheet, TEXT("method_manual"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the manual step"), Sheet->ShownStep(), FString(TEXT("manual")));
	TestTrue(TEXT("the back arrow on the manual step"), Sheet->IsShown(TEXT("back")));
	R.Key(EKeys::Escape, false, true);
	R.Key(EKeys::Gamepad_FaceButton_Right, false, true);
	TestEqual(TEXT("a key's repeats are not back (allow_echo false)"), Sheet->ShownStep(), FString(TEXT("manual")));
	R.Key(EKeys::Escape);
	for (int32 I = 0; I < 3; ++I)
	{
		R.Key(EKeys::Escape, false, true);  // Escape held past the repeat delay
	}
	TestEqual(TEXT("Escape held is back one step, not out of the checkout"), Sheet->ShownStep(), FString(TEXT("method")));
	TestTrue(TEXT("the chosen method has the focus"), R.HasFocus(*Sheet, TEXT("method_manual")));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the manual step again"), Sheet->ShownStep(), FString(TEXT("manual")));
	TestTrue(TEXT("its first Copy has the focus"), R.HasFocus(*Sheet, TEXT("copy_amount")));
	const TArray<FString> Texts = Sheet->VisibleTexts();
	TestTrue(TEXT("the address"), Texts.Contains(C.Recipient()));
	TestTrue(TEXT("the exact amount"), Texts.Contains(C.PayAmountText() + TEXT(" USDC")));
	TestTrue(TEXT("the token contract"), Texts.Contains(C.TokenAddress()));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("Enter on Copy copies the amount"), R.W.Platform->Clipboard, C.PayAmountText());
	TestTrue(TEXT("Down reaches the hash field"), R.KeyTo(*Sheet, TEXT("tx_input"), EKeys::Down));
	R.Type(TxText());
	TestEqual(TEXT("typed into the field"), C.TxInput(), TxText());
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestEqual(TEXT("Enter checks the hash: reported"), C.HintTx(), TxText());
	TestEqual(TEXT("the waiting step"), Sheet->ShownStep(), FString(TEXT("waiting")));
	TestFalse(TEXT("no back arrow once a transaction is seen"), Sheet->IsShown(TEXT("back")));
	TestTrue(TEXT("the hash is shown"), Sheet->VisibleTexts().Contains(TxText()));
	R.Key(EKeys::Escape);
	TestTrue(TEXT("Escape neither goes back nor closes now"), Sheet->ShownStep() == TEXT("waiting") && R.NumLayered() == 1);
	// the reported transaction lands: its receipt pays the session's exact amount
	const scfg::Json& Session = CheckoutOf(*Sheet).Core().session();
	const int64 Units = Session.is_dict() && Session.as_dict().get("units").is_int64() ? Session.as_dict().get("units").as_int64() : 0;
	FFakeChain& Arbitrum = R.W.Chains->Chain("arbitrum");
	Arbitrum.TxReceipts[kTx] = scfg::Json::dict({{"status", "0x1"},
		{"logs", scfg::Json::list({TransferLog(UsdcArbitrum(), kPayer, kDev, Units, Arbitrum.Head + 1, kTx)})}});
	R.W.PayOnChain(CheckoutOf(*Sheet));
	R.Frame(2);
	TestEqual(TEXT("done"), Sheet->ShownStep(), FString(TEXT("done")));
	TestTrue(TEXT("the toast says so"), R.Host->ToastTexts().Contains(
		ScfgTexts::Format(TEXT("payment_confirmed_item"), {TEXT("Chapter 2")})));
	TestTrue(TEXT("Copy hash on the done step"), Sheet->IsShown(TEXT("copy_tx")));
	TestTrue(TEXT("Continue has the focus"), R.HasFocus(*Sheet, TEXT("continue")));
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestTrue(TEXT("Pay resolves paid"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Paid && (*Box)->TxHash == TxText());
	TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
	TestEqual(TEXT("the game got no key"), FString::Join(R.Game->Got, TEXT(",")), FString());
	return true;
}

// Escape on the first step closes as "cancel", and the gamepad's back button does the same as Escape.
SCFG_SHEET_TEST(FScfgSheetEscapeTest, "EscapeAndGamepadBack")

bool FScfgSheetEscapeTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	TestTrue(TEXT("the sheet has the focus"), R.Sheet().IsValid() && R.FocusInside(*R.Sheet()));
	TestTrue(TEXT("Escape is the sheet's"), R.Key(EKeys::Escape));
	R.Frame(2);
	TestTrue(TEXT("Escape on the first step closes as Cancel"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("removed from the viewport"), R.NumLayered(), 0);
	TestTrue(TEXT("the focus is the game's again"), R.Focused() == R.Game);
	Box = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("a second sheet"), Sheet.IsValid()))
	{
		return false;
	}
	R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Gamepad_DPad_Down);
	R.Key(EKeys::Gamepad_FaceButton_Bottom);
	R.Pass();
	TestEqual(TEXT("the gamepad's confirm chose the network"), Sheet->ShownStep(), FString(TEXT("method")));
	R.Key(EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("its back button goes back"), Sheet->ShownStep(), FString(TEXT("network")));
	R.Key(EKeys::Gamepad_FaceButton_Right);
	R.Frame(2);
	TestTrue(TEXT("and on the first step closes as Cancel"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("the game got neither"), FString::Join(R.Game->Got, TEXT(",")), FString());
	return true;
}

// The gamepad: the d-pad and the left stick move the focus (one step per push), the confirm button presses.
SCFG_SHEET_TEST(FScfgSheetGamepadTest, "GamepadConfirmAndStick")

bool FScfgSheetGamepadTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("on the 3.00 preset"), R.HasFocus(*Sheet, TEXT("preset_3.00")));
	R.Key(EKeys::Gamepad_DPad_Right);
	TestTrue(TEXT("d-pad right: the next preset"), R.HasFocus(*Sheet, TEXT("preset_5.00")));
	R.Stick(EKeys::Gamepad_LeftX, -0.9f);
	TestTrue(TEXT("the stick left: back to 3.00"), R.HasFocus(*Sheet, TEXT("preset_3.00")));
	const TArray<FString> Order = Sheet->FocusOrder();
	const int32 Before = Order.IndexOfByKey(Sheet->FocusedKey());
	R.Stick(EKeys::Gamepad_LeftY, -0.9f);
	TestEqual(TEXT("the stick down: one control down"), Order.IndexOfByKey(Sheet->FocusedKey()), Before + 1);
	R.KeyTo(*Sheet, TEXT("forward"), EKeys::Gamepad_DPad_Down);
	R.Key(EKeys::Gamepad_FaceButton_Bottom);
	TestEqual(TEXT("the confirm button pressed Next"), Sheet->ShownStep(), FString(TEXT("network")));
	R.KeyTo(*Sheet, TEXT("cancel"), EKeys::Gamepad_DPad_Down);
	R.Key(EKeys::Gamepad_FaceButton_Bottom);
	R.Frame(2);
	TestTrue(TEXT("and Cancel"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("the game got no button and no stick"), FString::Join(R.Game->Got, TEXT(",")), FString());
	return true;
}

// test_focus_neighbours_stay_in_the_sheet: no key moves the focus out of the sheet, none reaches the game, and the
// focus goes back to the game's widget when the sheet closes.
SCFG_SHEET_TEST(FScfgSheetFocusStaysTest, "TheFocusStaysInTheSheetAndTheGameGetsNoKeys")

bool FScfgSheetFocusStaysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet took the focus"), Sheet.IsValid() && R.FocusInside(*Sheet)))
	{
		return false;
	}
	struct FPress
	{
		FKey Key;
		bool bShift;
	};
	const FPress Presses[] = {{EKeys::Up, false}, {EKeys::Up, false}, {EKeys::Left, false}, {EKeys::Right, false},
		{EKeys::Down, false}, {EKeys::Down, false}, {EKeys::Down, false}, {EKeys::Down, false}, {EKeys::Down, false},
		{EKeys::Down, false}, {EKeys::Down, false}, {EKeys::Down, false}, {EKeys::Tab, false}, {EKeys::Tab, false},
		{EKeys::Tab, true}, {EKeys::W, false}, {EKeys::S, false}, {EKeys::SpaceBar, false}, {EKeys::Gamepad_DPad_Up, false},
		{EKeys::Gamepad_LeftShoulder, false}, {EKeys::F, false}};
	for (const FPress& P : Presses)
	{
		R.Key(P.Key, P.bShift);
		TestTrue(FString::Printf(TEXT("%s keeps the focus in the sheet (%s)"), *P.Key.ToString(), *Sheet->FocusedKey()),
			R.FocusInside(*Sheet) && !Sheet->FocusedKey().IsEmpty());
	}
	R.Type(TEXT("w"));
	R.Stick(EKeys::Gamepad_RightY, 1.f);
	TestEqual(TEXT("the game got none of it"), FString::Join(R.Game->Got, TEXT(",")), FString());
	TestFalse(TEXT("nothing closed it"), Box->IsSet());
	TestTrue(TEXT("Tab wraps: from the last control to the first"), [&]()
	{
		const TArray<FString> Order = Sheet->FocusOrder();
		Sheet->FocusKey(Order.Last());
		R.Key(EKeys::Tab);
		return Sheet->FocusedKey() == Order[0];
	}());
	TestTrue(TEXT("Up stays on the first"), [&]()
	{
		R.Key(EKeys::Up);
		return Sheet->FocusedKey() == Sheet->FocusOrder()[0];
	}());
	TestTrue(TEXT("Press: Cancel"), Sheet->Press(TEXT("cancel")));
	R.Frame(2);
	TestTrue(TEXT("closed"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestTrue(TEXT("the focus goes back to the game once the sheet closes"), R.Focused() == R.Game);
	return true;
}

// The real arrow keys move through the tip-amount field and the manual step's hash field (left and right stay in a
// field for the caret), a field reached by them takes typing, and letters a game may bind to navigation (W, S) are
// typed, not taken.
SCFG_SHEET_TEST(FScfgSheetFieldsTest, "LettersAndArrowsInTheFields")

bool FScfgSheetFieldsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	const FScfgCheckoutController& C = Sheet->GetController();
	TestTrue(TEXT("Down reaches the amount field"), R.KeyTo(*Sheet, TEXT("custom_amount"), EKeys::Down));
	R.Type(TEXT("7"));
	TestEqual(TEXT("the field reached by the arrows takes typing"), C.CustomAmount(), FString(TEXT("7")));
	R.Type(TEXT("x,"));
	TestEqual(TEXT("only digits and the point"), C.CustomAmount(), FString(TEXT("7")));
	R.Key(EKeys::Left);
	R.Key(EKeys::Right);
	TestTrue(TEXT("left and right stay in the field"), R.HasFocus(*Sheet, TEXT("custom_amount")));
	R.Key(EKeys::Down);
	TestTrue(TEXT("Down leaves the field for Set"), R.HasFocus(*Sheet, TEXT("set_amount")));
	R.Key(EKeys::Up);
	TestTrue(TEXT("Up comes back into it"), R.HasFocus(*Sheet, TEXT("custom_amount")));
	R.Type(TEXT("5"));
	TestEqual(TEXT("and it takes typing again"), C.CustomAmount(), FString(TEXT("75")));
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestEqual(TEXT("Enter in the field sets the amount"), C.PriceText(), FString(TEXT("75.00")));
	R.Sheet()->Press(TEXT("forward"));
	R.Frame(2);
	R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Down);
	R.Key(EKeys::Enter);
	R.Pass();
	R.KeyTo(*Sheet, TEXT("method_manual"), EKeys::Down);
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the manual step"), Sheet->ShownStep(), FString(TEXT("manual")));
	TestTrue(TEXT("Down reaches the hash field"), R.KeyTo(*Sheet, TEXT("tx_input"), EKeys::Down));
	for (const FKey& K : {EKeys::A, EKeys::S, EKeys::W, EKeys::D})
	{
		FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(K, FModifierKeysState(), 0, false, 0, 0));
		R.Type(K.ToString().ToLower());
		FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(K, FModifierKeysState(), 0, false, 0, 0));
	}
	TestEqual(TEXT("W and S are typed, not taken for navigation"), C.TxInput(), FString(TEXT("aswd")));
	TestTrue(TEXT("and the focus stays in the field"), R.HasFocus(*Sheet, TEXT("tx_input")));
	R.Key(EKeys::Down);
	TestTrue(TEXT("Down leaves it for Check"), R.HasFocus(*Sheet, TEXT("check")));
	R.Key(EKeys::Up);
	R.Type(TEXT("ef"));
	TestEqual(TEXT("Up comes back into it, and it takes typing"), C.TxInput(), FString(TEXT("aswdef")));
	R.Key(EKeys::Up);
	TestTrue(TEXT("Up leaves it for Copy"), R.HasFocus(*Sheet, TEXT("copy_token")));
	TestEqual(TEXT("the game got nothing"), FString::Join(R.Game->Got, TEXT(",")), FString());
	return true;
}

// test_qr_matches_core_matrix: the QR view draws exactly the core's runs (qr_rects), one solid box each, at the largest
// integer scale that fits; on a narrow screen the sheet takes the whole width and the code still fits it.
SCFG_SHEET_TEST(FScfgSheetQrTest, "TheQrViewDrawsTheCoreRuns")

bool FScfgSheetQrTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	int32 WideScale = 0;
	for (const FVector2D Size : {FVector2D(1280, 720), FVector2D(240, 640)})
	{
		const bool bNarrow = Size.X < 400;
		FSheetRig R(*this, Size);
		if (!R.bReady)
		{
			return false;
		}
		R.Started(*this);
		R.W.Pay(EScfgPayKind::Unlock, Unlock());
		R.Frame(2);
		const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
		if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
		{
			return false;
		}
		Sheet->Press(TEXT("network_arbitrum"));
		R.Pass();
		Sheet->Press(TEXT("method_qr"));
		R.Frame(2);
		TestEqual(TEXT("the phone wallet step"), Sheet->ShownStep(), FString(TEXT("qr")));
		const TSharedPtr<SScfgQrView> Qr = StaticCastSharedPtr<SScfgQrView>(Sheet->Control(TEXT("qr")));
		if (!TestTrue(TEXT("a QR view"), Qr.IsValid()))
		{
			return false;
		}
		const FString Uri = Sheet->GetController().PaymentUri();
		const scfg::Result<scfg::paylinks::QrRects> Rects = scfg::paylinks::qr_rects(ScfgString::ToUtf8(Uri), 2);
		if (!TestTrue(TEXT("the core encodes the payment link"), Rects.ok()))
		{
			return false;
		}
		const int32 Modules = int32(Rects.value().size);
		const int32 Runs = int32(Rects.value().runs.size());
		TestEqual(TEXT("modules per side, the quiet zone included"), Qr->Modules(), Modules);
		TestEqual(TEXT("one run each"), Qr->NumRuns(), Runs);
		TestEqual(TEXT("the last paint drew one box per run"), Qr->LastBoxes(), Runs);
		const int32 Scale = Qr->LastScale();
		TestTrue(TEXT("an integer scale of at least 1"), Scale >= 1);
		if (!bNarrow)
		{
			WideScale = Scale;
			TestEqual(TEXT("176 units: the largest integer scale that fits"), Scale, FMath::Max(1, 176 / Modules));
		}
		else
		{
			TestEqual(TEXT("a narrow sheet: the whole width, the code what it leaves"), Qr->GetTargetSize(), 240.f - 88.f - 16.f);
			TestEqual(TEXT("at the largest scale that fits that"), Scale, FMath::Max(1, (240 - 88 - 16) / Modules));
			TestTrue(TEXT("smaller than on the wide screen: the QR scales with the sheet"), Scale < WideScale);
		}
		const TArray<FIntRect> Dark = Qr->DarkRects(Scale);
		bool bSame = Dark.Num() == Runs;
		for (int32 I = 0; bSame && I < Runs; ++I)
		{
			const scfg::paylinks::QrRun& Run = Rects.value().runs[size_t(I)];
			bSame = Dark[I] == FIntRect(int32(Run.x) * Scale, int32(Run.y) * Scale, int32(Run.x + Run.width) * Scale,
				int32(Run.y + 1) * Scale);
		}
		TestTrue(TEXT("every box is its run at that scale"), bSame);
		TestTrue(TEXT("the code fits its place"), Modules * Scale <= FMath::Max(int32(Qr->GetTargetSize()), Modules));
	}
	const TSharedRef<SScfgQrView> TooLong = SNew(SScfgQrView).Text(FString::ChrN(8000, TEXT('a'))).TargetSize(176);
	TestTrue(TEXT("a text no version holds: nothing shown, the core's error"), TooLong->Modules() == 0 && TooLong->NumRuns() == 0 &&
		!TooLong->GetError().IsEmpty());
	return true;
}

// The texts: every messages.json id and English (the vectors file itself), the core's checkout and restore messages
// by id, and every text any step and the restore screen show is ScfgTexts' (by id, a template filled) or data.
SCFG_SHEET_TEST(FScfgSheetTextsTest, "TextsAreKeyedByTheSharedIds")

bool FScfgSheetTextsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	const FString Vectors = MessagesJson();
	if (!TestFalse(TEXT("conformance/vectors/messages.json is found"), Vectors.IsEmpty()))
	{
		return false;
	}
	const scfg::Result<scfg::Json> Loaded = scfg::json::loads(ScfgString::ToUtf8(Vectors));
	if (!TestTrue(TEXT("and is JSON"), Loaded.ok() && Loaded.value().is_dict()))
	{
		return false;
	}
	const scfg::Json Messages = Loaded.value().as_dict().get("messages");
	int32 Ids = 0;
	for (const auto& [Id, English] : Messages.as_dict())
	{
		++Ids;
		TestEqual(FString::Printf(TEXT("ScfgTexts %s is messages.json's"), *ScfgString::FromUtf8(Id)),
			ScfgTexts::Get(ScfgString::FromUtf8(Id)), ScfgString::FromUtf8(English.as_str()));
		TestEqual(TEXT("and its message"), ScfgTexts::Message(ScfgString::FromUtf8(Id)), ScfgString::FromUtf8(English.as_str()));
	}
	TestEqual(TEXT("every id of the core's checkout MESSAGES"), Ids, int32(scfg::checkout::MESSAGES().size()));
	for (const auto& [Code, English] : scfg::restore_flow::MESSAGES())
	{
		TestEqual(FString::Printf(TEXT("restore_%s"), *ScfgString::FromUtf8(Code)),
			ScfgTexts::RestoreMessage(ScfgString::FromUtf8(Code)), ScfgString::FromUtf8(English));
	}
	TestEqual(TEXT("an id nobody defines shows as itself"), ScfgTexts::Get(TEXT("no_such_id")), FString(TEXT("no_such_id")));
	TestEqual(TEXT("%s in order, %% a percent sign"), ScfgTexts::Format(TEXT("confirming"), {TEXT("Base"), TEXT("3"), TEXT("20")}),
		FString(TEXT("Confirming on Base... 3/20")));

	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	const FScfgCheckoutController& C = Sheet->GetController();
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("amount"));
	CheckoutOf(*Sheet).Core().error = "invalid_amount";
	R.Frame(2);
	TestTrue(TEXT("a core message is shown by its id"), Sheet->VisibleTexts().Contains(ScfgTexts::Get(TEXT("invalid_amount"))));
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("amount with an error"));
	CheckoutOf(*Sheet).Core().error.reset();
	Sheet->Press(TEXT("forward"));
	R.Frame(2);
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("network"));
	Sheet->Press(TEXT("network_arbitrum"));
	R.Pass();
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("method"));
	for (const TCHAR* Method : {TEXT("browser"), TEXT("qr"), TEXT("manual")})
	{
		Sheet->Press(FString(TEXT("method_")) + Method);
		R.Frame(2);
		TestEqual(TEXT("the method's step"), Sheet->ShownStep(), FString(Method));
		CheckKeyed(*this, *Sheet, DataOf(C), Method);
		Sheet->Press(TEXT("back"));
		R.Frame(2);
	}
	Sheet->Press(TEXT("method_manual"));
	R.Frame(2);
	Sheet->Press(TEXT("forward"));
	R.Frame(2);
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("waiting"));
	CheckoutOf(*Sheet).Core().hint_tx = kTx;
	R.Frame(2);
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("waiting with a hash"));
	CheckoutOf(*Sheet).Core().hint_tx.reset();
	R.W.PayOnChain(CheckoutOf(*Sheet));
	R.Frame(2);
	TestEqual(TEXT("the done step"), Sheet->ShownStep(), FString(TEXT("done")));
	CheckKeyed(*this, *Sheet, DataOf(C), TEXT("done"));
	Sheet->Press(TEXT("continue"));
	R.Frame(2);
	R.W.Restore();
	R.Frame(2);
	const TSharedPtr<SScfgRestoreScreen> Screen = R.RestoreScreen();
	if (!TestTrue(TEXT("the restore screen"), Screen.IsValid()))
	{
		return false;
	}
	TArray<FString> Data = {TxText()};
	for (const FScfgReceiptLine& L : Screen->GetController().Receipts())
	{
		Data.Add(L.TxHash);
	}
	CheckKeyed(*this, *Screen, Data, TEXT("restore"));
	Screen->GetController().GetRestore()->Core().error = "tx_not_found";
	R.Frame(2);
	TestTrue(TEXT("a restore message is shown by its restore_ id"),
		Screen->VisibleTexts().Contains(ScfgTexts::Get(TEXT("restore_tx_not_found"))));
	CheckKeyed(*this, *Screen, Data, TEXT("restore with an error"));
	return true;
}

// test_restore_choose_and_confirm by keys: the hash typed, Check, the payment to name (Restore waits for a choice),
// Chapter 2 chosen, Restore, done, the lists, Close.
SCFG_SHEET_TEST(FScfgSheetRestoreByKeysTest, "RestoreByKeys")

bool FScfgSheetRestoreByKeysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	FScfgFacade& S = R.Started(*this);
	PutRoundPayment(R.W);
	const TSharedPtr<TArray<FScfgRestoreResult>> Called = MakeShared<TArray<FScfgRestoreResult>>();
	const TSharedRef<TOptional<FScfgRestoreResult>> Box = R.W.Restore(Called);
	R.Frame(2);
	const TSharedPtr<SScfgRestoreScreen> Screen = R.RestoreScreen();
	if (!TestTrue(TEXT("the default host shows the plugin's restore screen"), Screen.IsValid()))
	{
		return false;
	}
	const FScfgRestoreController& C = Screen->GetController();
	TestTrue(TEXT("the title"), Screen->VisibleTexts().Contains(ScfgTexts::Get(TEXT("restore_purchases"))));
	TestTrue(TEXT("the hash field has the focus"), R.HasFocus(*Screen, TEXT("tx_input")));
	R.Type(TxText());
	TestEqual(TEXT("typed into the field"), C.TxInput(), TxText());
	TestTrue(TEXT("Down reaches Check"), R.KeyTo(*Screen, TEXT("check"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("Check looks it up"), C.State(), FString(TEXT("checking")));
	TestTrue(TEXT("and says so"), Screen->VisibleTexts().Contains(ScfgTexts::Get(TEXT("checking_blockchain"))));
	R.Pass();
	TestEqual(TEXT("one candidate to choose for"), C.State(), FString(TEXT("choose")));
	TestTrue(TEXT("the candidate is described"), Screen->VisibleTexts().ContainsByPredicate([](const FString& T)
	{
		return T.StartsWith(TEXT("Payment of 5.000000 USDC on ")) && T.EndsWith(TEXT(". What was it for?"));
	}));
	TestFalse(TEXT("Restore waits for a choice"), Screen->Press(TEXT("restore")));
	TestTrue(TEXT("Keep as a tip is offered"), Screen->IsShown(TEXT("choice_0_tip")));
	TestTrue(TEXT("Chapter 2 has the focus"), R.HasFocus(*Screen, TEXT("choice_0_chapter2")));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("chosen"), C.Choice(0), FString(TEXT("chapter2")));
	TestTrue(TEXT("Down reaches Restore"), R.KeyTo(*Screen, TEXT("restore"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("Restore saves"), C.State(), FString(TEXT("saving")));
	R.Pass();
	TestEqual(TEXT("restored"), C.State(), FString(TEXT("done")));
	TestEqual(TEXT("the callback got the result"), Called->Num(), 1);
	TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
	const TArray<FString> Texts = Screen->VisibleTexts();
	TestTrue(TEXT("the restored and owned lists"), Texts.Contains(ScfgTexts::Get(TEXT("restored"))) &&
		Texts.Contains(ScfgTexts::Get(TEXT("purchases_on_device"))));
	TestTrue(TEXT("a receipt line"), Texts.Contains(TEXT("Chapter 2: 5.00 USDC on Arbitrum One")) ||
		Texts.ContainsByPredicate([](const FString& T) { return T.StartsWith(TEXT("Chapter 2: ")); }));
	TestTrue(TEXT("with a Copy hash button per receipt"), Screen->IsShown(TEXT("copy_0")));
	TestTrue(TEXT("Close has the focus"), R.HasFocus(*Screen, TEXT("close")));
	R.Key(EKeys::Up);
	R.Key(EKeys::Enter);
	TestEqual(TEXT("Copy hash copies it"), R.W.Platform->Clipboard, TxText());
	R.KeyTo(*Screen, TEXT("close"), EKeys::Down);
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestTrue(TEXT("Restore resolves restored"), Box->IsSet() && (*Box)->Status == EScfgRestoreStatus::Restored &&
		(*Box)->Restored.Num() == 1);
	TestEqual(TEXT("the screen left the viewport"), R.NumLayered(), 0);
	TestTrue(TEXT("the focus is the game's again"), R.Focused() == R.Game);
	return true;
}

// test_restore_screen_shows_errors_and_closes_on_ui_cancel and test_real_arrow_keys_leave_and_reenter_the_hash_field.
SCFG_SHEET_TEST(FScfgSheetRestoreErrorsTest, "RestoreErrorsAndEscape")

bool FScfgSheetRestoreErrorsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	const TSharedRef<TOptional<FScfgRestoreResult>> Box = R.W.Restore();
	R.Frame(2);
	const TSharedPtr<SScfgRestoreScreen> Screen = R.RestoreScreen();
	if (!TestTrue(TEXT("the restore screen"), Screen.IsValid()))
	{
		return false;
	}
	const FScfgRestoreController& C = Screen->GetController();
	R.Type(TEXT("ab"));
	TestTrue(TEXT("Down leaves the field for Check"), R.KeyTo(*Screen, TEXT("check"), EKeys::Down, 1));
	R.Key(EKeys::Down);
	TestTrue(TEXT("and goes on to Close"), R.HasFocus(*Screen, TEXT("close")));
	TestTrue(TEXT("Up comes back into the field"), R.KeyTo(*Screen, TEXT("tx_input"), EKeys::Up, 2));
	R.Type(TEXT("cd"));
	TestEqual(TEXT("which takes typing again"), C.TxInput(), FString(TEXT("abcd")));
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestTrue(TEXT("not a hash"), C.State() == TEXT("input") && C.MessageId() == TEXT("invalid_hash"));
	TestTrue(TEXT("the message is shown"), Screen->VisibleTexts().Contains(ScfgTexts::Get(TEXT("restore_invalid_hash"))));
	TestFalse(TEXT("no Paste button (the field takes Ctrl+V)"), Screen->Control(TEXT("paste")).IsValid());
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("Escape closes the restore screen"), Box->IsSet() && (*Box)->Status == EScfgRestoreStatus::Cancelled);
	return true;
}

// test_a_rebuild_below_never_takes_the_focus_from_a_screen_on_top and test_a_screen_closing_below_leaves_the_focus...:
// the restore screen opens over the payment sheet and takes the focus; a rebuild below leaves it there; Escape closes
// the screen on top only, and the focus goes back to the sheet; a sheet closing below leaves the focus on top, and the
// screen on top then gives it back to the game.
SCFG_SHEET_TEST(FScfgSheetStackedTest, "AScreenOnTopKeepsTheFocus")

bool FScfgSheetStackedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	FScfgFacade& S = R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Paying = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	TSharedRef<TOptional<FScfgRestoreResult>> Restoring = R.W.Restore();
	R.Frame(2);
	TSharedPtr<SScfgRestoreScreen> Top = R.RestoreScreen();
	if (!TestTrue(TEXT("both screens"), Sheet.IsValid() && Top.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the screen on top has the focus"), R.HasFocus(*Top, TEXT("tx_input")));
	TestTrue(TEXT("the payment moves on below"), CheckoutOf(*Sheet).Core().choose_network("arbitrum").ok());
	R.Pass();
	TestEqual(TEXT("the sheet rebuilt for the method step"), Sheet->ShownStep(), FString(TEXT("method")));
	TestTrue(TEXT("the focus stayed on the screen on top"), R.HasFocus(*Top, TEXT("tx_input")));
	R.Type(TEXT("ab"));
	TestEqual(TEXT("and typing still goes there"), Top->GetController().TxInput(), FString(TEXT("ab")));
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("Escape closes the screen on top only"), Restoring->IsSet() && !Paying->IsSet() && S.NumCheckouts() == 1);
	TestTrue(TEXT("the focus is back on the sheet"), R.FocusInside(*Sheet));
	Restoring = R.W.Restore();
	R.Frame(2);
	Top = R.RestoreScreen();
	if (!TestTrue(TEXT("the restore screen again"), Top.IsValid()))
	{
		return false;
	}
	Sheet->CloseWith(TEXT("cancel"));
	R.Frame(3);
	TestTrue(TEXT("the payment below was closed"), Paying->IsSet() && (*Paying)->Status == EScfgPaymentStatus::Cancelled);
	TestTrue(TEXT("the focus stayed on the screen on top"), R.HasFocus(*Top, TEXT("tx_input")));
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("the screen on top closes"), Restoring->IsSet());
	TestTrue(TEXT("then the game's widget has the focus again"), R.Focused() == R.Game);
	return true;
}

// The toast of a notification: on the viewport one Z-order above the sheets, never focused, gone after 3.3 s of real
// time (Slate's delta), removed on the next core-ticker frame.
SCFG_SHEET_TEST(FScfgSheetToastTest, "ToastsFadeInRealTime")

bool FScfgSheetToastTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Host->Notify(TEXT("Purchases restored."));
	R.Frame();
	TestEqual(TEXT("a toast on the viewport"), R.NumLayered(), 1);
	TestEqual(TEXT("its text"), R.Host->ToastTexts(), TArray<FString>{TEXT("Purchases restored.")});
	TestTrue(TEXT("the game keeps the focus"), R.Focused() == R.Game);
	R.Frame(3, 1.f);
	TestEqual(TEXT("still there after 3 s"), R.Host->NumToasts(), 1);
	R.Frame(1, 0.4f);
	FTSTicker::GetCoreTicker().Tick(0.f);
	TestEqual(TEXT("faded out and removed"), R.Host->NumToasts(), 0);
	TestEqual(TEXT("from the viewport too"), R.NumLayered(), 0);
	return true;
}

// The game paused (UGameplayStatics::SetGamePaused) and its time dilation at 0: the sheet still rebuilds, takes keys,
// and the toast still fades (nothing of the screens reads the world's time).
SCFG_SHEET_TEST(FScfgSheetPausedTest, "WorksWhilePausedAndDilated")

bool FScfgSheetPausedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	UWorld* World = R.Instance->GetWorld();
	if (!TestNotNull(TEXT("the game instance's world"), World))
	{
		return false;
	}
	World->SetGameMode(FURL());
	ULocalPlayer* Player = NewObject<ULocalPlayer>(GEngine, ULocalPlayer::StaticClass());
	R.Instance->AddLocalPlayer(Player, FPlatformUserId::CreateFromInternalId(0));
	APlayerController* PC = World->SpawnActor<APlayerController>();
	if (!TestTrue(TEXT("a game mode and a player controller"), World->GetAuthGameMode() != nullptr && PC != nullptr))
	{
		return false;
	}
	PC->SetPlayer(Player);
	PC->InitPlayerState();
	TestTrue(TEXT("SetGamePaused"), UGameplayStatics::SetGamePaused(World, true));
	TestTrue(TEXT("the game is paused"), UGameplayStatics::IsGamePaused(World) && World->IsPaused());
	World->GetWorldSettings()->TimeDilation = 0.f;
	TestEqual(TEXT("and dilated to 0"), World->GetWorldSettings()->TimeDilation, 0.f);

	R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet shows while paused"), Sheet.IsValid()))
	{
		return false;
	}
	R.KeyTo(*Sheet, TEXT("forward"), EKeys::Down);
	R.Key(EKeys::Enter);
	TestEqual(TEXT("keys work while paused"), Sheet->ShownStep(), FString(TEXT("network")));
	R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Down);
	R.Key(EKeys::Enter);
	R.Pass();
	TestEqual(TEXT("the pump and the rebuild run while paused"), Sheet->ShownStep(), FString(TEXT("method")));
	R.Host->Notify(TEXT("Purchases restored."));
	R.Frame(4, 1.f);
	FTSTicker::GetCoreTicker().Tick(0.f);
	TestEqual(TEXT("the toast faded in real time"), R.Host->NumToasts(), 0);
	for (int32 I = 0; I < 4 && !Box->IsSet(); ++I)
	{
		R.Key(EKeys::Escape);
		R.Frame(2);
	}
	TestTrue(TEXT("Escape went back to the first step and closed it"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	UGameplayStatics::SetGamePaused(World, false);
	World->GetWorldSettings()->TimeDilation = 1.f;
	return true;
}

// UScfgSubsystem gives its facade the default Slate host on its own game instance; GetSheetHost() is null then; a
// game host replaces it (an open screen still closes through the host that opened it); null is the default again; and
// a game instance without a viewport shows nothing ("sheet.unavailable", one warning).
SCFG_SHEET_TEST(FScfgSheetDefaultHostTest, "TheDefaultHostIsUsedWhenTheGameSetsNone")

bool FScfgSheetDefaultHostTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	UScfgSubsystem* Scfg = R.Instance->GetSubsystem<UScfgSubsystem>();
	if (!TestNotNull(TEXT("the subsystem"), Scfg))
	{
		return false;
	}
	TestTrue(TEXT("Initialize gave the facade the default host"), Scfg->GetFacade() && Scfg->GetFacade()->DefaultSheetHost.IsValid());
	TestFalse(TEXT("and no game host"), Scfg->GetSheetHost().IsValid());
	FScfgSubsystemTestAccess::Start(*Scfg, R.W.Options());  // the fakes from here on
	R.W.Facade = Scfg->GetFacade()->AsShared();
	FScfgOptions Options;
	Options.Wallet = UTF8_TO_TCHAR(kDev);
	TestEqual(TEXT("configure"), Scfg->Configure(Options), FString());
	R.W.Start();
	TOptional<FScfgPaymentResult> First;
	Scfg->PayAsync(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), [&First](const FScfgPaymentResult& Res) { First = Res; });
	R.Frame(2);
	TestEqual(TEXT("without SetSheetHost the plugin's sheet is on the game viewport"), R.NumLayered(), 1);
	TestTrue(TEXT("a payment sheet"), R.Layer.IsValid() && [&]()
	{
		FChildren* Children = R.Layer->GetChildren();
		for (int32 I = 0; I < Children->Num(); ++I)
		{
			if (Children->GetChildAt(I)->GetType() == FName(TEXT("SScfgPaymentSheet")))
			{
				return true;
			}
		}
		return false;
	}());
	Scfg->SetSheetHost(R.W.Host);
	TestTrue(TEXT("a game host is the game's"), Scfg->GetSheetHost() == R.W.Host);
	TOptional<FScfgPaymentResult> Second;
	Scfg->PayAsync(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), [&Second](const FScfgPaymentResult& Res) { Second = Res; });
	TestEqual(TEXT("the next sheet goes to it"), R.W.Host->Sheets.Num(), 1);
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("the first sheet still closes through the default host"), First.IsSet() && R.NumLayered() == 0);
	R.W.Host->CloseSheet(-1, TEXT("cancel"));
	R.Frame();
	TestTrue(TEXT("and the second through the game's"), Second.IsSet() && R.W.Host->Closes == 1);
	Scfg->SetSheetHost(nullptr);
	TOptional<FScfgPaymentResult> Third;
	Scfg->PayAsync(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), [&Third](const FScfgPaymentResult& Res) { Third = Res; });
	R.Frame(2);
	TestEqual(TEXT("null: the default host again"), R.NumLayered(), 1);
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("closed"), Third.IsSet());
	R.W.Facade.Reset();
	R.W.Transport = nullptr;

	// a game instance without a viewport
	FWorld Bare;
	FScfgFacade& S = Bare.Started(*this);
	TStrongObjectPtr<UGameInstance> Headless(NewObject<UGameInstance>(GEngine));
	S.SheetHost = nullptr;
	S.DefaultSheetHost = MakeShared<FScfgDefaultSheetHost>(Headless.Get());
	AddExpectedMessagePlain(TEXT("scfg: the payment sheet needs a game viewport"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	const TSharedRef<TOptional<FScfgPaymentResult>> Unavailable = Bare.Pay(EScfgPayKind::Unlock, Unlock());
	const TSharedRef<TOptional<FScfgRestoreResult>> NoRestore = Bare.Restore();
	TestTrue(TEXT("no viewport: sheet.unavailable"), Unavailable->IsSet() && (*Unavailable)->Status == EScfgPaymentStatus::Failed &&
		(*Unavailable)->Error == TEXT("sheet.unavailable"));
	TestTrue(TEXT("for the restore screen too"), NoRestore->IsSet() && (*NoRestore)->Error == TEXT("sheet.unavailable"));
	Bare.Drop();
	return true;
}

// The game instance ends with a sheet open, both ways: Shutdown (Deinitialize), and collected without it (the
// subsystem's BeginDestroy, inside garbage collection). Either way the sheet leaves the viewport, its widget is
// released, and closing it created no UObject.
SCFG_SHEET_TEST(FScfgSheetGameEndsTest, "TheGameInstanceEndsWithASheetOpen")

bool FScfgSheetGameEndsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	for (const bool bCollected : {false, true})
	{
		FSheetRig R(*this);
		if (!R.bReady)
		{
			return false;
		}
		UScfgSubsystem* Scfg = R.Instance->GetSubsystem<UScfgSubsystem>();
		if (!TestNotNull(TEXT("the subsystem"), Scfg))
		{
			return false;
		}
		FScfgSubsystemTestAccess::Start(*Scfg, R.W.Options());
		R.W.Facade = Scfg->GetFacade()->AsShared();
		FScfgOptions Options;
		Options.Wallet = UTF8_TO_TCHAR(kDev);
		Scfg->Configure(Options);
		R.W.Start();
		TOptional<FScfgPaymentResult> Result;
		Scfg->PayAsync(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")), [&Result](const FScfgPaymentResult& Res) { Result = Res; });
		R.Frame(2);
		TWeakPtr<SScfgScreen> Weak;
		if (const TArray<TSharedRef<SScfgScreen>> Open = SScfgScreen::OpenScreens(); Open.Num() > 0)
		{
			Weak = Open.Last();
		}
		TestEqual(TEXT("the sheet is on the viewport"), R.NumLayered(), 1);
		TestTrue(TEXT("and shown"), Weak.IsValid());
		UWorld* World = R.Instance->GetWorld();
		int32 Created = 0;
		if (!bCollected)
		{
			{
				FCreations During;
				Scfg->Deinitialize();  // what the instance's Shutdown runs for it
				Created = During.Count;
			}
			R.Instance->Shutdown();
			R.bInstanceShutDown = true;
		}
		else
		{
			// the engine says so for an instance collected without Shutdown (the case under test)
			AddExpectedMessagePlain(TEXT("A GameInstanceSubsystem collection was garbage collected without being deinitialized"),
				ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
			// the world context goes first (it references the instance); the instance is then only ours
			R.Instance->GetWorldContext()->GameViewport = nullptr;
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			R.Instance.Reset();
			FCreations During;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			Created = During.Count;
		}
		const TCHAR* Way = bCollected ? TEXT("collected") : TEXT("Shutdown");
		TestEqual(FString::Printf(TEXT("%s: the sheet left the viewport"), Way), R.NumLayered(), 0);
		TestEqual(FString::Printf(TEXT("%s: closing it created no UObject"), Way), Created, 0);
		TestTrue(FString::Printf(TEXT("%s: the call resolved, nothing delivered"), Way), Result.IsSet());
		R.W.Facade.Reset();
		R.W.Transport = nullptr;
		TestFalse(FString::Printf(TEXT("%s: its widget is released"), Way), Weak.IsValid());
		TestEqual(FString::Printf(TEXT("%s: no screen left open"), Way), SScfgScreen::OpenScreens().Num(), 0);
	}
	return true;
}

// A UMG host: UScfgSheetWidget shows the plugin's sheet (RebuildWidget returns it), takes the focus when constructed,
// and its host's Close (RemoveFromParent) creates no UObject.
SCFG_SHEET_TEST(FScfgSheetUmgTest, "TheUmgWidgetShowsThePluginsSheet")

namespace ScfgSheetTestsDetail
{

class FUmgHost final : public IScfgSheetHost
{
public:
	FSheetRig* Rig = nullptr;
	TStrongObjectPtr<UScfgSheetWidget> Widget;
	TSharedPtr<SWidget> Slate;
	int32 CreatedInClose = -1;

	virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override
	{
		Widget.Reset(CreateWidget<UScfgSheetWidget>(Rig->Instance.Get()));
		const TSharedRef<FScfgScreen> Screen = Widget->BindPayment(Checkout);
		Add();
		return Screen;
	}
	virtual void Close(const TSharedRef<FScfgScreen>& Screen) override
	{
		FCreations During;
		Remove();
		CreatedInClose = During.Count;
	}
	// A UMG host moving the widget: off its parent (its Slate wrapper destroyed), then added again.
	void Remove()
	{
		if (Slate.IsValid())
		{
			Rig->Layer->RemoveSlot(Slate.ToSharedRef());
		}
		Widget->RemoveFromParent();
		Slate.Reset();
	}
	void Add()
	{
		Slate = Widget->TakeWidget();
		Rig->Layer->AddSlot(FScfgDefaultSheetHost::ZORDER)[Slate.ToSharedRef()];
	}
};

}  // namespace ScfgSheetTestsDetail

bool FScfgSheetUmgTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	FScfgFacade& S = R.Started(*this);
	const TSharedRef<FUmgHost> Host = MakeShared<FUmgHost>();
	Host->Rig = &R;
	S.SheetHost = Host;
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgScreen> Screen = Host->Widget.IsValid() ? Host->Widget->GetSlateScreen() : nullptr;
	if (!TestTrue(TEXT("the widget holds the plugin's payment sheet"), Screen.IsValid() &&
			Screen->GetType() == FName(TEXT("SScfgPaymentSheet"))))
	{
		return false;
	}
	TestTrue(TEXT("constructed: it took the focus"), R.FocusInside(*Screen));
	Host->Remove();
	R.Frame(2);
	TestTrue(TEXT("removed: the focus went back to the game"), R.Focused() == R.Game);
	TestFalse(TEXT("and the flow still waits"), Box->IsSet());
	Host->Add();
	R.Frame(2);
	TestTrue(TEXT("added again: the same screen"), Host->Widget->GetSlateScreen() == Screen);
	TestTrue(TEXT("in the new Slate wrapper"), Screen->GetParentWidget().IsValid());
	TestTrue(TEXT("shown: in the open screens"), SScfgScreen::OpenScreens().Contains(Screen.ToSharedRef()));
	TestTrue(TEXT("with the focus"), R.FocusInside(*Screen));
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("Escape cancels through it"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("Close created no UObject"), Host->CreatedInClose, 0);
	FTSTicker::GetCoreTicker().Tick(0.f);  // the widget's check: off every window (UMG may still hold its wrapper)
	TestFalse(TEXT("hidden once it left the window"), Screen->IsOpen());
	TestTrue(TEXT("the focus is the game's again"), R.Focused() == R.Game);

	// the facade's teardown with a UMG-hosted sheet open (the subsystem's Deinitialize / BeginDestroy path; a real
	// collection cannot reach it: the widget's Slate wrapper references it, and it the game instance, its outer)
	Host->CreatedInClose = -1;
	const TSharedRef<TOptional<FScfgPaymentResult>> Second = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	TestTrue(TEXT("a second sheet"), Host->Widget.IsValid() && Host->Slate.IsValid());
	R.W.Drop();
	TestTrue(TEXT("teardown: resolved"), Second->IsSet());
	TestEqual(TEXT("teardown: Close created no UObject"), Host->CreatedInClose, 0);
	TestTrue(TEXT("teardown: off the viewport"), !Host->Slate.IsValid() && R.NumLayered() == 0);
	return true;
}

// Ruling R2-15 (I1): a game viewport that captured the mouse (the game hides the cursor) keeps neither the clicks
// nor the cursor while the sheet is open: the sheet releases the capture on show, shows the default cursor, takes
// the clicks, and gives the capture back to its holder on close.
SCFG_SHEET_TEST(FScfgSheetMouseTest, "TheMouseIsTheSheetsWhileItIsOpen")

bool FScfgSheetMouseTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	R.GameCapturesTheMouse();
	if (!TestTrue(TEXT("the game's widget has the mouse capture"), R.MouseCaptor() == R.Game))
	{
		return false;
	}
	R.Click(R.Game.ToSharedRef());
	TestEqual(TEXT("before the sheet a click is the game's"), FString::Join(R.Game->Got, TEXT(",")),
		FString(TEXT("mouse down,mouse up")));
	R.Game->Got.Reset();
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("the sheet released the game's capture"), R.MouseCaptor() == R.Game);
	TestTrue(TEXT("and remembers who had it"), Sheet->HeldMouseCaptor() == R.Game);
	TestEqual(TEXT("the default cursor over the sheet"),
		int32(Sheet->OnCursorQuery(Sheet->GetTickSpaceGeometry(), FPointerEvent()).GetCursorType()), int32(EMouseCursor::Default));
	R.Click(Sheet->Control(TEXT("preset_5.00")).ToSharedRef());
	TestEqual(TEXT("a click on a preset is the sheet's"), Sheet->TextOf(TEXT("price")), FString(TEXT("5.00")));

	// The window activated again (Alt+Tab back from the wallet): the engine gives the viewport the focus and the
	// capture, as FSceneViewport::OnViewportActivated's AcquireFocusAndCapture does (a SetInputMode(GameOnly) too).
	auto Activate = [&R]()
	{
		FWidgetPath Path;
		FSlateApplication& App = FSlateApplication::Get();
		App.FindPathToWidget(R.Game.ToSharedRef(), Path);
		App.ProcessReply(Path, FReply::Handled().SetUserFocus(R.Game.ToSharedRef(), EFocusCause::WindowActivate)
			.CaptureMouse(R.Game.ToSharedRef()), nullptr, nullptr, 0);
	};
	Activate();
	TestTrue(TEXT("activation: the game took the focus and the mouse"), R.Focused() == R.Game && R.MouseCaptor() == R.Game);
	R.Frame();
	TestTrue(TEXT("within one tick the sheet has the mouse again"), R.MouseCaptor() != R.Game);
	TestTrue(TEXT("and the keyboard focus"), R.FocusInside(*Sheet));
	R.Click(Sheet->Control(TEXT("preset_10.00")).ToSharedRef());
	TestEqual(TEXT("and the clicks"), Sheet->TextOf(TEXT("price")), FString(TEXT("10.00")));
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("a key is the sheet's too (Escape on the first step closes it)"), Box->IsSet());
	TestTrue(TEXT("closing gave the capture back to the game"), R.MouseCaptor() == R.Game);
	R.Game->Got.Reset();

	// two screens stacked: only the one on top takes the focus back
	const TSharedRef<TOptional<FScfgPaymentResult>> Below = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedRef<TOptional<FScfgRestoreResult>> Above = R.W.Restore();
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Lower = R.Sheet();
	const TSharedPtr<SScfgRestoreScreen> Top = R.RestoreScreen();
	if (!TestTrue(TEXT("both screens"), Lower.IsValid() && Top.IsValid()))
	{
		return false;
	}
	Activate();
	R.Frame(3);
	TestTrue(TEXT("stacked: the top screen took the focus back"), R.HasFocus(*Top, TEXT("tx_input")));
	TestFalse(TEXT("and the one below did not fight it"), R.FocusInside(*Lower));
	TestTrue(TEXT("and the mouse is no one's"), R.MouseCaptor() != R.Game);
	R.Key(EKeys::Escape);
	R.Frame(2);
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("both closed"), Above->IsSet() && Below->IsSet());
	TestTrue(TEXT("the capture is the game's again"), R.MouseCaptor() == R.Game);
	TestEqual(TEXT("the game got no click or key while a screen was open"), FString::Join(R.Game->Got, TEXT(",")), FString());
	return true;
}

// Ruling R2-15 follow-up (N1, N2): the mouse goes back as the game wants it at the close, not as it was at the open,
// and a cursor lock without a capture is given back too.
SCFG_SHEET_TEST(FScfgSheetMouseWishTest, "TheMouseGoesBackAsTheGameWantsItNow")

bool FScfgSheetMouseWishTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	for (const bool bWantsCapture : {false, true})
	{
		R.GameCapturesTheMouse();
		const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
		R.Frame(2);
		const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
		if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
		{
			return false;
		}
		TestFalse(TEXT("open: the capture released"), R.MouseCaptor() == R.Game);
		// meanwhile the game changed its input mode: a cursor shown and no capture (a UI-only menu), or not
		TSharedPtr<SWidget> Game = R.Game;
		Sheet->SetMouseWish([Game, bWantsCapture]()
		{
			FScfgMouseWish Wish;
			Wish.Viewport = Game;
			Wish.bCapture = bWantsCapture;
			return Wish;
		});
		R.Key(EKeys::Escape);
		R.Frame(2);
		TestTrue(TEXT("closed"), Box->IsSet());
		TestEqual(bWantsCapture ? TEXT("the game wants the capture: given back") : TEXT("the game no longer captures: not forced on it"),
			R.MouseCaptor() == R.Game, bWantsCapture);
		FSlateApplication::Get().ReleaseAllPointerCapture(0);
	}
	// a lock without a capture (a LockAlways viewport): recorded at the take, so it is given back at the close
	R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("a third sheet"), Sheet.IsValid()))
	{
		return false;
	}
	TSharedPtr<SWidget> Game = R.Game;
	Sheet->SetMouseWish([Game]()
	{
		FScfgMouseWish Wish;
		Wish.Viewport = Game;
		Wish.bCapture = true;
		Wish.bLock = true;
		Wish.bAlwaysLock = true;
		return Wish;
	});
	TestFalse(TEXT("no capture anywhere"), R.MouseCaptor().IsValid());
	Sheet->OnShown();  // the take again (as on a re-show): the lock it releases is recorded
	TestTrue(TEXT("an always-locking viewport: its lock is held, with no capture"), Sheet->HeldMouseCaptor() == R.Game);
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestFalse(TEXT("given back as a lock only: no capture forced"), R.MouseCaptor().IsValid());

	// B2: the engine's defaults (capture permanently, lock on capture) in a game that held nothing at the open (a
	// cursor-showing game releases capture and lock on every mouse-up): nothing is taken, nothing is forced at close
	R.Host->SetMouseWishOverride([Game]()
	{
		FScfgMouseWish Wish;
		Wish.Viewport = Game;
		Wish.bCapture = true;
		Wish.bHighPrecision = true;
		Wish.bLock = true;
		return Wish;
	});
	const TSharedRef<TOptional<FScfgPaymentResult>> Fourth = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Plain = R.Sheet();
	if (!TestTrue(TEXT("a fourth sheet"), Plain.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("nothing held at the open: nothing recorded"), Plain->HeldMouseCaptor().IsValid());
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("closed"), Fourth->IsSet());
	TestFalse(TEXT("the default wish turns nothing into a capture at the close"), R.MouseCaptor().IsValid());
	return true;
}

// Ruling R2-15, round 3 (B1): the screens take the mouse and the focus back only from the game viewport (or a focus
// that went nowhere). Another widget (an editor button or field in PIE, a game modal above the sheet, a menu) keeps
// its capture and its focus, and a capture it held at the open is neither taken nor recorded.
SCFG_SHEET_TEST(FScfgSheetOthersKeepInputTest, "OtherWidgetsKeepTheirMouseAndFocus")

bool FScfgSheetOthersKeepInputTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	FSlateApplication& App = FSlateApplication::Get();
	const TSharedRef<SGameSink> Other = SNew(SGameSink);
	R.Layer->AddSlot(FScfgDefaultSheetHost::ZORDER + 5)[Other];  // above the sheet: a modal, a menu, an editor widget
	R.Frame();
	auto Give = [&App, &Other](bool bFocus)
	{
		FWidgetPath Path;
		App.FindPathToWidget(Other, Path);
		FReply Reply = FReply::Handled().CaptureMouse(Other);
		if (bFocus)
		{
			Reply.SetUserFocus(Other, EFocusCause::SetDirectly);
		}
		App.ProcessReply(Path, Reply, nullptr, nullptr, 0);
	};
	Give(false);
	TestTrue(TEXT("another widget holds the capture (a button mid-click)"), R.MouseCaptor() == Other);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the open leaves its capture alone"), R.MouseCaptor() == Other);
	TestFalse(TEXT("and records nothing"), Sheet->HeldMouseCaptor().IsValid());
	Give(true);
	R.Frame(3);
	TestTrue(TEXT("a widget above the sheet keeps the focus"), R.Focused() == Other);
	TestTrue(TEXT("and the capture"), R.MouseCaptor() == Other);
	R.Key(EKeys::Q);
	TestTrue(TEXT("and gets the keys"), Other->Got.Contains(TEXT("Q")));
	TestEqual(TEXT("the sheet did not move"), Sheet->ShownStep(), FString(TEXT("amount")));
	// it goes away, and the focus with it: the sheet takes the focus that went nowhere
	App.ReleaseAllPointerCapture(0);
	R.Layer->RemoveSlot(Other);
	App.ClearAllUserFocus(EFocusCause::SetDirectly);
	R.Frame(2);
	TestTrue(TEXT("a focus that went nowhere comes back to the sheet"), R.FocusInside(*Sheet));
	R.Key(EKeys::Escape);
	R.Frame(2);
	TestTrue(TEXT("closed"), Box->IsSet());
	TestFalse(TEXT("no capture given to anyone at the close"), R.MouseCaptor().IsValid());
	return true;
}

// Task 7 re-review 3 (N1): a screen that closes below another hands what it took of the game viewport (the capture
// too) to the screen on top, merged with what that one holds, so the last close gives the capture back: with nothing
// held by the top screen, and with a lock alone held by it (an always-locking viewport).
SCFG_SHEET_TEST(FScfgSheetHandOverTest, "TheScreenBelowHandsItsCaptureToTheTop")

bool FScfgSheetHandOverTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	TSharedPtr<SWidget> Game = R.Game;
	for (const bool bAlwaysLock : {false, true})
	{
		const TCHAR* Case = bAlwaysLock ? TEXT("always-lock: ") : TEXT("nothing on top: ");
		R.Host->SetMouseWishOverride([Game, bAlwaysLock]()
		{
			FScfgMouseWish Wish;
			Wish.Viewport = Game;
			Wish.bCapture = true;
			Wish.bAlwaysLock = bAlwaysLock;
			return Wish;
		});
		R.GameCapturesTheMouse();
		const TSharedRef<TOptional<FScfgPaymentResult>> Below = R.W.Pay(EScfgPayKind::Unlock, Unlock());
		R.Frame(2);
		const TSharedPtr<SScfgPaymentSheet> Lower = R.Sheet();
		if (!TestTrue(FString(Case) + TEXT("the sheet"), Lower.IsValid()))
		{
			return false;
		}
		TestTrue(FString(Case) + TEXT("the sheet took the game's capture"), Lower->HeldMouseCaptor() == R.Game &&
			R.MouseCaptor() != R.Game);
		const TSharedRef<TOptional<FScfgRestoreResult>> Above = R.W.Restore();
		R.Frame(2);
		const TSharedPtr<SScfgRestoreScreen> Top = R.RestoreScreen();
		if (!TestTrue(FString(Case) + TEXT("the restore screen on top, with the focus"), Top.IsValid() &&
				R.FocusInside(*Top)))
		{
			return false;
		}
		TestEqual(FString(Case) + TEXT("the top screen holds a lock alone only under an always-locking viewport"),
			Top->HeldMouseCaptor() == R.Game, bAlwaysLock);
		Lower->CloseWith(TEXT("cancel"));  // the screen below closes first (a programmatic close)
		R.Frame(2);
		TestTrue(FString(Case) + TEXT("the sheet below closed"), Below->IsSet() && !Lower->IsOpen());
		TestTrue(FString(Case) + TEXT("the top screen keeps the focus"), R.FocusInside(*Top));
		TestTrue(FString(Case) + TEXT("and now holds the viewport's capture"), Top->HeldMouseCaptor() == R.Game);
		TestTrue(FString(Case) + TEXT("nobody has the capture meanwhile"), R.MouseCaptor() != R.Game);
		R.Key(EKeys::Escape);
		R.Frame(2);
		TestTrue(FString(Case) + TEXT("the restore screen closed"), Above->IsSet());
		TestTrue(FString(Case) + TEXT("the last close gave the capture back to the game"), R.MouseCaptor() == R.Game);
		FSlateApplication::Get().ReleaseAllPointerCapture(0);
	}
	return true;
}

// Ruling R2-15 (M1): a sheet the game takes off the viewport itself ("Remove All Widgets") is released on the host's
// next frame: the flow resolves as cancel, the focus goes back, nothing is left open.
SCFG_SHEET_TEST(FScfgSheetRemovedTest, "ASheetTheGameRemovesCountsAsCancel")

bool FScfgSheetRemovedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Tip, Tip(TEXT("3.00"), TEXT("USDC")));
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet has the focus"), Sheet.IsValid() && R.FocusInside(*Sheet)))
	{
		return false;
	}
	R.Viewport->RemoveAllViewportWidgets();
	TestEqual(TEXT("the game cleared its viewport"), R.Layer->GetNumWidgets(), 0);
	R.Host->Tick();
	R.Frame(2);
	TestTrue(TEXT("the flow resolved as cancel"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Cancelled);
	TestEqual(TEXT("the host shows nothing"), R.Host->Screens().Num(), 0);
	TestEqual(TEXT("no screen left open"), SScfgScreen::OpenScreens().Num(), 0);
	TestFalse(TEXT("the focus is not left on the removed sheet"), R.FocusInside(*Sheet));
	return true;
}

// The style override wins by name; the accent and the text on it come from the configuration (config::text_on).
SCFG_SHEET_TEST(FScfgSheetStyleTest, "TheStyleOverrideWins")

bool FScfgSheetStyleTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSheetTestsDetail;
	const TSharedRef<FSlateStyleSet> Custom = MakeShared<FSlateStyleSet>(TEXT("GameStyle"));
	Custom->Set(TEXT("Scfg.Title"), FTextBlockStyle().SetColorAndOpacity(FSlateColor(FLinearColor::Red)));
	const TSharedRef<FScfgStyle> Style = FScfgStyle::Create(TEXT("#ffcc00"), FString(), Custom);
	TestEqual(TEXT("the developer's colour for the title"), Style->Text(TEXT("Scfg.Title")).ColorAndOpacity.GetSpecifiedColor(),
		FLinearColor::Red);
	TestEqual(TEXT("the others are the plugin's"), Style->Text(TEXT("Scfg.Muted")).ColorAndOpacity.GetSpecifiedColor(),
		FScfgStyle::MutedColor);
	TestEqual(TEXT("the accent from the configuration"), Style->Accent(), FLinearColor(FColor::FromHex(TEXT("#ffcc00"))));
	TestEqual(TEXT("ink on a light accent (text_on)"), Style->OnAccent(), FScfgStyle::InkColor);
	TestEqual(TEXT("white on the default accent"), FScfgStyle::Create(TEXT("#0e7c74"), FString())->OnAccent(), FScfgStyle::WhiteColor);
	TestEqual(TEXT("anything but #rrggbb: the default accent"), FScfgStyle::Create(TEXT("teal"), FString())->Accent(),
		FLinearColor(FColor::FromHex(TEXT("#0e7c74"))));
	TestFalse(TEXT("no logo without one"), Style->HasLogo());
	TestEqual(TEXT("the docked width"), FScfgStyle::SheetWidthFor(FVector2D(1280, 720)), 520.f);
	TestEqual(TEXT("the whole width on a portrait screen"), FScfgStyle::SheetWidthFor(FVector2D(360, 640)), 360.f);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
