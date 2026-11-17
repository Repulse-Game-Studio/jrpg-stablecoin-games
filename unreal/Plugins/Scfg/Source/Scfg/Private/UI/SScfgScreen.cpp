// Twin of godot/addons/scfg/ui/screen_base.gd (see SScfgScreen.h).
#include "UI/SScfgScreen.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericApplication.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#include "UI/ScfgTexts.h"

namespace ScfgScreenDetail
{

// The screens shown and not hidden yet, oldest first (game thread).
static TArray<TWeakPtr<SScfgScreen>>& Registry()
{
	static TArray<TWeakPtr<SScfgScreen>> Open;
	return Open;
}

// Godot's back arrow (_arrow()): a short line with a chevron, in ink.
class SArrow final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SArrow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&) {}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		const FVector2f C = Size / 2.f;
		const float R = FMath::Min(Size.X, Size.Y) * 0.22f;
		const FLinearColor Ink = FScfgStyle::InkColor;
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			TArray<FVector2f>{C + FVector2f(-R, 0), C + FVector2f(R, 0)}, ESlateDrawEffect::None, Ink, true, 2.f);
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			TArray<FVector2f>{C + FVector2f(-R * 0.1f, -R * 0.9f), C + FVector2f(-R, 0), C + FVector2f(-R * 0.1f, R * 0.9f)},
			ESlateDrawEffect::None, Ink, true, 2.f);
		return LayerId;
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(36, 36); }
};

static void CollectTexts(const TSharedRef<SWidget>& Widget, TArray<FString>& Out)
{
	if (!Widget->GetVisibility().IsVisible())
	{
		return;
	}
	static const FName TextBlock(TEXT("STextBlock"));
	static const FName EditableText(TEXT("SEditableText"));
	if (Widget->GetType() == TextBlock)
	{
		const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		if (!Text.IsEmpty())
		{
			Out.Add(Text);
		}
		return;
	}
	if (Widget->GetType() == EditableText)
	{
		const FString Text = StaticCastSharedRef<SEditableText>(Widget)->GetText().ToString();
		if (!Text.IsEmpty())
		{
			Out.Add(Text);
		}
		return;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 I = 0; Children && I < Children->Num(); ++I)
	{
		CollectTexts(Children->GetChildAt(I), Out);
	}
}

static FName ButtonTextStyle(FName Variation)
{
	if (Variation == TEXT("Scfg.Button.Small"))
	{
		return TEXT("Scfg.ButtonText.Small");
	}
	if (Variation == TEXT("Scfg.Button.Primary"))
	{
		return TEXT("Scfg.ButtonText.Primary");
	}
	if (Variation == TEXT("Scfg.Button.Accent"))
	{
		return TEXT("Scfg.ButtonText.Accent");
	}
	return TEXT("Scfg.ButtonText");
}

// Widget is Ancestor or below it (Slate's parent links of the built tree).
static bool IsInside(const TSharedRef<SWidget>& Widget, const TSharedRef<SWidget>& Ancestor)
{
	for (TSharedPtr<SWidget> W = Widget; W.IsValid(); W = W->GetParentWidget())
	{
		if (W == Ancestor)
		{
			return true;
		}
	}
	return false;
}

static bool IsUp(const FKey& K)
{
	return K == EKeys::Up || K == EKeys::Gamepad_DPad_Up;
}

static bool IsDown(const FKey& K)
{
	return K == EKeys::Down || K == EKeys::Gamepad_DPad_Down;
}

}  // namespace ScfgScreenDetail

SScfgScreen::~SScfgScreen()
{
	ScfgScreenDetail::Registry().RemoveAll([this](const TWeakPtr<SScfgScreen>& W) { return !W.IsValid() || W.Pin().Get() == this; });
}

void SScfgScreen::ConstructScreen(const TSharedRef<FScfgStyle>& InStyle)
{
	Style = InStyle;
	SetCanTick(true);
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style->Brush(TEXT("Scfg.Dim")))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		[
			SNew(SBox)
			.WidthOverride(this, &SScfgScreen::PanelWidth)
			[
				SNew(SBorder)
				.BorderImage(Style->Brush(TEXT("Scfg.Sheet")))
				.Padding(FMargin(44, 32, 44, 28))
				[
					SAssignNew(Layout, SVerticalBox)
				]
			]
		]
	];
}

FOptionalSize SScfgScreen::PanelWidth() const
{
	return FOptionalSize(FScfgStyle::SheetWidthFor(LastSize));
}

// --- the host's part ------------------------------------------------------------------------------------------------

