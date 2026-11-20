// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/restore.gd (see SScfgRestoreScreen.h).
#include "UI/SScfgRestoreScreen.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgFacade.h"
#include "ScfgString.h"
#include "UI/ScfgTexts.h"

#include "Scfg/Restore.h"

namespace ScfgRestoreScreenDetail
{

static void AddTo(SVerticalBox& Box, const TSharedRef<SWidget>& Widget, float Gap = 10.f)
{
	Box.AddSlot().AutoHeight().Padding(0, 0, 0, Gap)[Widget];
}

}  // namespace ScfgRestoreScreenDetail

void SScfgRestoreScreen::Construct(
	const FArguments& InArgs, const FScfgRestoreController& InController, const TSharedRef<FScfgStyle>& InStyle)
{
	Controller.Emplace(InController);
	ConstructScreen(InStyle);
}

FString SScfgRestoreScreen::Signature() const
{
	const FScfgRestoreController& C = *Controller;
	// the purchases on this device change only with the receipts: their revision, not a copy each frame
	return FString::Printf(TEXT("%s\x1f%s\x1f%d\x1f%s\x1f%d\x1f%d\x1f%d"), *C.State(), *C.MessageId(), C.NumCandidates(),
		*C.ChoicesSignature(), C.CanConfirm() ? 1 : 0, C.Added().Num(), C.ReceiptsRevision());
}

FString SScfgRestoreScreen::StepKey() const
{
	return Controller->State();
}

FString SScfgRestoreScreen::PrimaryKey() const
{
	const FScfgRestoreController& C = *Controller;
	const FString State = C.State();
	if (State == TEXT("input") || State == TEXT("checking"))
	{
		return TEXT("tx_input");
	}
	if (State == TEXT("choose"))
	{
		for (const int32 I : C.PendingChoices())
		{
			const FScfgRestoreCandidateView Candidate = C.Candidate(I);
			if (C.Choice(I).IsEmpty() && Candidate.Choices.Num() > 0)
			{
				return FString::Printf(TEXT("choice_%d_%s"), I, *Candidate.Choices[0]);
			}
		}
		return TEXT("restore");
	}
	return TEXT("close");
}

void SScfgRestoreScreen::Sync()
{
	const FString Now = Controller->TxInput();
	if (Control(TEXT("tx_input")).IsValid() && (!Synced.IsSet() || !Synced->Equals(Now, ESearchCase::CaseSensitive)))
	{
		SetFieldText(TEXT("tx_input"), Now);
		Synced = Now;
	}
}

void SScfgRestoreScreen::OnBack()
{
	CloseWith(TEXT("close"));
}

void SScfgRestoreScreen::BuildContent()
{
	FScfgRestoreController C = *Controller;
	Synced.Reset();
	TSharedPtr<SVerticalBox> Body;
	GetLayout().AddSlot().FillHeight(1).Padding(0, 0, 0, 14)[ScrollingBody(Body)];
	if (const TSharedPtr<SWidget> Mark = Logo())
	{
		ScfgRestoreScreenDetail::AddTo(*Body, Mark.ToSharedRef());
	}
	ScfgRestoreScreenDetail::AddTo(*Body, Reg(TEXT("title"), Label(ScfgTexts::Get(TEXT("restore_purchases")), TEXT("Scfg.Title"))));
	const FString State = C.State();
	if (State == TEXT("closed"))
	{
		ScfgRestoreScreenDetail::AddTo(*Body, Label(ScfgTexts::Get(TEXT("screen_inactive"))));
	}
	else if (State == TEXT("input") || State == TEXT("checking"))
	{
		InputPart(*Body);
	}
	else if (State == TEXT("choose"))
	{
		ChoosePart(*Body);
	}
	else if (State == TEXT("saving"))
	{
		ScfgRestoreScreenDetail::AddTo(*Body, StatusRow(ScfgTexts::Get(TEXT("saving")), GetStyle().Accent()));
	}
	else if (State == TEXT("done"))
	{
		const TArray<FScfgReceiptLine> Added = C.Added();
		if (Added.Num() > 0)
		{
			ScfgRestoreScreenDetail::AddTo(*Body, Label(ScfgTexts::Get(TEXT("restored")), TEXT("Scfg.Ok")));
			for (const FScfgReceiptLine& R : Added)
			{
				ScfgRestoreScreenDetail::AddTo(*Body, Label(ReceiptLine(R)));
			}
		}
		else
		{
			ScfgRestoreScreenDetail::AddTo(*Body, Label(ScfgTexts::Get(TEXT("nothing_to_restore"))));
		}
	}
	const FString Msg = C.Message();
	if (!Msg.IsEmpty())
	{
		ScfgRestoreScreenDetail::AddTo(*Body, Reg(TEXT("message"), Label(Msg, TEXT("Scfg.Error"))));
	}
	const TArray<FScfgReceiptLine> Owned = C.Receipts();
	if (Owned.Num() > 0)
	{
		ScfgRestoreScreenDetail::AddTo(*Body, Label(ScfgTexts::Get(TEXT("purchases_on_device")), TEXT("Scfg.Label")));
		for (int32 N = 0; N < Owned.Num(); ++N)
		{
			const FString Tx = Owned[N].TxHash;
			ScfgRestoreScreenDetail::AddTo(*Body,
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)[Label(ReceiptLine(Owned[N]))]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).MinDesiredWidth(110)
					[
						Button(FString::Printf(TEXT("copy_%d"), N), ScfgTexts::Get(TEXT("copy_hash")), TEXT("Scfg.Button.Small"),
							[C, Tx]() mutable { C.Copy(Tx); })
					]
				]);
		}
	}
	TWeakPtr<SScfgRestoreScreen> Weak = StaticCastSharedRef<SScfgRestoreScreen>(AsShared());
	const TSharedRef<SVerticalBox> Footer = SNew(SVerticalBox);
	ScfgRestoreScreenDetail::AddTo(*Footer, Rule(), 12);
	ScfgRestoreScreenDetail::AddTo(*Footer, Button(TEXT("close"), ScfgTexts::Get(TEXT("close")), TEXT("Scfg.Button"), [Weak]()
		{
			if (const TSharedPtr<SScfgRestoreScreen> S = Weak.Pin())
			{
				S->CloseWith(TEXT("close"));
			}
		}), 0);
	GetLayout().AddSlot().AutoHeight()[Footer];
}

