// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/sheet.gd (see SScfgPaymentSheet.h).
#include "UI/SScfgPaymentSheet.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgFacade.h"
#include "ScfgLog.h"
#include "UI/SScfgQrView.h"
#include "UI/ScfgTexts.h"

#if UE_BUILD_SHIPPING
bool SScfgPaymentSheet::bShowDevLines = false;
#else
bool SScfgPaymentSheet::bShowDevLines = true;
#endif

namespace ScfgPaymentSheetDetail
{

// The states whose only action is Continue (the payment is over, or this sheet is).
static bool IsEndedState(const FString& State)
{
	return State == TEXT("closed") || State == TEXT("paid") || State == TEXT("underpaid") || State == TEXT("expired");
}

static void AddTo(SVerticalBox& Box, const TSharedRef<SWidget>& Widget, float Gap = 10.f)
{
	Box.AddSlot().AutoHeight().Padding(0, 0, 0, Gap)[Widget];
}

}  // namespace ScfgPaymentSheetDetail

void SScfgPaymentSheet::Construct(
	const FArguments& InArgs, const FScfgCheckoutController& InController, const TSharedRef<FScfgStyle>& InStyle)
{
	Controller.Emplace(InController);
	ConstructScreen(InStyle);
}

TPair<int32, int32> SScfgPaymentSheet::Dots() const
{
	const FScfgCheckoutController& C = *Controller;
	if (C.State() == TEXT("closed") || C.Step() == TEXT("done"))
	{
		return TPair<int32, int32>(0, 0);
	}
	const TPair<int32, int32> D = C.StepDots();
	return D.Value > 1 ? D : TPair<int32, int32>(0, 0);
}

bool SScfgPaymentSheet::Ended() const
{
	return ScfgPaymentSheetDetail::IsEndedState(Controller->State());
}

// --- what is shown --------------------------------------------------------------------------------------------------

FString SScfgPaymentSheet::Signature() const
{
	const FScfgCheckoutController& C = *Controller;
	TArray<FString> Parts = {C.Step(), C.State(), C.MessageId(), C.CanGoBack() ? TEXT("1") : TEXT("0"), C.HintTx(),
		LexToString(C.Confirmations()), LexToString(C.Required()), C.Method(), C.ChainKey(), C.PriceText(),
		C.SessionId(), C.BrowserFailed() ? TEXT("1") : TEXT("0"), C.ResultTx(), C.Amount()};
	for (const FScfgChainOptionView& O : C.ChainOptions())
	{
		Parts.Add(O.Key);
	}
	Parts.Add(TEXT("|"));
	Parts.Append(C.Methods());
	Parts.Add(FString::FromInt(FMath::RoundToInt(PanelWidthNow())));  // Godot's k: a resize rebuilds (the QR's size)
	return FString::Join(Parts, TEXT("\x1f"));
}

FString SScfgPaymentSheet::StepKey() const
{
	return Controller->Step();
}

FString SScfgPaymentSheet::PrimaryKey() const
{
	const FScfgCheckoutController& C = *Controller;
	if (Ended())
	{
		return TEXT("continue");
	}
	const FString Step = C.Step();
	if (Step == TEXT("amount"))
	{
		const TArray<FString> Presets = C.TipPresets();
		for (int32 I = 0; I < Presets.Num(); ++I)
		{
			if (C.IsPresetSelected(I))
			{
				return TEXT("preset_") + Presets[I];
			}
		}
		return TEXT("custom_amount");
	}
	if (Step == TEXT("network"))
	{
		const TArray<FScfgChainOptionView> Options = C.ChainOptions();
		if (!C.ChainKey().IsEmpty())
		{
			return TEXT("network_") + C.ChainKey();
		}
		return Options.Num() > 0 ? TEXT("network_") + Options[0].Key : FString(TEXT("cancel"));
	}
	if (Step == TEXT("method"))
	{
		const TArray<FString> Methods = C.Methods();
		if (!C.Method().IsEmpty() && Methods.Contains(C.Method()))
		{
			return TEXT("method_") + C.Method();
		}
		return Methods.Num() > 0 ? TEXT("method_") + Methods[0] : FString(TEXT("cancel"));
	}
	if (Step == TEXT("browser") || Step == TEXT("qr"))
	{
		return TEXT("forward");
	}
	if (Step == TEXT("manual"))
	{
		return TEXT("copy_amount");
	}
	if (Step == TEXT("waiting"))
	{
		for (const TCHAR* Key : {TEXT("explorer"), TEXT("reopen"), TEXT("keep_playing")})
		{
			if (Control(Key).IsValid())
			{
				return Key;
			}
		}
	}
	return TEXT("cancel");
}