TArray<TSharedRef<SScfgScreen>> SScfgScreen::OpenScreens()
{
	TArray<TSharedRef<SScfgScreen>> Out;
	for (const TWeakPtr<SScfgScreen>& W : ScfgScreenDetail::Registry())
	{
		if (const TSharedPtr<SScfgScreen> S = W.Pin())
		{
			Out.Add(S.ToSharedRef());
		}
	}
	return Out;
}

void SScfgScreen::TakeMouse()
{
	FSlateApplication& App = FSlateApplication::Get();
	const int32 MouseUser = App.GetUserIndexForMouse();
	const TSharedPtr<FSlateUser> User = App.GetUser(MouseUser);
	if (!User.IsValid())
	{
		return;
	}
	const TSharedPtr<SWidget> Captor = User->HasCursorCapture() ? User->GetCursorCaptor() : nullptr;
	const TOptional<FScfgMouseWish> Wish = MouseWish ? TOptional<FScfgMouseWish>(MouseWish()) : TOptional<FScfgMouseWish>();
	if (Captor.IsValid() && !ScfgScreenDetail::IsInside(Captor.ToSharedRef(), AsShared()))
	{
		const TSharedPtr<GenericApplication> Platform = App.GetPlatformApplication();
		MouseCaptor = Captor;
		bMouseHeld = true;
		bMouseHighPrecision = Platform.IsValid() && Platform->IsUsingHighPrecisionMouseMode();
		bMouseLocked = bMouseLocked || CaptorLocks.Get(bMouseHighPrecision);
	}
	// A cursor locked without a capture (a LockAlways viewport): released below, so given back too.
	if (Wish.IsSet() && Wish->bLock && Wish->Viewport.IsValid())
	{
		bMouseHeld = true;
		bMouseLocked = true;
		if (!MouseCaptor.IsValid())
		{
			MouseCaptor = Wish->Viewport;
		}
	}
	// The reply releases the capture, high-precision mode and the cursor lock as an event would (SlateApplication.cpp's
	// ProcessReply); on a path to this screen, which is on top of the viewport now.
	FWidgetPath Path;
	if (!App.FindPathToWidget(AsShared(), Path) || !Path.IsValid())
	{
		if (Captor.IsValid())
		{
			App.FindPathToWidget(Captor.ToSharedRef(), Path);
		}
	}
	if (Path.IsValid())
	{
		App.ProcessReply(Path, FReply::Handled().ReleaseMouseCapture().ReleaseMouseLock(), nullptr, nullptr, MouseUser);
	}
	else
	{
		App.ReleaseAllPointerCapture(MouseUser);
	}
}

void SScfgScreen::GiveMouseBack()
{
	if (!bMouseHeld)
	{
		return;
	}
	bMouseHeld = false;
	TSharedPtr<SWidget> Target = MouseCaptor.Pin();
	MouseCaptor.Reset();
	const bool bLockedWhenTaken = bMouseLocked;
	bMouseLocked = false;
	// What the game wants now, if its viewport can say (it may have changed its input mode while the screen was open);
	// else what was recorded.
	bool bCapture = Target.IsValid();
	bool bHighPrecision = bMouseHighPrecision;
	bool bLock = bLockedWhenTaken;
	if (MouseWish)
	{
		const FScfgMouseWish Wish = MouseWish();
		if (Wish.Viewport.IsValid() && (!Target.IsValid() || Target == Wish.Viewport))
		{
			Target = Wish.Viewport;
			bCapture = Wish.bCapture;
			bHighPrecision = Wish.bHighPrecision;
			bLock = Wish.bLock;
		}
	}
	FSlateApplication& App = FSlateApplication::Get();
	FWidgetPath Path;
	if (!Target.IsValid() || (!bCapture && !bLock) || !App.FindPathToWidget(Target.ToSharedRef(), Path) || !Path.IsValid())
	{
		return;  // the game wants neither now, or its widget is gone (the level changed)
	}
	FReply Reply = FReply::Handled();
	if (bCapture && bHighPrecision)
	{
		Reply.UseHighPrecisionMouseMovement(Target.ToSharedRef());
	}
	else if (bCapture)
	{
		Reply.CaptureMouse(Target.ToSharedRef());
	}
	if (bLock)
	{
		Reply.LockMouseToWidget(Target.ToSharedRef());
	}
	App.ProcessReply(Path, Reply, nullptr, nullptr, App.GetUserIndexForMouse());
}