void SScfgRestoreScreen::InputPart(SVerticalBox& Body)
{
	FScfgRestoreController C = *Controller;
	const bool bInput = C.State() == TEXT("input");
	ScfgRestoreScreenDetail::AddTo(Body, Label(ScfgTexts::Get(TEXT("paste_hash_help")), TEXT("Scfg.Muted")));
	TWeakPtr<SScfgRestoreScreen> Weak = StaticCastSharedRef<SScfgRestoreScreen>(AsShared());
	ScfgRestoreScreenDetail::AddTo(Body, Field(TEXT("tx_input"), C.TxInput(), FScfgFacade::TxMaxLength, FScfgFacade::TxChars(),
				  [C, Weak](const FString& Text) mutable
				  {
					  C.SetTxInput(Text);
					  if (const TSharedPtr<SScfgRestoreScreen> S = Weak.Pin())
					  {
						  S->Synced = Text;
					  }
				  },
				  [C]() mutable { C.Check(); }, TEXT("tx"), !bInput));
	Synced = C.TxInput();
	ScfgRestoreScreenDetail::AddTo(Body, Button(TEXT("check"), ScfgTexts::Get(TEXT("check")), TEXT("Scfg.Button.Accent"), [C]() mutable { C.Check(); },
				  FString(), bInput));
	if (C.State() == TEXT("checking"))
	{
		ScfgRestoreScreenDetail::AddTo(Body, Reg(TEXT("status"), StatusRow(ScfgTexts::Get(TEXT("checking_blockchain")), GetStyle().Accent())));
	}
}

void SScfgRestoreScreen::ChoosePart(SVerticalBox& Body)
{
	FScfgRestoreController C = *Controller;
	for (const int32 I : C.PendingChoices())
	{
		const FScfgRestoreCandidateView Candidate = C.Candidate(I);
		ScfgRestoreScreenDetail::AddTo(Body, Label(CandidateLine(Candidate)));
		const int32 Columns = FMath::Max(1, FMath::Min(Candidate.Choices.Num(), 2));
		const TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(4));
		for (int32 J = 0; J < Candidate.Choices.Num(); ++J)
		{
			const FString Choice = Candidate.Choices[J];
			Grid->AddSlot(J % Columns, J / Columns)
			[
				Option(FString::Printf(TEXT("choice_%d_%s"), I, *Choice), ChoiceLabel(Choice), FString(), C.Choice(I) == Choice,
					[C, I, Choice]() mutable { C.Choose(I, Choice); }, FString::Printf(TEXT("choices_%d:%d"), I, J / 2))
			];
		}
		ScfgRestoreScreenDetail::AddTo(Body, Grid);
	}
	ScfgRestoreScreenDetail::AddTo(Body, Button(TEXT("restore"), ScfgTexts::Get(TEXT("restore")), TEXT("Scfg.Button.Primary"),
				  [C]() mutable { C.Confirm(); }, FString(), C.CanConfirm()));
}

FString SScfgRestoreScreen::ReceiptLine(const FScfgReceiptLine& Receipt) const
{
	const FScfgRestoreController& C = *Controller;
	const FString What = Receipt.Kind == TEXT("unlock") ? C.ItemTitle(Receipt.Item) : ScfgTexts::Get(TEXT("tip"));
	return ScfgTexts::Format(TEXT("summary_line"), {What, Receipt.Amount, Receipt.Token, C.ChainName(Receipt.Chain)});
}

FString SScfgRestoreScreen::CandidateLine(const FScfgRestoreCandidateView& Candidate) const
{
	return ScfgTexts::Format(TEXT("unmatched_payment_prompt"),
		{Candidate.Amount, Candidate.Token, Controller->ChainName(Candidate.Chain)});
}

FString SScfgRestoreScreen::ChoiceLabel(const FString& Choice) const
{
	return ScfgString::ToUtf8(Choice) == scfg::restore::TIP_CHOICE ? ScfgTexts::Get(TEXT("keep_as_tip")) : Controller->ItemTitle(Choice);
}