void SScfgPaymentSheet::Sync()
{
	const FScfgCheckoutController& C = *Controller;
	const TPair<FString, FString> Values[] = {{TEXT("tx_input"), C.TxInput()}, {TEXT("custom_amount"), C.CustomAmount()}};
	for (const TPair<FString, FString>& V : Values)
	{
		const FString* Last = Synced.Find(V.Key);
		if (Control(V.Key).IsValid() && (!Last || !Last->Equals(V.Value, ESearchCase::CaseSensitive)))
		{
			SetFieldText(V.Key, V.Value);
			Synced.Add(V.Key, V.Value);
		}
	}
}

void SScfgPaymentSheet::OnBack()
{
	FScfgCheckoutController& C = *Controller;
	if (C.CanGoBack())
	{
		C.Back();
	}
	else if (!C.TxSeen() && C.Step() != TEXT("waiting") && C.Step() != TEXT("done") && C.StepDots().Key == 0)
	{
		CloseWith(TEXT("cancel"));
	}
}

// --- the build ------------------------------------------------------------------------------------------------------

void SScfgPaymentSheet::BuildContent()
{
	Shown = Controller->Step();
	Synced.Reset();
	BuildHeader();
	TSharedPtr<SVerticalBox> Body;
	const TSharedRef<SWidget> Scroll = ScrollingBody(Body);
	const TPair<FString, TFunction<void()>> Forward = BuildStep(*Body);
	const TSharedRef<SVerticalBox> Step = SNew(SVerticalBox);
	Step->AddSlot().FillHeight(1).Padding(0, 0, 0, 12)[Scroll];
	if (Forward.Key.IsEmpty())
	{
		// the key exists on every step; collapsed when the step has none
		Step->AddSlot().AutoHeight()[Button(TEXT("forward"), FString(), TEXT("Scfg.Button.Accent"), nullptr, FString(), true, false)];
	}
	else
	{
		Step->AddSlot().AutoHeight()[Button(TEXT("forward"), Forward.Key, TEXT("Scfg.Button.Accent"), Forward.Value)];
	}
	GetLayout().AddSlot().FillHeight(1).Padding(0, 0, 0, 14)[Step];
	BuildFooter();
}