void SScfgScreen::OnShown()
{
	const TSharedRef<SScfgScreen> Self = StaticCastSharedRef<SScfgScreen>(AsShared());
	if (FSlateApplication::IsInitialized())
	{
		TakeMouse();
		FSlateApplication::Get().ForEachUser([this](FSlateUser& User)
		{
			const TSharedPtr<SWidget> Had = User.GetFocusedWidget();
			if (!Had.IsValid() || HasFocusWithin(User.GetUserIndex()))
			{
				return;
			}
			BackFocus.Add(User.GetUserIndex(), Had);
			if (User.GetUserIndex() != 0)
			{
				return;
			}
			for (const TSharedRef<SScfgScreen>& Below : OpenScreens())
			{
				if (&Below.Get() != this && Below->HasFocusWithin(0))
				{
					BackScreen = Below;
					BackKey = Below->FocusedKey();
				}
			}
		});
	}
	ScfgScreenDetail::Registry().RemoveAll([this](const TWeakPtr<SScfgScreen>& W) { return !W.IsValid() || W.Pin().Get() == this; });
	ScfgScreenDetail::Registry().Add(Self);
	Refresh();
	// shown again (a UMG host re-adding its widget): nothing to rebuild, but the focus is taken all the same
	if (!bEnded && LastSignature.IsSet() && !Covered())
	{
		FocusKeyOrPrimary(CurrentKey);
	}
}

void SScfgScreen::OnHidden(bool bRestoreFocus)
{
	const TSharedRef<SScfgScreen> Self = StaticCastSharedRef<SScfgScreen>(AsShared());
	ScfgScreenDetail::Registry().RemoveAll([this](const TWeakPtr<SScfgScreen>& W) { return !W.IsValid() || W.Pin().Get() == this; });
	if (!bRestoreFocus || !FSlateApplication::IsInitialized())
	{
		return;
	}
	// A screen drawn over this one keeps the focus (else the keyboard would reach the game behind it); if it opened
	// over this one, it inherits where this one would give the focus back.
	for (const TSharedRef<SScfgScreen>& Top : OpenScreens())
	{
		if (!Top->bEnded && Top->HasFocusWithin(0))
		{
			if (Top->BackScreen.Pin() == Self)
			{
				Top->BackFocus = BackFocus;
				Top->BackScreen = BackScreen;
				Top->BackKey = BackKey;
			}
			if (!Top->bMouseHeld && bMouseHeld)
			{
				Top->MouseCaptor = MouseCaptor;
				Top->bMouseHeld = true;
				Top->bMouseHighPrecision = bMouseHighPrecision;
				Top->bMouseLocked = bMouseLocked;
				bMouseHeld = false;
			}
			return;
		}
	}
	FSlateApplication& App = FSlateApplication::Get();
	bool bRestored = false;
	for (const TPair<int32, TWeakPtr<SWidget>>& Back : BackFocus)
	{
		const TSharedPtr<SWidget> Widget = Back.Value.Pin();
		FWidgetPath Path;
		if (Widget.IsValid() && App.FindPathToWidget(Widget.ToSharedRef(), Path) && Path.IsValid())
		{
			App.SetUserFocus(Back.Key, Widget, EFocusCause::SetDirectly);
			bRestored |= Back.Key == 0;
		}
	}
	if (!bRestored)
	{
		// The control is gone (the screen below rebuilt meanwhile): the same key in its new build.
		const TSharedPtr<SScfgScreen> Below = BackScreen.Pin();
		if (Below.IsValid() && !Below->bEnded && ScfgScreenDetail::Registry().ContainsByPredicate(
				[&Below](const TWeakPtr<SScfgScreen>& W) { return W.Pin() == Below; }))
		{
			Below->FocusKeyOrPrimary(BackKey);
		}
	}
	// Nowhere to give it back to (the game's widget is gone too): a focus left on this screen, which is leaving the
	// viewport, would keep the keys from everyone.
	App.ForEachUser([this, &App](FSlateUser& User)
	{
		if (HasFocusWithin(User.GetUserIndex()))
		{
			App.ClearUserFocus(User.GetUserIndex(), EFocusCause::SetDirectly);
		}
	});
	GiveMouseBack();
}

bool SScfgScreen::IsOpen() const
{
	return ScfgScreenDetail::Registry().ContainsByPredicate(
		[this](const TWeakPtr<SScfgScreen>& W) { return W.Pin().Get() == this; });
}

void SScfgScreen::CloseWith(const FString& How)
{
	if (bEnded)
	{
		return;
	}
	bEnded = true;
	if (const TSharedPtr<FScfgScreen> S = Screen.Pin())
	{
		S->Close(How);
	}
}

// --- the build ------------------------------------------------------------------------------------------------------

