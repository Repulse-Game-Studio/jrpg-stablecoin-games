// Copyright 2026 Repulse
// No Python twin: godot/showcase/main.gd's Unreal form (see ScfgShowcase.h). Read it top to bottom: 1. configure,
// 2. build the shop, 3. pay, restore, export, clear, 4. listen, 5. show the state.
#include "ScfgShowcase.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgDevLog.h"
#include "ScfgSettings.h"
#include "ScfgSubsystem.h"
#if !UE_BUILD_SHIPPING
#include "ScfgE2EDriver.h"
#endif

namespace ScfgShowcaseDetail
{
// --- 1. Configuration ------------------------------------------------------------------------------------------------
// The project settings (Config/DefaultGame.ini, [/Script/Scfg.ScfgSettings]) hold these same values; ConfigureInCode()
// uses them with -scfg-showcase-code, or when the project has no settings.

// The wallet that receives the payments. This placeholder is the public example address from the EIP-55 spec: it is
// not yours, so a real payment to it would be lost. Replace it with your own (here and in the project settings).
const TCHAR* const WALLET = TEXT("0x52908400098527886E0F7030069857D2E4169EE7");
// The accent color of the payment sheet, as "#rrggbb".
const TCHAR* const ACCENT = TEXT("#7c5cff");
// The logo on the payment sheet and the checkout page, relative to the project folder. A loose file: its folder
// (Content/Showcase) is in Project Settings > Packaging > Additional Non-Asset Directories to Package, so a packaged
// game has it too (Config/DefaultGame.ini).
const TCHAR* const LOGO = TEXT("Content/Showcase/logo.png");

// The paid items, passed to AddItem(). Code (1-99) identifies the item in the payment amount: never change it after
// release. Pay may charge another amount than the price: anything above it (pay what you want), or down to an optional
// MinPrice (a sale).
struct FCodeItem
{
	const TCHAR* Id;
	int32 Code;
	const TCHAR* Price;
	const TCHAR* Title;
};
const FCodeItem ITEMS[] = {
	{TEXT("chapter2"), 1, TEXT("1.00"), TEXT("Chapter 2: The Fall")},
	{TEXT("soundtrack"), 2, TEXT("0.50"), TEXT("Original soundtrack")},
	{TEXT("supporter"), 3, TEXT("2.00"), TEXT("Supporter pack")},
};
const TCHAR* const TOKEN = TEXT("USDC");

// The supporter pack also offers a bigger, pay-what-you-want amount.
const TCHAR* const GENEROUS_ITEM = TEXT("supporter");
const TCHAR* const GENEROUS_AMOUNT = TEXT("5.00");

// The tip jar's suggested amounts (the player can change the amount in the sheet).
const TCHAR* const TIP_AMOUNTS[] = {TEXT("0.50"), TEXT("1.00"), TEXT("3.00")};

// The command-line switch that configures in code even when the project has settings.
const TCHAR* const CODE_SWITCH = TEXT("scfg-showcase-code");

const TCHAR* const FROM_SETTINGS = TEXT("the project settings (Config/DefaultGame.ini)");
const TCHAR* const FROM_CODE = TEXT("code (ConfigureInCode)");
const TCHAR* const FROM_E2E = TEXT("the e2e config");

// The status words of godot/showcase (the core's status strings).
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

FString ErrorSuffix(const FString& Error)
{
	return Error.IsEmpty() ? FString() : FString::Printf(TEXT(" (error: %s)"), *Error);
}

FString Short(const FString& Hash)
{
	return Hash.Len() <= 14 ? Hash : Hash.Left(8) + TEXT("...") + Hash.Right(6);
}

FString BuildKind()
{
#if PLATFORM_ANDROID || PLATFORM_IOS
	return TEXT("mobile (best effort, unverified)");
#else
	return TEXT("desktop (the checkout opens in your browser)");
#endif
}
}

// --- the HUD ----------------------------------------------------------------------------------------------------------

TSharedRef<STextBlock> SScfgShowcaseHud::MakeText(const FString& Text, int32 Size, bool bBold)
{
	const TSharedRef<STextBlock> Block = SNew(STextBlock)
		.Text(FText::FromString(Text))
		.Font(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size))
		.ColorAndOpacity(bBold ? FLinearColor::White : FLinearColor(0.85f, 0.85f, 0.88f))
		.AutoWrapText(true)
		.Visibility(EVisibility::HitTestInvisible);
	TextBlocks.Add(Block);
	return Block;
}

