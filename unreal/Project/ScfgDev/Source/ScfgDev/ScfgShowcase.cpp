// No Python twin: godot/demo/main.gd's Unreal form (see ScfgShowcase.h).
#include "ScfgShowcase.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgDevLog.h"
#include "ScfgSubsystem.h"
#if !UE_BUILD_SHIPPING
#include "ScfgE2EDriver.h"
#endif

namespace ScfgShowcaseDetail
{
const TCHAR* const LOCKED = TEXT("Chapter 2: locked");
const TCHAR* const ITEM_TITLE = TEXT("Chapter 2: The Fall");
// The public EIP-55 example address (EIP-55's own test vector): not yours, so a real payment to it is lost. A game
// puts its own wallet here.
const TCHAR* const PLACEHOLDER_WALLET = TEXT("0x52908400098527886E0F7030069857D2E4169EE7");

// The status words of godot/demo (the core's status strings).
FString Word(EScfgPaymentStatus Status)
{
	switch (Status)
	{
	case EScfgPaymentStatus::Paid: return TEXT("paid");
	case EScfgPaymentStatus::AlreadyOwned: return TEXT("already_owned");
	case EScfgPaymentStatus::Cancelled: return TEXT("cancelled");
	case EScfgPaymentStatus::Expired: return TEXT("expired");
	case EScfgPaymentStatus::Underpaid: return TEXT("underpaid");
	case EScfgPaymentStatus::Failed: return TEXT("failed");
	case EScfgPaymentStatus::Pending: return TEXT("pending");
	}
	return TEXT("unknown");
}

FString Word(EScfgRestoreStatus Status)
{
	switch (Status)
	{
	case EScfgRestoreStatus::Restored: return TEXT("restored");
	case EScfgRestoreStatus::NothingFound: return TEXT("nothing_found");
	case EScfgRestoreStatus::Cancelled: return TEXT("cancelled");
	case EScfgRestoreStatus::Failed: return TEXT("failed");
	}
	return TEXT("unknown");
}
}

// --- the HUD ----------------------------------------------------------------------------------------------------------

void SScfgShowcaseHud::Construct(const FArguments& InArgs)
{
	const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 28);
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 16);
	auto Text = [this, &Body](const FString& S) -> TSharedRef<STextBlock>
	{
		Labels.Add(S);
		return SNew(STextBlock).Text(FText::FromString(S)).Font(Body).ColorAndOpacity(FLinearColor::White)
			.Visibility(EVisibility::HitTestInvisible);
	};
	// Only the buttons take clicks: a click anywhere else is the world's (the player controller counts them).
	SetVisibility(EVisibility::SelfHitTestInvisible);
	const TSharedRef<SVerticalBox> Column = SNew(SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible);
	Labels.Add(TEXT("Stablecoin for Games Showcase"));
	Column->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
	[
		SNew(STextBlock).Text(FText::FromString(Labels.Last())).Font(Title).ColorAndOpacity(FLinearColor::White)
			.Visibility(EVisibility::HitTestInvisible)
	];
	Column->AddSlot().AutoHeight().Padding(0, 0, 0, 16)[Text(TEXT("This build runs on testnets, so no real money is involved."))];
	Column->AddSlot().AutoHeight().Padding(0, 0, 0, 16)[SAssignNew(Chapter, STextBlock).Visibility(EVisibility::HitTestInvisible).Font(Body).ColorAndOpacity(FLinearColor::White)];
	Column->AddSlot().AutoHeight().Padding(0, 4)[MakeButton(TEXT("Unlock"), TEXT("Unlock Chapter 2 (1.00 USDC)"), InArgs._OnUnlock)];
	Column->AddSlot().AutoHeight().Padding(0, 4)[MakeButton(TEXT("Tip"), TEXT("Tip the developer (1.00 USDC)"), InArgs._OnTip)];
	Column->AddSlot().AutoHeight().Padding(0, 4)[MakeButton(TEXT("Restore"), TEXT("Restore purchases"), InArgs._OnRestore)];
	Column->AddSlot().AutoHeight().Padding(0, 16, 0, 0)[SAssignNew(Status, STextBlock).Visibility(EVisibility::HitTestInvisible).Font(Body).ColorAndOpacity(FLinearColor::White).AutoWrapText(true)];
	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Center)
	.Padding(80, 0, 0, 0)
	[
		SNew(SBox).WidthOverride(560).Visibility(EVisibility::SelfHitTestInvisible)[Column]
	];
}