void SScfgScreen::Refresh()
{
	if (bEnded || !Layout.IsValid() || !ReadyToShow())
	{
		return;
	}
	Sync();
	const FString Sig = Signature();
	// case-sensitive: a Solana signature differs from another by case alone (FString's == ignores case)
	if (LastSignature.IsSet() && LastSignature->Equals(Sig, ESearchCase::CaseSensitive))
	{
		return;
	}
	ResyncFocus();
	const FString Keep = CurrentKey;
	const FString Step = StepKey();
	const bool bMoved = !LastSignature.IsSet() || !Step.Equals(LastStep, ESearchCase::CaseSensitive);
	LastSignature = Sig;
	LastStep = Step;
	Layout->ClearChildren();
	Entries.Reset();
	Keyed.Reset();
	Fields.Reset();
	Scrolls.Reset();
	CurrentKey.Reset();
	BuildContent();
	++Builds;
	if (!Covered())
	{
		FocusKeyOrPrimary(bMoved ? FString() : Keep);
	}
}

void SScfgScreen::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	LastSize = FVector2D(AllottedGeometry.GetLocalSize());
	if (bShowWhenPainted && !bEnded && !IsOpen())
	{
		OnShown();
	}
	Refresh();
	if (bFocusPending && !bEnded && !Covered())
	{
		FocusKeyOrPrimary(CurrentKey);
	}
	KeepInput();
	ResyncFocus();
}

bool SScfgScreen::IsTopmost() const
{
	const TArray<TSharedRef<SScfgScreen>> Open = OpenScreens();
	for (int32 I = Open.Num() - 1; I >= 0; --I)
	{
		if (!Open[I]->bEnded)
		{
			return &Open[I].Get() == this;
		}
	}
	return false;
}

void SScfgScreen::KeepInput()
{
	if (bEnded || !FSlateApplication::IsInitialized() || !IsOpen() || !IsTopmost())
	{
		return;
	}
	FSlateApplication& App = FSlateApplication::Get();
	const TSharedPtr<FSlateUser> Mouse = App.GetUser(App.GetUserIndexForMouse());
	const TSharedPtr<SWidget> Captor = Mouse.IsValid() && Mouse->HasCursorCapture() ? Mouse->GetCursorCaptor() : nullptr;
	if (Captor.IsValid() && !ScfgScreenDetail::IsInside(Captor.ToSharedRef(), AsShared()))
	{
		TakeMouse();  // the game viewport captured again (window activated, SetInputMode under the sheet)
	}
	if (!HasFocusWithin(0) && !bFocusPending)
	{
		FocusKeyOrPrimary(CurrentKey);
	}
}

TSharedRef<STextBlock> SScfgScreen::Label(const FString& Text, FName StyleName, bool bWrap)
{
	return SNew(STextBlock)
		.Text(FText::FromString(Text))
		.TextStyle(&Style->Text(StyleName))
		.AutoWrapText(bWrap);
}

TSharedRef<STextBlock> SScfgScreen::Mono(const FString& Text)
{
	return SNew(STextBlock)
		.Text(FText::FromString(Text))
		.TextStyle(&Style->Text(TEXT("Scfg.Mono")))
		.AutoWrapText(true)
		.WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping);
}

TSharedRef<SWidget> SScfgScreen::Ring(const FString& Key, const TSharedRef<SWidget>& Inner)
{
	TWeakPtr<SScfgScreen> Weak = StaticCastSharedRef<SScfgScreen>(AsShared());
	const FSlateBrush* On = Style->Brush(TEXT("Scfg.Focus"));
	const FSlateBrush* Off = Style->Brush(TEXT("Scfg.NoFocus"));
	return SNew(SBorder)
		.Padding(FMargin(2))
		.BorderImage_Lambda([Weak, Key, On, Off]()
		{
			const TSharedPtr<SScfgScreen> S = Weak.Pin();
			return S.IsValid() && !S->bEnded && S->CurrentKey == Key ? On : Off;
		})
		[
			Inner
		];
}

void SScfgScreen::AddEntry(FEntry Entry)
{
	Keyed.Add(Entry.Key, Entry.Focus);
	Entries.Add(MoveTemp(Entry));
}