void SScfgShowcaseHud::Construct(const FArguments& InArgs)
{
	// Only the buttons take clicks: a click anywhere else is the world's (the player controller counts them).
	SetVisibility(EVisibility::SelfHitTestInvisible);
	const EVisibility Through = EVisibility::SelfHitTestInvisible;
	const TSharedRef<SVerticalBox> Left = SNew(SVerticalBox).Visibility(Through);
	const TSharedRef<SVerticalBox> Right = SNew(SVerticalBox).Visibility(Through);
	auto Section = [this](const TSharedRef<SVerticalBox>& Column, const FString& Title)
	{
		Column->AddSlot().AutoHeight().Padding(0, 20, 0, 6)[MakeText(Title, 18, true)];
	};

	Left->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[MakeText(TEXT("Stablecoin for Games Showcase"), 28, true)];
	Left->AddSlot().AutoHeight()[MakeText(TEXT("This build runs on testnets, so no real money is involved."))];
	Left->AddSlot().AutoHeight().Padding(0, 12, 0, 0)[SAssignNew(Refused, STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 16)).ColorAndOpacity(FLinearColor(1.f, 0.42f, 0.42f))
		.AutoWrapText(true).Visibility(EVisibility::Collapsed)];
	TextBlocks.Add(Refused.ToSharedRef());
	Section(Left, TEXT("Unlocks"));
	Left->AddSlot().AutoHeight()[SAssignNew(ItemsBox, SVerticalBox).Visibility(Through)];
	Section(Left, TEXT("Tip jar"));
	Left->AddSlot().AutoHeight()[SAssignNew(TipsBox, SHorizontalBox).Visibility(Through)];
	Section(Left, TEXT("Player tools"));
	Left->AddSlot().AutoHeight()[SAssignNew(ToolsBox, SHorizontalBox).Visibility(Through)];
	Left->AddSlot().AutoHeight().Padding(0, 20, 0, 0)[SAssignNew(Status, STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 16)).ColorAndOpacity(FLinearColor::White).AutoWrapText(true)
		.Visibility(EVisibility::HitTestInvisible)];
	TextBlocks.Add(Status.ToSharedRef());

	Section(Right, TEXT("What SCFG knows"));
	Right->AddSlot().AutoHeight()[SAssignNew(Summary, STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16))
		.ColorAndOpacity(FLinearColor::White).AutoWrapText(true).Visibility(EVisibility::HitTestInvisible)];
	TextBlocks.Add(Summary.ToSharedRef());
	Section(Right, TEXT("Receipts"));
	Right->AddSlot().AutoHeight()[SAssignNew(ReceiptsText, STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
		.ColorAndOpacity(FLinearColor::White).AutoWrapText(true).Visibility(EVisibility::HitTestInvisible)];
	TextBlocks.Add(ReceiptsText.ToSharedRef());
	Section(Right, TEXT("Event log"));
	Right->AddSlot().AutoHeight()[SAssignNew(LogText, STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 13))
		.ColorAndOpacity(FLinearColor(0.7f, 0.72f, 0.78f)).AutoWrapText(true).Visibility(EVisibility::HitTestInvisible)];
	TextBlocks.Add(LogText.ToSharedRef());

	ChildSlot
	.HAlign(HAlign_Fill)
	.VAlign(VAlign_Top)
	.Padding(80, 50, 80, 0)
	[
		SNew(SHorizontalBox).Visibility(Through)
		+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(820).Visibility(Through)[Left]]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(60, 0, 0, 0)[Right]
	];
}

TSharedRef<SButton> SScfgShowcaseHud::MakeButton(FButtonSpec Spec)
{
	TSharedPtr<STextBlock> Label;
	const TSharedRef<SButton> Button = SNew(SButton)
		.OnClicked_Lambda([OnClick = MoveTemp(Spec.OnClick)]()
		{
			if (OnClick)
			{
				OnClick();
			}
			return FReply::Handled();
		})
		.ContentPadding(FMargin(14, 6))
		[
			SAssignNew(Label, STextBlock).Text(FText::FromString(Spec.Text)).Font(FCoreStyle::GetDefaultFontStyle("Regular", 16))
		];
	All.Add({Spec.Name, Spec.Text, Button, Label});
	return Button;
}