void SScfgPaymentSheet::BuildHeader()
{
	FScfgCheckoutController C = *Controller;
	const TSharedRef<SVerticalBox> Header = SNew(SVerticalBox);
	GetLayout().AddSlot().AutoHeight().Padding(0, 0, 0, 14)[Header];
	const TSharedRef<SHorizontalBox> Nav = SNew(SHorizontalBox);
	Nav->AddSlot().AutoWidth()[BackArrow(TEXT("back"), [C]() mutable { C.Back(); }, C.CanGoBack())];
	Nav->AddSlot().FillWidth(1)[SNew(SSpacer)];
	const TPair<int32, int32> D = Dots();
	if (D.Value > 0)
	{
		const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 I = 0; I < D.Value; ++I)
		{
			const FLinearColor Color = I <= D.Key ? GetStyle().Accent() : FScfgStyle::LineColor;
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(3, 0)[Dot(Color)];
		}
		Nav->AddSlot().AutoWidth().VAlign(VAlign_Center)[Reg(TEXT("dots"), Row)];
	}
	ScfgPaymentSheetDetail::AddTo(*Header, SNew(SBox).MinDesiredHeight(36)[Nav]);
	if (const TSharedPtr<SWidget> Mark = Logo())
	{
		ScfgPaymentSheetDetail::AddTo(*Header, Mark.ToSharedRef());
	}
	if (C.State() == TEXT("closed"))
	{
		return;
	}
	if (bShowDevLines)
	{
		ScfgPaymentSheetDetail::AddTo(*Header, Label(C.IsTestnet() ? ScfgTexts::Format(TEXT("dev_mode_paying_to_testnet"), {Short(C.Recipient())})
										 : ScfgTexts::Format(TEXT("dev_mode_paying_to"), {Short(C.Recipient())}),
						 TEXT("Scfg.Dev")));
		for (const FString& W : C.DevWarnings())
		{
			ScfgPaymentSheetDetail::AddTo(*Header, Label(W, TEXT("Scfg.Dev")));
		}
	}
	ScfgPaymentSheetDetail::AddTo(*Header, Reg(TEXT("title"), Label(C.Title(), TEXT("Scfg.Title"))));
	ScfgPaymentSheetDetail::AddTo(*Header,
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0, 0, 8, 0)
		[
			Reg(TEXT("price"), Label(C.PriceText(), TEXT("Scfg.Amount"), false))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0, 0, 0, 6)
		[
			Label(C.Token(), TEXT("Scfg.AmountToken"), false)
		]);
	const FString Note = ExactNote();
	if (!Note.IsEmpty())
	{
		ScfgPaymentSheetDetail::AddTo(*Header, Label(Note, TEXT("Scfg.Note")), 0);
	}
}

TPair<FString, TFunction<void()>> SScfgPaymentSheet::BuildStep(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	const FString Step = C.Step();
	if (C.State() == TEXT("closed"))
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("payment_screen_inactive"))));
		return {};
	}
	if (Step == TEXT("amount"))
	{
		StepAmount(Body);
		return {ScfgTexts::Get(TEXT("next")), [C]() mutable { C.Next(); }};
	}
	if (Step == TEXT("network"))
	{
		StepNetwork(Body);
	}
	else if (Step == TEXT("method"))
	{
		StepMethod(Body);
	}
	else if (Step == TEXT("browser"))
	{
		StepBrowser(Body);
		return {ScfgTexts::Get(TEXT("open_checkout_browser")), [C]() mutable { C.OpenBrowser(); }};
	}
	else if (Step == TEXT("qr"))
	{
		StepQr(Body);
		return {ScfgTexts::Get(TEXT("i_sent_it")), [C]() mutable { C.MarkSent(); }};
	}
	else if (Step == TEXT("manual"))
	{
		StepManual(Body);
		return {ScfgTexts::Get(TEXT("i_sent_it")), [C]() mutable { C.MarkSent(); }};
	}
	else if (Step == TEXT("waiting"))
	{
		StepWaiting(Body);
	}
	else
	{
		StepDone(Body);
	}
	return {};
}