TSharedRef<SWidget> SScfgScreen::Button(const FString& Key, const FString& Text, FName Variation,
	TFunction<void()> OnPress, const FString& Row, bool bEnabled, bool bVisible)
{
	TSharedPtr<SButton> B;
	const TSharedRef<SWidget> Outer = Ring(Key,
		SAssignNew(B, SButton)
		.ButtonStyle(&Style->Button(Variation))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.IsFocusable(true)
		.IsEnabled(bEnabled)
		.OnClicked_Lambda([OnPress]()
		{
			if (OnPress)
			{
				OnPress();
			}
			return FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Text(FText::FromString(Text))
			.TextStyle(&Style->Text(ScfgScreenDetail::ButtonTextStyle(Variation)))
			.Justification(ETextJustify::Center)
		]);
	Outer->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	FEntry Entry{Key, B.ToSharedRef(), Row, MoveTemp(OnPress)};
	Entry.bEnabled = bEnabled;
	Entry.bVisible = bVisible;
	AddEntry(MoveTemp(Entry));
	return Outer;
}

TSharedRef<SWidget> SScfgScreen::Option(const FString& Key, const FString& Text, const FString& Sub, bool bSelected,
	TFunction<void()> OnPress, const FString& Row)
{
	const FString Suffix = bSelected ? TEXT(".Selected") : TEXT("");
	const TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
	Lines->AddSlot().AutoHeight().HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Text))
		.TextStyle(&Style->Text(*(TEXT("Scfg.OptionText") + Suffix)))
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	];
	if (!Sub.IsEmpty())
	{
		Lines->AddSlot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Sub))
			.TextStyle(&Style->Text(*(TEXT("Scfg.OptionSub") + Suffix)))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		];
	}
	TSharedPtr<SButton> B;
	const TSharedRef<SWidget> Outer = Ring(Key,
		SAssignNew(B, SButton)
		.ButtonStyle(&Style->Button(bSelected ? TEXT("Scfg.Option.Selected") : TEXT("Scfg.Option")))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.IsFocusable(true)
		.OnClicked_Lambda([OnPress]()
		{
			if (OnPress)
			{
				OnPress();
			}
			return FReply::Handled();
		})
		[
			Lines
		]);
	AddEntry(FEntry{Key, B.ToSharedRef(), Row, MoveTemp(OnPress)});
	return Outer;
}

TSharedRef<SWidget> SScfgScreen::Field(const FString& Key, const FString& Text, int32 MaxLength,
	const FString& Allowed, TFunction<void(const FString&)> OnText, TFunction<void()> OnSubmit, const FString& Row,
	bool bReadOnly)
{
	TWeakPtr<SScfgScreen> Weak = StaticCastSharedRef<SScfgScreen>(AsShared());
	TSharedPtr<SEditableTextBox> Box;
	const TSharedRef<SWidget> Outer = Ring(Key,
		SAssignNew(Box, SEditableTextBox)
		.Style(&Style->Field())
		.Text(FText::FromString(Text))
		.IsReadOnly(bReadOnly)
		.IsCaretMovedWhenGainFocus(true)
		.SelectAllTextWhenFocused(false)
		.ClearKeyboardFocusOnCommit(false)
		.RevertTextOnEscape(false)
		.AllowContextMenu(true)
		.MaximumLength(MaxLength)
		.OnTextChanged_Lambda([Weak, Key, MaxLength, Allowed, OnText](const FText& New)
		{
			const TSharedPtr<SScfgScreen> S = Weak.Pin();
			if (!S.IsValid() || S->bSettingField)
			{
				return;
			}
			const FString Raw = New.ToString();
			FString Clean;
			for (const TCHAR C : Raw)
			{
				int32 At = INDEX_NONE;
				if (Allowed.FindChar(C, At) && Clean.Len() < MaxLength)
				{
					Clean.AppendChar(C);
				}
			}
			if (!Clean.Equals(Raw, ESearchCase::CaseSensitive))
			{
				S->SetFieldText(Key, Clean);
			}
			if (OnText)
			{
				OnText(Clean);
			}
		}));
	Fields.Add(Key, Box.ToSharedRef());
	FEntry Entry{Key, Box.ToSharedRef(), Row, MoveTemp(OnSubmit)};
	Entry.bField = true;
	AddEntry(MoveTemp(Entry));
	return Outer;
}

void SScfgScreen::SetFieldText(const FString& Key, const FString& Text)
{
	if (const TSharedRef<SEditableTextBox>* Box = Fields.Find(Key))
	{
		TGuardValue<bool> Guard(bSettingField, true);
		(*Box)->SetText(FText::FromString(Text));
	}
}

TSharedRef<SWidget> SScfgScreen::BackArrow(const FString& Key, TFunction<void()> OnPress, bool bVisible)
{
	TSharedPtr<SButton> B;
	const TSharedRef<SWidget> Outer = Ring(Key,
		SNew(SBox)
		.WidthOverride(36)
		.HeightOverride(36)
		[
			SAssignNew(B, SButton)
			.ButtonStyle(&Style->Button(TEXT("Scfg.Button.Back")))
			.ToolTipText(FText::FromString(ScfgTexts::Get(TEXT("back"))))
			.IsFocusable(true)
			.ContentPadding(FMargin(0))
			.OnClicked_Lambda([OnPress]()
			{
				if (OnPress)
				{
					OnPress();
				}
				return FReply::Handled();
			})
			[
				SNew(ScfgScreenDetail::SArrow)
			]
		]);
	Outer->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	FEntry Entry{Key, B.ToSharedRef(), FString(), MoveTemp(OnPress)};
	Entry.bVisible = bVisible;
	AddEntry(MoveTemp(Entry));
	return Outer;
}