void SScfgShowcaseHud::AddItem(const FString& Id, const FString& Title, TArray<FButtonSpec> Buy)
{
	const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox).Visibility(EVisibility::SelfHitTestInvisible);
	Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)[MakeText(Title)];
	const TSharedRef<STextBlock> State = MakeText(FString());
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0)[State];
	FRow Entry{Id, State, {}};
	for (FButtonSpec& Spec : Buy)
	{
		Entry.ButtonNames.Add(Spec.Name);
		Row->AddSlot().AutoWidth().Padding(4, 0, 0, 0)[MakeButton(MoveTemp(Spec))];
	}
	Rows.Add(MoveTemp(Entry));
	ItemsBox->AddSlot().AutoHeight().Padding(0, 3)[Row];
}

void SScfgShowcaseHud::AddTip(FButtonSpec Button)
{
	TipsBox->AddSlot().AutoWidth().Padding(0, 0, 8, 0)[MakeButton(MoveTemp(Button))];
}

void SScfgShowcaseHud::AddTool(FButtonSpec Button)
{
	ToolsBox->AddSlot().AutoWidth().Padding(0, 0, 8, 0)[MakeButton(MoveTemp(Button))];
}

void SScfgShowcaseHud::SetOwned(const FString& Id, bool bOwned)
{
	for (const FRow& Row : Rows)
	{
		if (Row.Id != Id)
		{
			continue;
		}
		Row.State->SetText(FText::FromString(bOwned ? TEXT("owned") : TEXT("")));
		for (const FButtonView& B : All)
		{
			if (Row.ButtonNames.Contains(B.Name))
			{
				B.Widget->SetVisibility(bOwned ? EVisibility::Collapsed : EVisibility::Visible);
			}
		}
	}
}

void SScfgShowcaseHud::SetButtonText(const FString& Name, const FString& Text)
{
	for (FButtonView& B : All)
	{
		if (B.Name == Name)
		{
			B.Text = Text;
			B.Label->SetText(FText::FromString(Text));
		}
	}
}

void SScfgShowcaseHud::SetSummary(const TArray<FString>& Lines)
{
	Summary->SetText(FText::FromString(FString::Join(Lines, TEXT("\n"))));
}

void SScfgShowcaseHud::SetReceipts(const TArray<FString>& Lines)
{
	ReceiptsText->SetText(FText::FromString(FString::Join(Lines, TEXT("\n"))));
}

void SScfgShowcaseHud::SetStatus(const FString& Text)
{
	Status->SetText(FText::FromString(Text));
}

void SScfgShowcaseHud::Log(const FString& Line)
{
	LogLines.Add(Line);
	if (LogLines.Num() > LOG_LINES)
	{
		LogLines.RemoveAt(0, LogLines.Num() - LOG_LINES);
	}
	LogText->SetText(FText::FromString(FString::Join(LogLines, TEXT("\n"))));
}