void SScfgPaymentSheet::StepAmount(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("choose_amount")), TEXT("Scfg.Label")));
	const TArray<FString> Presets = C.TipPresets();
	const TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(4));
	const int32 Columns = FMath::Max(1, FMath::Min(Presets.Num(), 4));
	for (int32 I = 0; I < Presets.Num(); ++I)
	{
		const FString Preset = Presets[I];
		Grid->AddSlot(I % Columns, I / Columns)
		[
			Option(TEXT("preset_") + Preset, PlainAmount(Preset) + TEXT(" ") + C.Token(), FString(),
				C.IsPresetSelected(I), [C, Preset]() mutable { C.SetTip(Preset); }, TEXT("presets"))
		];
	}
	ScfgPaymentSheetDetail::AddTo(Body, Grid);
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("or_enter_your_own")), TEXT("Scfg.Label")));
	TWeakPtr<SScfgPaymentSheet> Weak = StaticCastSharedRef<SScfgPaymentSheet>(AsShared());
	ScfgPaymentSheetDetail::AddTo(Body,
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 8, 0)
		[
			Field(TEXT("custom_amount"), C.CustomAmount(), 8, TEXT("0123456789."),
				[C, Weak](const FString& Text) mutable
				{
					C.SetCustomAmount(Text);
					if (const TSharedPtr<SScfgPaymentSheet> S = Weak.Pin())
					{
						S->Synced.Add(TEXT("custom_amount"), Text);
					}
				},
				[C]() mutable { C.ApplyCustomAmount(); }, TEXT("amount"))
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).MinDesiredWidth(90)
			[
				Button(TEXT("set_amount"), ScfgTexts::Get(TEXT("set")), TEXT("Scfg.Button.Small"),
					[C]() mutable { C.ApplyCustomAmount(); }, TEXT("amount"))
			]
		]);
	Synced.Add(TEXT("custom_amount"), C.CustomAmount());
}

void SScfgPaymentSheet::StepNetwork(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Format(TEXT("which_network"), {C.Token()}), TEXT("Scfg.Label")));
	const TArray<FScfgChainOptionView> Options = C.ChainOptions();
	const int32 Columns = FMath::Max(1, FMath::Min(Options.Num(), 2));
	const TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(4));
	for (int32 I = 0; I < Options.Num(); ++I)
	{
		const FString Key = Options[I].Key;
		Grid->AddSlot(I % Columns, I / Columns)
		[
			Option(TEXT("network_") + Key, Options[I].Name, FeeLabel(Options[I].FeeHint), C.ChainKey() == Key,
				[C, Key]() mutable { C.ChooseNetwork(Key); }, FString::Printf(TEXT("networks:%d"), I / 2))
		];
	}
	ScfgPaymentSheetDetail::AddTo(Body, Grid);
}

void SScfgPaymentSheet::StepMethod(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("how_to_pay")), TEXT("Scfg.Label")));
	for (const FString& M : C.Methods())
	{
		ScfgPaymentSheetDetail::AddTo(Body, Option(TEXT("method_") + M, MethodLabel(M), MethodHint(M), C.Method() == M,
					  [C, M]() mutable { C.ChooseMethod(M); }), 8);
	}
}

void SScfgPaymentSheet::StepBrowser(SVerticalBox& Body)
{
	const FScfgCheckoutController& C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("browser_wallet")), TEXT("Scfg.Label")));
	ScfgPaymentSheetDetail::AddTo(Body, Label(C.Family() == TEXT("solana") ? ScfgTexts::Get(TEXT("solana_opens_browser_note"))
												  : ScfgTexts::Get(TEXT("evm_opens_browser_note"))));
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Format(TEXT("approve_in_wallet"), {AmountLine(false), C.ChainDisplay()}), TEXT("Scfg.Muted")));
}