TSharedRef<SButton> SScfgShowcaseHud::MakeButton(const FString& Name, const FString& Text, FSimpleDelegate OnClick)
{
	const TSharedRef<SButton> Button = SNew(SButton)
		.OnClicked_Lambda([OnClick]()
		{
			OnClick.ExecuteIfBound();
			return FReply::Handled();
		})
		.ContentPadding(FMargin(16, 8))
		[
			SNew(STextBlock).Text(FText::FromString(Text)).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16))
		];
	All.Add({Name, Text, Button});
	return Button;
}

void SScfgShowcaseHud::SetChapter(const FString& Text, bool bShowUnlock)
{
	Chapter->SetText(FText::FromString(Text));
	for (const FButtonView& B : All)
	{
		if (B.Name == TEXT("Unlock"))
		{
			B.Widget->SetVisibility(bShowUnlock ? EVisibility::Visible : EVisibility::Collapsed);
		}
	}
}

void SScfgShowcaseHud::SetStatus(const FString& Text)
{
	Status->SetText(FText::FromString(Text));
}

TArray<SScfgShowcaseHud::FButtonView> SScfgShowcaseHud::Buttons() const
{
	TArray<FButtonView> Out;
	for (const FButtonView& B : All)
	{
		if (B.Widget->GetVisibility().IsVisible())
		{
			Out.Add(B);
		}
	}
	return Out;
}

TArray<FString> SScfgShowcaseHud::Texts() const
{
	TArray<FString> Out = {Labels[0], Labels[1], Chapter->GetText().ToString()};
	for (const FButtonView& B : Buttons())
	{
		Out.Add(B.Text);
	}
	const FString S = Status->GetText().ToString();
	if (!S.IsEmpty())
	{
		Out.Add(S);
	}
	return Out;
}

TSharedPtr<SWidget> SScfgShowcaseHud::FirstButton() const
{
	const TArray<FButtonView> Shown = Buttons();
	return Shown.IsEmpty() ? nullptr : TSharedPtr<SWidget>(Shown[0].Widget);
}

// --- the player controller and game mode -----------------------------------------------------------------------------

bool AScfgShowcasePlayerController::InputKey(const FInputKeyEventArgs& Params)
{
	if (Params.Key == EKeys::LeftMouseButton && Params.Event == IE_Pressed)
	{
		++WorldClicks;
	}
	else if (Params.Key.IsValid() && !Params.Key.IsMouseButton() && !Params.Key.IsGamepadKey() && !Params.Key.IsAxis1D()
		&& Params.Event == IE_Pressed)
	{
		++WorldKeys;
	}
	return Super::InputKey(Params);
}

AScfgShowcaseGameMode::AScfgShowcaseGameMode()
{
	PlayerControllerClass = AScfgShowcasePlayerController::StaticClass();
}

// --- the game instance -------------------------------------------------------------------------------------------------

TSubclassOf<AGameModeBase> UScfgShowcaseGameInstance::OverrideGameModeClass(TSubclassOf<AGameModeBase> GameModeClass,
	const FString& MapName, const FString& Options, const FString& Portal) const
{
	return AScfgShowcaseGameMode::StaticClass();
}

UScfgSubsystem* UScfgShowcaseGameInstance::Scfg() const
{
	return GetSubsystem<UScfgSubsystem>();
}

AScfgShowcasePlayerController* UScfgShowcaseGameInstance::PlayerController() const
{
	return Cast<AScfgShowcasePlayerController>(GetFirstLocalPlayerController());
}