void SScfgShowcaseHud::SetRefused(const FString& Text)
{
	Refused->SetText(FText::FromString(Text));
	Refused->SetVisibility(Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible);
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
	TArray<FString> Out;
	for (const TSharedRef<STextBlock>& Block : TextBlocks)
	{
		if (!Block->GetVisibility().IsVisible())
		{
			continue;
		}
		TArray<FString> Lines;
		Block->GetText().ToString().ParseIntoArrayLines(Lines);
		Out.Append(Lines);
	}
	for (const FButtonView& B : Buttons())
	{
		Out.Add(B.Text);
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

// --- the game instance: 1. configure -----------------------------------------------------------------------------------

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
	Super::Init();  // the subsystem's Initialize: the project settings are applied here (when they have a wallet)
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
	// Before the first frame: payments start on the first frame after a configuration, and a Configure() after that is
	// configure.after_start.
	bConfigured = ConfigureScfg();
	if (!bConfigured && Driver.IsValid())
	{
		Fail();
		return;
	}
	if (UScfgSubsystem* S = Scfg())
	{
		S->OnPaymentResolved.AddDynamic(this, &UScfgShowcaseGameInstance::HandlePaymentResolved);
		S->OnReceiptsChanged.AddDynamic(this, &UScfgShowcaseGameInstance::HandleReceiptsChanged);
	}
#if !UE_BUILD_SHIPPING
	if (Driver.IsValid() && !Driver->Start())
	{
		Fail();
	}
#endif
}

bool UScfgShowcaseGameInstance::ConfigureScfg()
{
	using namespace ScfgShowcaseDetail;
	UScfgSubsystem* S = Scfg();
	if (!S)
	{
		UE_LOG(LogScfgDev, Error, TEXT("showcase: the Scfg subsystem is missing"));
		return false;
	}
	S->SetNotify(true);  // show a "Payment confirmed" toast for payments that confirm in the background
#if !UE_BUILD_SHIPPING
	if (Driver.IsValid() && !Driver->OwnConfig())
	{
		ConfiguredFrom = FROM_E2E;
		// Configure and AddItem return "" or the error they logged ("LogScfg: Error: scfg: ...").
		if (!S->Configure(Driver->Options()).IsEmpty())
		{
			return false;
		}
		for (const TPair<FString, FScfgItem>& Item : Driver->Items())
		{
			Items.Add({Item.Key, Item.Value});
		}
		return AddItems();
	}
#endif
	const UScfgSettings* Settings = GetDefault<UScfgSettings>();
	if (FParse::Param(FCommandLine::Get(), CODE_SWITCH) || Settings->IsEmpty())
	{
		ConfiguredFrom = FROM_CODE;
		return ConfigureInCode();
	}
	// The subsystem applied the project settings when the game instance started (Super::Init()); a setting it refused
	// was logged then. Validate() runs the same checks, without touching the subsystem.
	ConfiguredFrom = FROM_SETTINGS;
	if (const TArray<FScfgSettingsProblem> Problems = Settings->Validate(); !Problems.IsEmpty())
	{
		UE_LOG(LogScfgDev, Error, TEXT("showcase: the SCFG project settings were not applied (%d problem(s), the first "
			"%s: %s): press Validate in Project Settings > Plugins > SCFG"), Problems.Num(), *Problems[0].Code,
			*Problems[0].Message);
		return false;
	}
	for (const FScfgSettingsItem& Row : Settings->Items)
	{
		Items.Add({Row.Id, Row.ToItem()});
	}
	return true;
}

// Configures the plugin and declares the items, as a game does in its game instance's Init(). After the project
// settings were applied, this Configure() replaces them (options, chains and items).
bool UScfgShowcaseGameInstance::ConfigureInCode()
{
	using namespace ScfgShowcaseDetail;
	FScfgOptions Options;
	Options.Wallet = WALLET;
	Options.bTestnet = true;  // Sepolia, Arbitrum Sepolia, Amoy, Base Sepolia, Arc Testnet
	Options.Accent = ACCENT;
	Options.Logo = LOGO;
	// Optional, among others: SolanaWallet, Chains, Tokens, Methods, RpcUrls, Confirmations.
	if (!Scfg()->Configure(Options).IsEmpty())
	{
		return false;
	}
	for (const FCodeItem& Code : ITEMS)
	{
		FScfgItem Item;
		Item.Code = Code.Code;
		Item.Price = Code.Price;
		Item.Token = TOKEN;
		Item.Title = Code.Title;
		Items.Add({Code.Id, Item});
	}
	return AddItems();
}

bool UScfgShowcaseGameInstance::AddItems()
{
	for (const FShopItem& Shop : Items)
	{
		if (!Scfg()->AddItem(Shop.Id, Shop.Item).IsEmpty())
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

// --- 2. The shop -----------------------------------------------------------------------------------------------------

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
	SAssignNew(Hud, SScfgShowcaseHud);
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
	if (!bConfigured)
	{
		Hud->SetRefused(TEXT("The plugin refused the configuration; see the Output Log (LogScfg)."));
		Log(TEXT("Not configured: the shop is closed."));
		return;
	}
	BuildShop();
	Refresh();
	Log(FString::Printf(TEXT("Ready. Configured from %s. Build: %s. Pick something on the left."), *ConfiguredFrom,
		*ScfgShowcaseDetail::BuildKind()));
	FocusHud();
	UE_LOG(LogScfgDev, Log, TEXT("showcase: ready"));
}

void UScfgShowcaseGameInstance::BuildShop()
{
	using namespace ScfgShowcaseDetail;
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	// Every button calls one member; the weak pointer keeps a click after Shutdown harmless.
	auto Call = [Weak](TFunction<void(UScfgShowcaseGameInstance&)> Action)
	{
		return [Weak, Action = MoveTemp(Action)]()
		{
			if (UScfgShowcaseGameInstance* Self = Weak.Get())
			{
				Action(*Self);
			}
		};
	};
	// One row per item: its title, whether it is owned, and the button(s) that buy it.
	for (const FShopItem& Shop : Items)
	{
		const FString Id = Shop.Id;
		TArray<SScfgShowcaseHud::FButtonSpec> Buy;
		Buy.Add({TEXT("Unlock.") + Id, FString::Printf(TEXT("Unlock: %s %s"), *Shop.Item.Price, *Shop.Item.Token),
			Call([Id](UScfgShowcaseGameInstance& G) { G.Unlock(Id); })});
		if (Id == GENEROUS_ITEM)
		{
			Buy.Add({TEXT("Pay.") + Id, FString::Printf(TEXT("Pay %s %s"), GENEROUS_AMOUNT, *Shop.Item.Token),
				Call([Id](UScfgShowcaseGameInstance& G) { G.Unlock(Id, GENEROUS_AMOUNT); })});
		}
		Hud->AddItem(Id, Shop.Item.Title.IsEmpty() ? Id : Shop.Item.Title, MoveTemp(Buy));
	}
	for (const TCHAR* Amount : TIP_AMOUNTS)
	{
		const FString A = Amount;
		Hud->AddTip({TEXT("Tip.") + A, FString::Printf(TEXT("%s %s"), *A, TOKEN),
			Call([A](UScfgShowcaseGameInstance& G) { G.Tip(A); })});
	}
	Hud->AddTool({TEXT("Restore"), TEXT("Restore purchases"), Call([](UScfgShowcaseGameInstance& G) { G.Restore(); })});
	Hud->AddTool({TEXT("Export"), TEXT("Export receipts"), Call([](UScfgShowcaseGameInstance& G) { G.ExportReceipts(); })});
	Hud->AddTool({TEXT("Clear"), TEXT("Clear local data"), Call([](UScfgShowcaseGameInstance& G) { G.ClearLocalData(); })});
	Hud->AddTool({TEXT("Notify"), TEXT("Notify: on"), Call([](UScfgShowcaseGameInstance& G) { G.ToggleNotify(); })});
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

// --- 3. Paying, restoring, exporting ---------------------------------------------------------------------------------

// Opens the payment sheet for an item. Amount overrides the price ("" keeps it).
void UScfgShowcaseGameInstance::Unlock(const FString& Item, const FString& Amount)
{
	FScfgPayRequest Request;
	Request.Item = Item;
	Request.Amount = Amount;
	Log(Amount.IsEmpty() ? FString::Printf(TEXT("PayAsync(Unlock, Item %s)"), *Item)
						 : FString::Printf(TEXT("PayAsync(Unlock, Item %s, Amount %s)"), *Item, *Amount));
	BeginSheet();
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	// The callback runs once, when the player closes the sheet; a game instance shut down meanwhile drops it.
	Scfg()->PayAsync(EScfgPayKind::Unlock, Request, [Weak, Item](const FScfgPaymentResult& Result)
	{
		UScfgShowcaseGameInstance* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->EndSheet();
		Self->LogPayment(TEXT("unlock result"), Result);
		switch (Result.Status)
		{
		case EScfgPaymentStatus::Paid:
		case EScfgPaymentStatus::AlreadyOwned:
			Self->SetStatus(TEXT("Payment confirmed. Thank you!"));
			break;
		case EScfgPaymentStatus::Pending:
			Self->SetStatus(FString::Printf(TEXT("No problem, keep playing. %s unlocks by itself when the payment arrives."),
				*Self->TitleOf(Item)));
			break;
		default:
			Self->SetStatus(FString::Printf(TEXT("Maybe later. (Status: %s)"), *ScfgShowcaseDetail::Word(Result.Status)));
			break;
		}
		Self->AfterFlow();
	});
}

void UScfgShowcaseGameInstance::Tip(const FString& Amount)
{
	FScfgPayRequest Request;
	Request.Amount = Amount;
	Request.Token = ScfgShowcaseDetail::TOKEN;
	Log(FString::Printf(TEXT("PayAsync(Tip, Amount %s, Token %s)"), *Amount, *Request.Token));
	BeginSheet();
	TWeakObjectPtr<UScfgShowcaseGameInstance> Weak(this);
	Scfg()->PayAsync(EScfgPayKind::Tip, Request, [Weak](const FScfgPaymentResult& Result)
	{
		UScfgShowcaseGameInstance* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		Self->EndSheet();
		Self->LogPayment(TEXT("tip result"), Result);
		Self->SetStatus(Result.Status == EScfgPaymentStatus::Paid
				? FString::Printf(TEXT("Thank you for the %s %s tip!"), *Result.Amount, *Result.Token)
				: FString::Printf(TEXT("Tip: %s"), *ScfgShowcaseDetail::Word(Result.Status)));
		Self->AfterFlow();
	});
}

// Shows the restore screen, where the player pastes the hash of a payment made elsewhere.
void UScfgShowcaseGameInstance::Restore()
{
	Log(TEXT("RestoreAsync()"));
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
		const FString Word = ScfgShowcaseDetail::Word(Result.Status);
		const FString Added = Result.Status == EScfgRestoreStatus::Restored
			? FString::Printf(TEXT(", %d receipt(s) added"), Result.Restored.Num()) : FString();
		Self->Log(FString::Printf(TEXT("restore result: %s%s%s"), *Word, *Added,
			*ScfgShowcaseDetail::ErrorSuffix(Result.Error)));
		Self->SetStatus(FString::Printf(TEXT("Restore: %s"), *Word));
		Self->AfterFlow();
	});
}

void UScfgShowcaseGameInstance::ExportReceipts()
{
	// "" or the error it logged; the default path is Saved/scfg-receipts.json.
	const FString Path = Scfg()->ExportReceipts(FString());
	Log(Path.IsEmpty() ? FString(TEXT("ExportReceipts() failed: see the Output Log"))
					   : FString::Printf(TEXT("ExportReceipts() wrote %s"), *Path));
}

void UScfgShowcaseGameInstance::ClearLocalData()
{
	Scfg()->ClearLocalData();
	Log(TEXT("ClearLocalData(): receipts forgotten on this device. Restore brings them back."));
	Refresh();  // OnReceiptsChanged only comes when there were receipts; the pending payments go too
}

void UScfgShowcaseGameInstance::ToggleNotify()
{
	const bool bNotify = !Scfg()->GetNotify();
	Scfg()->SetNotify(bNotify);
	Log(FString::Printf(TEXT("SetNotify(%s)"), bNotify ? TEXT("true") : TEXT("false")));
	Refresh();
}

// The game is paused while a sheet is open: the sheet takes the focus, but game code that reads input keeps running,
// and the plugin keeps working while the game is paused.
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

void UScfgShowcaseGameInstance::AfterFlow()
{
	Refresh();
	FocusHud();
}

// --- 4. Listening ----------------------------------------------------------------------------------------------------

// A final result: a payment that confirms in the background (the player kept playing) or after a restart, and the
// results of a PayAsync above too (a cancel included). The same payment can come twice, so it logs and refreshes from
// Owns(), and never counts.
void UScfgShowcaseGameInstance::HandlePaymentResolved(const FScfgPaymentResult& Result)
{
	LogPayment(TEXT("OnPaymentResolved"), Result);
	if (Result.Status == EScfgPaymentStatus::Paid)
	{
		SetStatus(TEXT("Payment confirmed. Thank you!"));
	}
	Refresh();
}

// The stored receipts changed: a payment confirmed, a restore, ClearLocalData().
void UScfgShowcaseGameInstance::HandleReceiptsChanged()
{
	Log(TEXT("OnReceiptsChanged"));
	Refresh();
}

// --- 5. Showing the state --------------------------------------------------------------------------------------------

void UScfgShowcaseGameInstance::Refresh()
{
	UScfgSubsystem* S = Scfg();
	if (!Hud.IsValid() || !S || !bConfigured)
	{
		return;
	}
	TArray<FString> Owned;
	for (const FShopItem& Shop : Items)
	{
		const bool bOwned = S->Owns(Shop.Id);
		Hud->SetOwned(Shop.Id, bOwned);
		if (bOwned)
		{
			Owned.Add(TitleOf(Shop.Id));
		}
	}
	const FString Tips = FString::Printf(TEXT("Tips: %s %s%s"), *S->TipTotal(), ScfgShowcaseDetail::TOKEN,
		S->HasTipped() ? TEXT(" (thank you!)") : TEXT(""));
	Hud->SetSummary({
		FString::Printf(TEXT("Configured from: %s"), *ConfiguredFrom),
		FString::Printf(TEXT("Supported: %s"), S->Supported() ? TEXT("yes") : TEXT("no")),
		FString::Printf(TEXT("Owned: %s"), Owned.IsEmpty() ? TEXT("nothing yet") : *FString::Join(Owned, TEXT(", "))),
		Tips,
		FString::Printf(TEXT("Payments still being watched: %d"), S->Pending().Num()),
	});
	TArray<FString> Lines;
	for (const TPair<FString, FScfgReceipt>& Pair : S->Receipts())
	{
		const FScfgReceipt& R = Pair.Value;
		const FString What = R.Kind == TEXT("unlock") ? TitleOf(R.Item) : FString(TEXT("Tip"));
		Lines.Add(FString::Printf(TEXT("%s: %s %s on %s, tx %s"), *What, *R.Amount, *R.Token, *R.Chain,
			*ScfgShowcaseDetail::Short(R.TxHash)));
	}
	if (Lines.IsEmpty())
	{
		Lines.Add(TEXT("None yet. Pay for something, or restore a payment."));
	}
	Hud->SetReceipts(Lines);
	Hud->SetButtonText(TEXT("Notify"), S->GetNotify() ? TEXT("Notify: on") : TEXT("Notify: off"));
}

FString UScfgShowcaseGameInstance::TitleOf(const FString& Item) const
{
	for (const FShopItem& Shop : Items)
	{
		if (Shop.Id == Item && !Shop.Item.Title.IsEmpty())
		{
			return Shop.Item.Title;
		}
	}
	return Item;
}

void UScfgShowcaseGameInstance::SetStatus(const FString& Text)
{
	if (Hud.IsValid())
	{
		Hud->SetStatus(Text);
	}
}

void UScfgShowcaseGameInstance::Log(const FString& Text)
{
	UE_LOG(LogScfgDev, Log, TEXT("showcase: %s"), *Text);
	if (Hud.IsValid())
	{
		Hud->Log(FDateTime::Now().ToString(TEXT("%H:%M:%S  ")) + Text);
	}
}

void UScfgShowcaseGameInstance::LogPayment(const FString& Source, const FScfgPaymentResult& Result)
{
	const FString Paid = Result.Amount.IsEmpty()
		? FString()
		: FString::Printf(TEXT(" %s %s on %s"), *Result.Amount, *Result.Token, *Result.Chain);
	Log(FString::Printf(TEXT("%s: %s%s%s"), *Source, *ScfgShowcaseDetail::Word(Result.Status), *Paid,
		*ScfgShowcaseDetail::ErrorSuffix(Result.Error)));
}

// Keyboard and gamepad players keep a focused button after every flow (only while the cursor is shown, the showcase's
// menu: a game-only input mode keeps the focus where the sheet gave it back, on the game viewport).
void UScfgShowcaseGameInstance::FocusHud()
{
	const APlayerController* PC = GetFirstLocalPlayerController();
	if (Hud.IsValid() && FSlateApplication::IsInitialized() && (!PC || PC->ShouldShowMouseCursor()))
	{
		if (const TSharedPtr<SWidget> First = Hud->FirstButton())
		{
			FSlateApplication::Get().SetUserFocus(0, First, EFocusCause::SetDirectly);
		}
	}
}