TSharedRef<SWidget> SScfgScreen::Reg(const FString& Key, const TSharedRef<SWidget>& Widget)
{
	Keyed.Add(Key, Widget);
	return Widget;
}

TSharedRef<SWidget> SScfgScreen::Rule() const
{
	return SNew(SBox).HeightOverride(1.5f)[SNew(SImage).Image(Style->Brush(TEXT("Scfg.Rule")))];
}

TSharedRef<SWidget> SScfgScreen::Dot(const FLinearColor& Color) const
{
	return SNew(SBox)
		.WidthOverride(FScfgStyle::DotSize)
		.HeightOverride(FScfgStyle::DotSize)
		.VAlign(VAlign_Center)
		[
			SNew(SImage).Image(Style->Brush(TEXT("Scfg.Dot"))).ColorAndOpacity(FSlateColor(Color))
		];
}

TSharedRef<SWidget> SScfgScreen::StatusRow(const FString& Text, const FLinearColor& Color)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)[Dot(Color)]
		+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(Text, TEXT("Scfg.Status"))];
}

TSharedPtr<SWidget> SScfgScreen::Logo()
{
	if (!Style->HasLogo())
	{
		return nullptr;
	}
	const FVector2D Fit = Style->LogoFitted();
	return Reg(TEXT("logo"),
		SNew(SBox)
		.HAlign(HAlign_Left)
		.WidthOverride(float(Fit.X))
		.HeightOverride(float(Fit.Y))
		[
			SNew(SImage).Image(Style->Brush(TEXT("Scfg.Logo")))
		]);
}

TSharedRef<SWidget> SScfgScreen::ScrollingBody(TSharedPtr<SVerticalBox>& OutBody)
{
	TSharedPtr<SScrollBox> Scroll;
	SAssignNew(Scroll, SScrollBox)
	.Orientation(Orient_Vertical)
	+ SScrollBox::Slot()
	[
		SAssignNew(OutBody, SVerticalBox)
	];
	Scrolls.Add(Scroll);
	return Scroll.ToSharedRef();
}

// --- focus ----------------------------------------------------------------------------------------------------------

const SScfgScreen::FEntry* SScfgScreen::Find(const FString& Key) const
{
	return Entries.FindByPredicate([&Key](const FEntry& E) { return E.Key == Key; });
}

TArray<const SScfgScreen::FEntry*> SScfgScreen::Shown() const
{
	TArray<const FEntry*> Out;
	for (const FEntry& E : Entries)
	{
		if (E.bVisible)
		{
			Out.Add(&E);
		}
	}
	return Out;
}

TArray<FString> SScfgScreen::FocusOrder() const
{
	TArray<FString> Out;
	for (const FEntry* E : Shown())
	{
		Out.Add(E->Key);
	}
	return Out;
}

bool SScfgScreen::HasFocusWithin(int32 UserIndex) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(UserIndex);
	return Focused.IsValid() && (Focused.Get() == this || HasUserFocusedDescendants(UserIndex));
}

void SScfgScreen::ResyncFocus()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0);
	if (!Focused.IsValid())
	{
		return;
	}
	for (const FEntry& E : Entries)
	{
		if (Focused == E.Focus || E.Focus->HasUserFocusedDescendants(0))
		{
			CurrentKey = E.Key;
			return;
		}
	}
}

FString SScfgScreen::FocusedKey() const
{
	return CurrentKey;
}

void SScfgScreen::FocusEntry(const FEntry& Entry)
{
	CurrentKey = Entry.Key;
	bFocusPending = false;
	if (FSlateApplication::IsInitialized())
	{
		FWidgetPath Path;
		if (FSlateApplication::Get().FindPathToWidget(Entry.Focus, Path) && Path.IsValid())
		{
			// The path form: the widget form searches the native windows only, not the virtual ones (a widget
			// component, a test).
			FSlateApplication::Get().SetAllUserFocus(Path, EFocusCause::Navigation);
		}
		else
		{
			bFocusPending = true;  // not reachable yet (no window, not laid out): the next tick tries again
		}
	}
	for (const TSharedPtr<SScrollBox>& Scroll : Scrolls)
	{
		if (ScfgScreenDetail::IsInside(Entry.Focus, Scroll.ToSharedRef()))
		{
			Scroll->ScrollDescendantIntoView(Entry.Focus, false, EDescendantScrollDestination::IntoView);
		}
	}
}