void SScfgPaymentSheet::StepQr(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("phone_wallet")), TEXT("Scfg.Label")));
	// The code and the steps side by side, the code 176 units at most; on a narrow sheet (a portrait or small screen)
	// the steps go below it and the code takes what the sheet's width leaves (the QR scales with the sheet).
	const float Inner = ContentWidthNow();
	const bool bNarrow = Inner < 176.f + 16.f + 20.f + 160.f;
	const float Target = bNarrow ? FMath::Clamp(Inner - 16.f, 16.f, 176.f) : 176.f;
	TSharedPtr<SWidget> Code;
	const FString Uri = C.PaymentUri();
	if (!Uri.IsEmpty())
	{
		const TSharedRef<SScfgQrView> Qr = SNew(SScfgQrView).Text(Uri).TargetSize(Target);
		if (!Qr->GetError().IsEmpty())
		{
			UE_LOG(LogScfg, Error, TEXT("scfg: screen: the payment link has no QR code: %s"), *Qr->GetError());
		}
		Code = SNew(SBorder).BorderImage(GetStyle().Brush(TEXT("Scfg.QrFrame"))).Padding(8)[Reg(TEXT("qr"), Qr)];
	}
	const TSharedRef<SVerticalBox> Steps = SNew(SVerticalBox);
	ScfgPaymentSheetDetail::AddTo(*Steps, Label(ScfgTexts::Get(TEXT("qr_step_open")), TEXT("Scfg.QrStep")), 8);
	ScfgPaymentSheetDetail::AddTo(*Steps, Label(ScfgTexts::Get(TEXT("qr_step_scan")), TEXT("Scfg.QrStep")), 8);
	ScfgPaymentSheetDetail::AddTo(*Steps, Label(ScfgTexts::Format(TEXT("qr_step_approve"), {AmountLine(false)}), TEXT("Scfg.QrStep")), 0);
	if (bNarrow)
	{
		if (Code.IsValid())
		{
			Body.AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 10)[Code.ToSharedRef()];
		}
		ScfgPaymentSheetDetail::AddTo(Body, Steps);
	}
	else
	{
		const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		if (Code.IsValid())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 20, 0)[Code.ToSharedRef()];
		}
		Row->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Steps];
		ScfgPaymentSheetDetail::AddTo(Body, Row);
	}
	if (C.IsMobile() && C.QrWalletButton())
	{
		ScfgPaymentSheetDetail::AddTo(Body, Button(TEXT("wallet_app"), ScfgTexts::Get(TEXT("open_wallet_app")), TEXT("Scfg.Button"),
					  [C]() mutable { C.OpenWalletApp(); }));
	}
	ScfgPaymentSheetDetail::AddTo(Body, Label(C.Family() == TEXT("solana") ? ScfgTexts::Get(TEXT("wallet_changes_amount"))
												  : ScfgTexts::Get(TEXT("wallet_changes_amount_manual")),
				  TEXT("Scfg.Muted")));
}

void SScfgPaymentSheet::StepManual(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	const bool bSolana = C.Family() == TEXT("solana");
	ScfgPaymentSheetDetail::AddTo(Body, Label(bSolana ? ScfgTexts::Get(TEXT("send_exact_amount_solana"))
							: ScfgTexts::Format(TEXT("send_exact_amount"), {C.ChainDisplay()}),
				  TEXT("Scfg.Warning")));
	struct FRow
	{
		const TCHAR* Id;
		FString Caption;
		FString Value;
	};
	const FRow Rows[] = {
		{TEXT("amount"), ScfgTexts::Get(TEXT("amount")), AmountLine(true)},
		{TEXT("recipient"), ScfgTexts::Get(TEXT("to_address")), C.Recipient()},
		{TEXT("token"), bSolana ? ScfgTexts::Get(TEXT("token_mint")) : ScfgTexts::Get(TEXT("token_contract")), C.TokenAddress()},
	};
	for (const FRow& R : Rows)
	{
		FString First = R.Value;
		int32 Space = INDEX_NONE;
		if (First.FindChar(TEXT(' '), Space))
		{
			First = First.Left(Space);
		}
		const TSharedRef<SVerticalBox> Group = SNew(SVerticalBox);
		ScfgPaymentSheetDetail::AddTo(*Group, Label(R.Caption, TEXT("Scfg.Label")), 2);
		Group->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)[Mono(R.Value)]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).MinDesiredWidth(72)
				[
					Button(FString(TEXT("copy_")) + R.Id, ScfgTexts::Get(TEXT("copy")), TEXT("Scfg.Button.Small"),
						[C, First]() mutable { C.Copy(First); })
				]
			]
		];
		ScfgPaymentSheetDetail::AddTo(Body, Group);
	}
	if (C.AllowTxPaste())
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Format(TEXT("sent_from_exchange_paste"), {TxWord()}), TEXT("Scfg.Label")));
		TWeakPtr<SScfgPaymentSheet> Weak = StaticCastSharedRef<SScfgPaymentSheet>(AsShared());
		ScfgPaymentSheetDetail::AddTo(Body,
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 8, 0)
			[
				Field(TEXT("tx_input"), C.TxInput(), FScfgFacade::TX_MAX_LENGTH, FScfgFacade::TxChars(),
					[C, Weak](const FString& Text) mutable
					{
						C.SetTxInput(Text);
						if (const TSharedPtr<SScfgPaymentSheet> S = Weak.Pin())
						{
							S->Synced.Add(TEXT("tx_input"), Text);
						}
					},
					[C]() mutable { C.SubmitHash(); }, TEXT("tx"))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).MinDesiredWidth(90)
				[
					Button(TEXT("check"), ScfgTexts::Get(TEXT("check")), TEXT("Scfg.Button.Small"),
						[C]() mutable { C.SubmitHash(); }, TEXT("tx"))
				]
			]);
		Synced.Add(TEXT("tx_input"), C.TxInput());
	}
}