void UScfgShowcaseGameInstance::Init()
{
	Super::Init();
#if !UE_BUILD_SHIPPING
	FString ConfigPath;
	if (FScfgE2EDriver::Requested(ConfigPath))
	{
		Driver = FScfgE2EDriver::Create(ConfigPath, this);
		if (!Driver.IsValid())
		{
			Fail();  // the driver logged why
			return;
		}
	}
#endif
	if (!ConfigureScfg())
	{
		Fail();
		return;
	}
	UScfgSubsystem* S = Scfg();
	S->OnPaymentResolved.AddDynamic(this, &UScfgShowcaseGameInstance::HandlePaymentResolved);
	S->OnReceiptsChanged.AddDynamic(this, &UScfgShowcaseGameInstance::HandleReceiptsChanged);
#if !UE_BUILD_SHIPPING
	if (Driver.IsValid() && !Driver->Start())
	{
		Fail();
	}
#endif
}

bool UScfgShowcaseGameInstance::ConfigureScfg()
{
	UScfgSubsystem* S = Scfg();
	if (!S)
	{
		UE_LOG(LogScfgDev, Error, TEXT("showcase: the Scfg subsystem is missing"));
		return false;
	}
	FScfgOptions Options;
	TMap<FString, FScfgItem> Items;
#if !UE_BUILD_SHIPPING
	if (Driver.IsValid())
	{
		Options = Driver->Options();
		Items = Driver->Items();
	}
	else
#endif
	{
		Options.Wallet = ScfgShowcaseDetail::PLACEHOLDER_WALLET;
		Options.bTestnet = true;  // Sepolia / Arbitrum Sepolia / Amoy / Base Sepolia: no real money
		FScfgItem Item;
		Item.Code = 1;
		Item.Price = TEXT("1.00");
		Item.Token = TEXT("USDC");
		Item.Title = ScfgShowcaseDetail::ITEM_TITLE;
		Items.Add(ITEM, Item);
	}
	// Configure and AddItem return "" or the error they logged ("LogScfg: Error: scfg: ...").
	if (!S->Configure(Options).IsEmpty())
	{
		return false;
	}
	for (const TPair<FString, FScfgItem>& Item : Items)
	{
		if (!S->AddItem(Item.Key, Item.Value).IsEmpty())
		{
			return false;
		}
	}
	return true;
}

void UScfgShowcaseGameInstance::Fail()
{
	bFailed = true;
	Driver.Reset();
	UE_LOG(LogScfgDev, Error, TEXT("showcase: the setup failed, quitting with exit code 1"));
	FPlatformMisc::RequestExitWithStatus(false, 1, TEXT("ScfgShowcase"));
}

void UScfgShowcaseGameInstance::OnStart()
{
	Super::OnStart();
	if (bFailed)
	{
		return;
	}
	UGameViewportClient* Viewport = GetGameViewportClient();
	if (!Viewport)
	{
		UE_LOG(LogScfgDev, Warning, TEXT("showcase: no game viewport, no HUD"));
		return;
	}
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	SAssignNew(Hud, SScfgShowcaseHud)
		.OnUnlock_Lambda([Weak]() { if (Weak.IsValid()) Weak->Unlock(); })
		.OnTip_Lambda([Weak]() { if (Weak.IsValid()) Weak->Tip(); })
		.OnRestore_Lambda([Weak]() { if (Weak.IsValid()) Weak->Restore(); });
	Viewport->AddViewportWidgetContent(Hud.ToSharedRef(), 0);
	// A menu-style game: the cursor shown, the HUD's buttons clickable, the world still gets what the HUD does not take.
	if (APlayerController* PC = GetFirstLocalPlayerController())
	{
		PC->SetShowMouseCursor(true);
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
	}
	Refresh();
	FocusHud();
	UE_LOG(LogScfgDev, Log, TEXT("showcase: ready"));
}

void UScfgShowcaseGameInstance::Shutdown()
{
	Driver.Reset();
	if (UGameViewportClient* Viewport = GetGameViewportClient(); Viewport && Hud.IsValid())
	{
		Viewport->RemoveViewportWidgetContent(Hud.ToSharedRef());
	}
	Hud.Reset();
	Super::Shutdown();
}

void UScfgShowcaseGameInstance::SetStatus(const FString& Text)
{
	if (Hud.IsValid())
	{
		Hud->SetStatus(Text);
	}
}