void SScfgScreen::FocusKey(const FString& Key)
{
	if (const FEntry* E = Find(Key))
	{
		FocusEntry(*E);
	}
}

void SScfgScreen::FocusKeyOrPrimary(const FString& Keep)
{
	const TArray<const FEntry*> Order = Shown();
	const FEntry* Target = nullptr;
	for (const FString& Key : {Keep, PrimaryKey()})
	{
		if (Key.IsEmpty())
		{
			continue;
		}
		const FEntry* const* Found = Order.FindByPredicate([&Key](const FEntry* E) { return E->Key == Key; });
		if (Found)
		{
			Target = *Found;
			break;
		}
	}
	if (!Target && Order.Num() > 0)
	{
		Target = Order[0];
	}
	if (Target)
	{
		FocusEntry(*Target);
	}
}

bool SScfgScreen::Covered() const
{
	bool bAfter = false;
	for (const TSharedRef<SScfgScreen>& S : OpenScreens())
	{
		if (&S.Get() == this)
		{
			bAfter = true;
			continue;
		}
		if (bAfter && !S->bEnded && S->HasFocusWithin(0))
		{
			return true;
		}
	}
	return false;
}

void SScfgScreen::Move(EUINavigation Direction)
{
	if (const FEntry* To = Target(Direction))
	{
		FocusEntry(*To);
	}
}

const SScfgScreen::FEntry* SScfgScreen::Target(EUINavigation Direction)
{
	const TArray<const FEntry*> Order = Shown();
	const int32 N = Order.Num();
	if (N == 0)
	{
		return nullptr;
	}
	ResyncFocus();
	int32 I = Order.IndexOfByPredicate([this](const FEntry* E) { return E->Key == CurrentKey; });
	if (I == INDEX_NONE)
	{
		const FString Primary = PrimaryKey();
		const FEntry* const* Found = Order.FindByPredicate([&Primary](const FEntry* E) { return E->Key == Primary; });
		return Found ? *Found : Order[0];
	}
	int32 To = I;
	switch (Direction)
	{
	case EUINavigation::Up:
		To = FMath::Max(0, I - 1);
		break;
	case EUINavigation::Down:
		To = FMath::Min(N - 1, I + 1);
		break;
	case EUINavigation::Next:
		To = (I + 1) % N;
		break;
	case EUINavigation::Previous:
		To = (I - 1 + N) % N;
		break;
	case EUINavigation::Left:
		if (!Order[I]->Row.IsEmpty() && I > 0 && Order[I - 1]->Row == Order[I]->Row)
		{
			To = I - 1;
		}
		break;
	case EUINavigation::Right:
		if (!Order[I]->Row.IsEmpty() && I < N - 1 && Order[I + 1]->Row == Order[I]->Row)
		{
			To = I + 1;
		}
		break;
	default:
		break;
	}
	return Order[To];
}

bool SScfgScreen::HandleKey(const FKeyEvent& Event)
{
	if (bEnded)
	{
		return true;  // closed, waiting for the host to remove it: nothing more, and nothing for the game
	}
	ResyncFocus();
	const FEntry* Current = Find(CurrentKey);
	const bool bField = Current && Current->bField;
	const FKey Key = Event.GetKey();
	EUINavigationAction Action = EUINavigationAction::Invalid;
	EUINavigation Direction = EUINavigation::Invalid;
	if (FSlateApplication::IsInitialized())
	{
		Action = FSlateApplication::Get().GetNavigationActionFromKey(Event);
		Direction = FSlateApplication::Get().GetNavigationDirectionFromKey(Event);
	}
	// back: once per press, never on a held key's repeats (Godot's ui_cancel with allow_echo false)
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right
		|| (!bField && Action == EUINavigationAction::Back))
	{
		if (!Event.IsRepeat())
		{
			OnBack();
		}
		return true;
	}
	// confirm
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom
		|| (!bField && (Key == EKeys::SpaceBar || Action == EUINavigationAction::Accept)))
	{
		if (Current && Current->bVisible && Current->bEnabled && Current->Activate && !Event.IsRepeat())
		{
			Current->Activate();
		}
		return true;
	}
	// navigation: in a field only the real arrow keys up and down, Tab and the d-pad (letters are text)
	EUINavigation Move = EUINavigation::Invalid;
	if (ScfgScreenDetail::IsUp(Key))
	{
		Move = EUINavigation::Up;
	}
	else if (ScfgScreenDetail::IsDown(Key))
	{
		Move = EUINavigation::Down;
	}
	else if (Key == EKeys::Tab)
	{
		Move = Event.IsShiftDown() ? EUINavigation::Previous : EUINavigation::Next;
	}
	else if (Key == EKeys::Gamepad_DPad_Left || (!bField && Key == EKeys::Left))
	{
		Move = EUINavigation::Left;
	}
	else if (Key == EKeys::Gamepad_DPad_Right || (!bField && Key == EKeys::Right))
	{
		Move = EUINavigation::Right;
	}
	else if (!bField && Direction != EUINavigation::Invalid)
	{
		Move = Direction;
	}
	if (Move != EUINavigation::Invalid)
	{
		this->Move(Move);
		return true;
	}
	return false;
}