void SScfgPaymentSheet::StepWaiting(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, Label(ProgressText(), TEXT("Scfg.Heading")));
	if (C.State() == TEXT("confirming"))
	{
		const float Percent = float(C.Confirmations()) / float(FMath::Max<int64>(1, C.Required()));
		ScfgPaymentSheetDetail::AddTo(Body, Reg(TEXT("progress"),
					  SNew(SBox).HeightOverride(8)[SNew(SProgressBar).Style(&GetStyle().Progress()).Percent(Percent)]));
	}
	if (!C.HintTx().IsEmpty())
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Format(TEXT("your_hash"), {TxWord()}), TEXT("Scfg.Label")), 2);
		ScfgPaymentSheetDetail::AddTo(Body, Mono(C.HintTx()));
		const FString Url = C.ExplorerUrl();
		if (!Url.IsEmpty())
		{
			ScfgPaymentSheetDetail::AddTo(Body, SNew(SBox).HAlign(HAlign_Left)
				[
					Button(TEXT("explorer"), ScfgTexts::Get(TEXT("view_on_explorer")), TEXT("Scfg.Button.Small"),
						[C, Url]() mutable { C.OpenExplorer(Url); })
				]);
		}
	}
	else if (C.Method() == TEXT("browser"))
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("finish_in_browser")), TEXT("Scfg.Muted")));
		ScfgPaymentSheetDetail::AddTo(Body, SNew(SBox).HAlign(HAlign_Left)
			[
				Button(TEXT("reopen"), ScfgTexts::Get(TEXT("reopen_checkout_page")), TEXT("Scfg.Button.Small"),
					[C]() mutable { C.OpenBrowser(); })
			]);
	}
	ScfgPaymentSheetDetail::AddTo(Body, SNew(SSpacer).Size(FVector2D(0, 4)), 0);
	ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("keep_playing_note")), TEXT("Scfg.Muted")));
}

void SScfgPaymentSheet::StepDone(SVerticalBox& Body)
{
	FScfgCheckoutController C = *Controller;
	ScfgPaymentSheetDetail::AddTo(Body, SNew(STextBlock)
				  .Text(FText::FromString(DoneTitle()))
				  .TextStyle(&GetStyle().Text(TEXT("Scfg.Heading")))
				  .ColorAndOpacity(FSlateColor(StatusColor()))
				  .AutoWrapText(true));
	const FString Tx = C.ResultTx();
	if (C.State() == TEXT("paid") && !Tx.IsEmpty())
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Format(TEXT("keep_this_to_restore"), {TxWord()}), TEXT("Scfg.Muted")));
		ScfgPaymentSheetDetail::AddTo(Body, Mono(Tx));
		const FString Url = C.ExplorerUrl();
		const FString CopyText = C.Family() == TEXT("solana") ? ScfgTexts::Get(TEXT("copy_signature")) : ScfgTexts::Get(TEXT("copy_hash"));
		ScfgPaymentSheetDetail::AddTo(Body,
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				Button(TEXT("copy_tx"), CopyText, TEXT("Scfg.Button.Small"), [C, Tx]() mutable { C.Copy(Tx); }, TEXT("done"))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(TEXT("explorer"), ScfgTexts::Get(TEXT("view_on_explorer")), TEXT("Scfg.Button.Small"),
					[C, Url]() mutable { C.OpenExplorer(Url); }, TEXT("done"))
			]);
	}
	else if (C.State() == TEXT("underpaid"))
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("underpaid_note")), TEXT("Scfg.Muted")));
	}
	else if (C.State() == TEXT("expired"))
	{
		ScfgPaymentSheetDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("already_paid_note")), TEXT("Scfg.Muted")));
	}
}