void UScfgShowcaseGameInstance::Refresh()
{
	if (!Hud.IsValid())
	{
		return;
	}
	const bool bOwned = Scfg()->Owns(ITEM);
	Hud->SetChapter(bOwned ? UNLOCKED : ScfgShowcaseDetail::LOCKED, !bOwned);
}

void UScfgShowcaseGameInstance::FocusHud()
{
	// Only while the cursor is shown (the showcase's menu): a game-only input mode keeps the focus where the sheet
	// gave it back, on the game viewport.
	const APlayerController* PC = GetFirstLocalPlayerController();
	if (Hud.IsValid() && FSlateApplication::IsInitialized() && (!PC || PC->ShouldShowMouseCursor()))
	{
		if (const TSharedPtr<SWidget> First = Hud->FirstButton())
		{
			FSlateApplication::Get().SetUserFocus(0, First, EFocusCause::SetDirectly);
		}
	}
}

void UScfgShowcaseGameInstance::BeginSheet()
{
	if (Busy++ == 0)
	{
		bWasPaused = UGameplayStatics::IsGamePaused(this);
		UGameplayStatics::SetGamePaused(this, true);
	}
}

void UScfgShowcaseGameInstance::EndSheet()
{
	if (--Busy == 0)
	{
		UGameplayStatics::SetGamePaused(this, bWasPaused);
	}
}

void UScfgShowcaseGameInstance::Unlock()
{
	BeginSheet();
	FScfgPayRequest Request;
	Request.Item = ITEM;
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	Scfg()->PayAsync(EScfgPayKind::Unlock, Request, [Weak](const FScfgPaymentResult& Result)
	{
		UScfgShowcaseGameInstance* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->EndSheet();
		Self->Refresh();
		Self->FocusHud();
		switch (Result.Status)
		{
		case EScfgPaymentStatus::Paid:
		case EScfgPaymentStatus::AlreadyOwned:
			Self->SetStatus(TEXT("Payment confirmed. Thank you!"));
			break;
		case EScfgPaymentStatus::Pending:
			Self->SetStatus(TEXT("No problem, keep playing. Chapter 2 unlocks by itself when the payment arrives."));
			break;
		default:
			Self->SetStatus(FString::Printf(TEXT("Maybe later. (Status: %s)"),
				*ScfgShowcaseDetail::Word(Result.Status)));
			break;
		}
	});
}

void UScfgShowcaseGameInstance::Tip()
{
	BeginSheet();
	FScfgPayRequest Request;
	Request.Amount = TEXT("1.00");
	Request.Token = TEXT("USDC");
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	Scfg()->PayAsync(EScfgPayKind::Tip, Request, [Weak](const FScfgPaymentResult& Result)
	{
		UScfgShowcaseGameInstance* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->EndSheet();
		Self->FocusHud();
		Self->SetStatus(Result.Status == EScfgPaymentStatus::Paid
				? FString::Printf(TEXT("Thank you for the %s %s tip!"), *Result.Amount, *Result.Token)
				: FString::Printf(TEXT("Tip: %s"), *ScfgShowcaseDetail::Word(Result.Status)));
	});
}

void UScfgShowcaseGameInstance::Restore()
{
	BeginSheet();
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	Scfg()->RestoreAsync([Weak](const FScfgRestoreResult& Result)
	{
		UScfgShowcaseGameInstance* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->EndSheet();
		Self->Refresh();
		Self->FocusHud();
		Self->SetStatus(FString::Printf(TEXT("Restore: %s"), *ScfgShowcaseDetail::Word(Result.Status)));
	});
}

void UScfgShowcaseGameInstance::HandlePaymentResolved(const FScfgPaymentResult& Result)
{
	// Every result without a Pay callback lands here (a payment the player kept playing through): idempotent.
	if (Result.Status == EScfgPaymentStatus::Paid && Hud.IsValid())
	{
		Refresh();
		SetStatus(TEXT("Payment confirmed. Thank you!"));
	}
}

void UScfgShowcaseGameInstance::HandleReceiptsChanged()
{
	Refresh();
}