// --- input ----------------------------------------------------------------------------------------------------------

FReply SScfgScreen::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	// The root itself got the focus (a host focused the screen): pass it to the current control.
	if (const FEntry* E = Find(CurrentKey))
	{
		return FReply::Handled().SetUserFocus(E->Focus, EFocusCause::SetDirectly);
	}
	return FReply::Handled();
}

FNavigationReply SScfgScreen::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	// Slate's own navigation (an input preprocessor's, a stick the engine turned into navigation) follows the screen's
	// order too, and never leaves it.
	if (!bEnded)
	{
		if (const FEntry* To = Target(InNavigationEvent.GetNavigationType()))
		{
			CurrentKey = To->Key;
			bFocusPending = false;
			return FNavigationReply::Explicit(To->Focus);
		}
	}
	return FNavigationReply::Stop();
}

FReply SScfgScreen::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return HandleKey(InKeyEvent) ? FReply::Handled() : FReply::Unhandled();
}

FReply SScfgScreen::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return FReply::Handled();  // whatever the focused control left: not for the game
}

FReply SScfgScreen::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return FReply::Handled();
}

FReply SScfgScreen::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	return FReply::Handled();
}

FReply SScfgScreen::OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent)
{
	const FKey Key = InAnalogInputEvent.GetKey();
	const float V = InAnalogInputEvent.GetAnalogValue();
	if (!bEnded && (Key == EKeys::Gamepad_LeftY || Key == EKeys::Gamepad_LeftX))
	{
		const bool bY = Key == EKeys::Gamepad_LeftY;
		bool& bLatched = bY ? bStickY : bStickX;
		if (FMath::Abs(V) < 0.3f)
		{
			bLatched = false;
		}
		else if (FMath::Abs(V) > 0.5f && !bLatched)
		{
			bLatched = true;
			// the stick's Y is positive upwards
			Move(bY ? (V > 0 ? EUINavigation::Up : EUINavigation::Down) : (V > 0 ? EUINavigation::Right : EUINavigation::Left));
		}
	}
	return FReply::Handled();
}

FReply SScfgScreen::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();  // the dimmed backdrop stops the click
}

FReply SScfgScreen::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();
}

FReply SScfgScreen::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();
}

FReply SScfgScreen::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();
}

FReply SScfgScreen::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();
}

FCursorReply SScfgScreen::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	return FCursorReply::Cursor(EMouseCursor::Default);  // shown over the screen, whatever the game's viewport says
}

// --- readers --------------------------------------------------------------------------------------------------------

TSharedPtr<SWidget> SScfgScreen::Control(const FString& Key) const
{
	const TSharedRef<SWidget>* W = Keyed.Find(Key);
	return W ? TSharedPtr<SWidget>(*W) : nullptr;
}

bool SScfgScreen::IsShown(const FString& Key) const
{
	if (const FEntry* E = Find(Key))
	{
		return E->bVisible;
	}
	return Keyed.Contains(Key);
}

bool SScfgScreen::Press(const FString& Key)
{
	const FEntry* E = Find(Key);
	if (!E || !E->bVisible || !E->bEnabled || !E->Activate || bEnded)
	{
		return false;
	}
	E->Activate();
	return true;
}

FString SScfgScreen::TextOf(const FString& Key) const
{
	const TSharedPtr<SWidget> W = Control(Key);
	if (!W.IsValid())
	{
		return FString();
	}
	if (const TSharedRef<SEditableTextBox>* Box = Fields.Find(Key))
	{
		return (*Box)->GetText().ToString();
	}
	TArray<FString> Texts;
	ScfgScreenDetail::CollectTexts(W.ToSharedRef(), Texts);
	return FString::Join(Texts, TEXT(" "));
}

TArray<FString> SScfgScreen::VisibleTexts() const
{
	TArray<FString> Out;
	if (Layout.IsValid())
	{
		ScfgScreenDetail::CollectTexts(Layout.ToSharedRef(), Out);
	}
	return Out;
}