void SScfgPaymentSheet::BuildFooter()
{
	const FScfgCheckoutController& C = *Controller;
	TWeakPtr<SScfgPaymentSheet> Weak = StaticCastSharedRef<SScfgPaymentSheet>(AsShared());
	const auto CloseAs = [Weak](const TCHAR* How)
	{
		const FString Word = How;
		return [Weak, Word]()
		{
			if (const TSharedPtr<SScfgPaymentSheet> S = Weak.Pin())
			{
				S->CloseWith(Word);
			}
		};
	};
	const TSharedRef<SVerticalBox> Footer = SNew(SVerticalBox);
	const FString Msg = C.Message();
	if (!Msg.IsEmpty())
	{
		ScfgPaymentSheetDetail::AddTo(*Footer, Reg(TEXT("message"), Label(Msg, TEXT("Scfg.Error"))), 12);
	}
	ScfgPaymentSheetDetail::AddTo(*Footer, Rule(), 12);
	const FString Status = C.State() != TEXT("closed") ? StatusText() : FString();
	if (!Status.IsEmpty())
	{
		ScfgPaymentSheetDetail::AddTo(*Footer, Reg(TEXT("status"), StatusRow(Status, StatusColor())), 12);
	}
	const TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);
	if (Ended())
	{
		Actions->AddSlot().FillWidth(1)[Button(TEXT("continue"), ScfgTexts::Get(TEXT("continue")), TEXT("Scfg.Button.Primary"),
			CloseAs(TEXT("finish")), TEXT("footer"))];
	}
	else
	{
		Actions->AddSlot().FillWidth(1)[Button(TEXT("cancel"), ScfgTexts::Get(TEXT("cancel")), TEXT("Scfg.Button"),
			CloseAs(TEXT("cancel")), TEXT("footer"))];
		if (C.HasSession())
		{
			Actions->AddSlot().FillWidth(1).Padding(10, 0, 0, 0)[Button(TEXT("keep_playing"),
				ScfgTexts::Get(TEXT("continue_playing")), TEXT("Scfg.Button.Primary"), CloseAs(TEXT("keep_playing")), TEXT("footer"))];
		}
	}
	ScfgPaymentSheetDetail::AddTo(*Footer, Actions, 0);
	GetLayout().AddSlot().AutoHeight()[Footer];
}

// --- texts (Ren'Py's helpers of the same names) ---------------------------------------------------------------------

FString SScfgPaymentSheet::TxWord() const
{
	return Controller->Family() == TEXT("solana") ? ScfgTexts::Get(TEXT("word_signature")) : ScfgTexts::Get(TEXT("word_tx_hash"));
}

FString SScfgPaymentSheet::MethodLabel(const FString& Method) const
{
	if (Method == TEXT("browser"))
	{
		return ScfgTexts::Get(TEXT("browser_wallet"));
	}
	if (Method == TEXT("qr"))
	{
		return ScfgTexts::Get(TEXT("phone_wallet"));
	}
	return ScfgTexts::Get(TEXT("manual_transfer"));
}

FString SScfgPaymentSheet::MethodHint(const FString& Method) const
{
	if (Method == TEXT("browser"))
	{
		return Controller->Family() == TEXT("solana") ? ScfgTexts::Get(TEXT("solana_extension_sub"))
													   : ScfgTexts::Get(TEXT("evm_extension_sub"));
	}
	if (Method == TEXT("qr"))
	{
		return ScfgTexts::Get(TEXT("scan_with_phone_wallet"));
	}
	return ScfgTexts::Get(TEXT("send_from_exchange"));
}

FString SScfgPaymentSheet::FeeLabel(const FString& Fee)
{
	if (Fee == TEXT("low"))
	{
		return ScfgTexts::Get(TEXT("fees_low"));
	}
	if (Fee == TEXT("lowest"))
	{
		return ScfgTexts::Get(TEXT("fees_lowest"));
	}
	if (Fee == TEXT("high"))
	{
		return ScfgTexts::Get(TEXT("fees_high"));
	}
	return Fee;  // a developer's own chain: its own word
}

FString SScfgPaymentSheet::StatusText() const
{
	const FScfgCheckoutController& C = *Controller;
	const FString Step = C.Step();
	if (Step == TEXT("amount") || Step == TEXT("waiting") || Step == TEXT("done") || C.State() == TEXT("choose"))
	{
		return FString();
	}
	if (C.State() == TEXT("waiting") && C.HintTx().IsEmpty())
	{
		return FString();
	}
	return ProgressText();
}

FString SScfgPaymentSheet::ProgressText() const
{
	const FScfgCheckoutController& C = *Controller;
	const FString Chain = C.ChainDisplay();
	const FString State = C.State();
	if (State == TEXT("starting"))
	{
		return ScfgTexts::Get(TEXT("preparing_payment"));
	}
	if (State == TEXT("waiting"))
	{
		return C.HintTx().IsEmpty() ? ScfgTexts::Format(TEXT("waiting_for_payment"), {Chain})
									: ScfgTexts::Format(TEXT("tx_received_waiting"), {Chain});
	}
	if (State == TEXT("confirming"))
	{
		return ScfgTexts::Format(TEXT("confirming"), {Chain, LexToString(C.Confirmations()), LexToString(C.Required())});
	}
	return FString();
}

FString SScfgPaymentSheet::DoneTitle() const
{
	const FString State = Controller->State();
	if (State == TEXT("paid"))
	{
		return ScfgTexts::Get(TEXT("paid_title"));
	}
	if (State == TEXT("underpaid"))
	{
		return ScfgTexts::Get(TEXT("payment_too_small"));
	}
	return ScfgTexts::Get(TEXT("payment_window_closed"));
}

FLinearColor SScfgPaymentSheet::StatusColor() const
{
	const FString State = Controller->State();
	if (State == TEXT("paid"))
	{
		return FScfgStyle::OkColor;
	}
	if (State == TEXT("underpaid"))
	{
		return FScfgStyle::ErrorColor;
	}
	if (State == TEXT("expired"))
	{
		return FScfgStyle::WarnColor;
	}
	return GetStyle().Accent();
}

FString SScfgPaymentSheet::AmountLine(bool bExact) const
{
	const FScfgCheckoutController& C = *Controller;
	return (bExact ? C.PayAmountText() : C.PriceText()) + TEXT(" ") + C.Token();
}

FString SScfgPaymentSheet::ExactNote() const
{
	const FScfgCheckoutController& C = *Controller;
	const FString Step = C.Step();
	if (!C.HasSession() || Step == TEXT("manual") || Step == TEXT("done") || C.State() == TEXT("closed"))
	{
		return FString();
	}
	return ScfgTexts::Format(TEXT("wallet_will_ask"), {AmountLine(true)});
}

FString SScfgPaymentSheet::PlainAmount(const FString& Text)
{
	return Text.EndsWith(TEXT(".00")) ? Text.LeftChop(3) : Text;
}

FString SScfgPaymentSheet::Short(const FString& Address)
{
	return Address.Left(6) + TEXT("...") + Address.Right(4);
}
