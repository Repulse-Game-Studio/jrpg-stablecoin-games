// Copyright 2026 Repulse
// The showcase: one screen that uses the Scfg plugin's whole public API, the Unreal twin of godot/showcase (main.gd) and
// of Unity's Showcase sample. Read ScfgShowcase.cpp top to bottom: 1. configure, 2. build the shop, 3. pay, restore,
// export, clear, 4. listen, 5. show the state. Testnet only: no real money moves.
//
// The left column is a small shop (three unlocks, one also at a pay-what-you-want amount), a tip jar and the player
// tools (Restore purchases, Export receipts, Clear local data, notifications on or off); the right column is what the
// plugin knows (where the configuration came from, Supported(), the owned items, TipTotal() and HasTipped(), Pending(),
// Receipts()), the last result and an event log of every result and event. A Slate HUD over the engine's Entry map,
// with the cursor shown; the game is paused while a sheet is open. Public API only (UScfgSubsystem, UScfgSettings).
//
// A PLUGIN OF ITS OWN (unreal/Plugins/ScfgShowcase, the release's scfg-unreal-showcase-<version>.zip): it goes next to
// the Scfg plugin in a project's Plugins folder (README.md). It has no level of its own: the project's Game Instance
// Class is UScfgShowcaseGameInstance and its Game Default Map the engine's empty /Engine/Maps/Entry (Project Settings >
// Maps & Modes); the game instance picks its own game mode (OverrideGameModeClass), so any map works. The repository's
// dev project (unreal/Project/ScfgDev) loads it from unreal/Plugins, with that wiring in its Config/DefaultEngine.ini.
//
// THE WALLET. The payments go to the developer's wallet, never to a placeholder: the -scfg-showcase-wallet=0x... (and
// -scfg-showcase-solana-wallet=...) command-line switches, else the project settings' Wallet and Solana Wallet
// (Project Settings > Plugins > SCFG). With neither, the showcase configures nothing, says so on its HUD and logs it
// ("LogScfgShowcase: Warning: showcase: No wallet is set, ..."), and the shop stays closed.
//
// CONFIGURATION. The project settings, when they have a wallet and items: the plugin applies them by itself when the
// game instance starts, and the showcase only reads them (the dev project's are in its Config/DefaultGame.ini). Settings
// with items but the wallet from the command line are applied with ConfigureWithSettings(Wallet, SolanaWallet). In
// code instead (ConfigureInCode: the showcase's three items, testnet, its accent and the logo in this plugin's
// Resources/, or the project's Content/Showcase/logo.png), with -scfg-showcase-code on the command line, or when the
// project's settings have no items (a fresh project with only its wallet set), which replaces the settings ("a code
// call after it wins", ScfgSettings.h). Settings with Testnet off are refused: the showcase runs on testnets only.
//
// The harness hook: a test harness may take over the setup (IScfgShowcaseHarness, SetHarnessFactory). The repository's
// dev project sets one outside Shipping, its e2e driver (unreal/Project/ScfgDev/Source/ScfgDev/ScfgE2EDriver.h), made
// only when the command line has -scfg-e2e=<config.json>. A harness that provides the configuration (its Options() and
// Items()) points the game at a local test chain (tools/run_unreal_e2e.py's payments); one whose OwnConfig() is true
// leaves the configuration to the showcase (the tour of the chain-free buttons, tests/unreal_showcase). A harness that
// cannot run, or a configuration that Configure / AddItem refuses under one, makes the game quit with exit code 1 (a
// test run never falls back to another setup); the missing wallet does not, so a harness can check that refusal too.
// Without a harness a refused configuration is shown on the HUD instead, and the game keeps running.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"

#include "ScfgTypes.h"

#include "ScfgShowcase.generated.h"

class SButton;
class SHorizontalBox;
class STextBlock;
class SVerticalBox;
class UScfgSettings;
class UScfgSubsystem;
class UScfgShowcaseGameInstance;

// The showcase's HUD: a view the game instance fills (ScfgShowcase.cpp, "2. The shop"). Every button has a name for
// keyboard focus and the e2e driver: "Unlock.<item>", "Pay.<item>", "Tip.<amount>", "Restore", "Export", "Clear",
// "Notify". Only the buttons take clicks: a click anywhere else is the world's.
class SCFGSHOWCASE_API SScfgShowcaseHud final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScfgShowcaseHud) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	struct FButtonSpec
	{
		FString Name;
		FString Text;
		TFunction<void()> OnClick;
	};
	// One shop row: the item's title, whether it is owned, and the buttons that buy it.
	void AddItem(const FString& Id, const FString& Title, TArray<FButtonSpec> Buy);
	void AddTip(FButtonSpec Button);
	void AddTool(FButtonSpec Button);

	// Shows the item as owned (and hides its buttons) or not.
	void SetOwned(const FString& Id, bool bOwned);
	void SetButtonText(const FString& Name, const FString& Text);
	// The right column: one line each.
	void SetSummary(const TArray<FString>& Lines);
	void SetReceipts(const TArray<FString>& Lines);
	// The last result, in a player's words.
	void SetStatus(const FString& Text);
	// One event log line (the newest LOG_LINES are kept).
	void Log(const FString& Line);
	// Shown instead of the shop when the plugin refused the configuration.
	void SetRefused(const FString& Text);

	struct FButtonView
	{
		FString Name;
		FString Text;
		TSharedRef<SButton> Widget;
		TSharedPtr<STextBlock> Label;
	};
	// The buttons shown, in layout order.
	TArray<FButtonView> Buttons() const;
	// Every line of text shown (labels, readouts, the log, button texts), in layout order.
	TArray<FString> Texts() const;
	// The keyboard focus goes to the first button shown (the game's own focus, as Godot's _focus_first_button()).
	TSharedPtr<SWidget> FirstButton() const;

	static constexpr int32 LOG_LINES = 12;

private:
	TSharedRef<SButton> MakeButton(FButtonSpec Spec);
	TSharedRef<STextBlock> MakeText(const FString& Text, int32 Size = 16, bool bBold = false);

	struct FRow
	{
		FString Id;
		TSharedRef<STextBlock> State;
		TArray<FString> ButtonNames;
	};
	TArray<FRow> Rows;
	TArray<FButtonView> All;
	// Every text block, in layout order (the button labels are read from All).
	TArray<TSharedRef<STextBlock>> TextBlocks;
	TArray<FString> LogLines;
	TSharedPtr<SVerticalBox> ItemsBox;
	TSharedPtr<SHorizontalBox> TipsBox;
	TSharedPtr<SHorizontalBox> ToolsBox;
	TSharedPtr<STextBlock> Summary;
	TSharedPtr<STextBlock> ReceiptsText;
	TSharedPtr<STextBlock> Status;
	TSharedPtr<STextBlock> LogText;
	TSharedPtr<STextBlock> Refused;
};

// Counts the clicks the world got (a click on the HUD or on a sheet is Slate's and never reaches it).
UCLASS()
class SCFGSHOWCASE_API AScfgShowcasePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

	int32 WorldClicks = 0;
	// Keyboard keys the world got (pressed), for the e2e driver: the hidden-sheet step's key.
	int32 WorldKeys = 0;
};

UCLASS()
class SCFGSHOWCASE_API AScfgShowcaseGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AScfgShowcaseGameMode();
};

// A test harness that takes over the showcase's setup (see the file header): made by the factory SetHarnessFactory()
// sets, in the game instance's Init(), and released when it shuts down.
class IScfgShowcaseHarness
{
public:
	virtual ~IScfgShowcaseHarness() = default;
	// True: the showcase configures itself, as it does without a harness (Options() and Items() are not used).
	virtual bool OwnConfig() const = 0;
	// Configure()'s options and the items to AddItem(), when OwnConfig() is false.
	virtual const FScfgOptions& Options() const = 0;
	virtual const TMap<FString, FScfgItem>& Items() const = 0;
	// Called once the showcase is configured (or refused for want of a wallet): false (logged) quits with exit code 1.
	virtual bool Start() = 0;
};

// Where the payments go (see THE WALLET in the file header): the command line's wallets, else the project settings'.
struct FScfgShowcaseWallets
{
	FString Wallet;
	FString SolanaWallet;
	// The wallets came from -scfg-showcase-wallet= / -scfg-showcase-solana-wallet=.
	bool bFromCommandLine = false;

	bool IsEmpty() const { return Wallet.IsEmpty() && SolanaWallet.IsEmpty(); }
};

UCLASS()
class SCFGSHOWCASE_API UScfgShowcaseGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// The item the e2e unlocks, and the tip it pays (one of the tip jar's amounts).
	static constexpr const TCHAR* ITEM = TEXT("chapter2");
	static constexpr const TCHAR* TIP_AMOUNT = TEXT("1.00");

	virtual void Init() override;
	virtual void OnStart() override;
	virtual void Shutdown() override;
	virtual TSubclassOf<AGameModeBase> OverrideGameModeClass(TSubclassOf<AGameModeBase> GameModeClass,
		const FString& MapName, const FString& Options, const FString& Portal) const override;

	// The HUD's actions. Unlock, Tip and Restore show a sheet or the restore screen, the game paused meanwhile; Amount
	// "" is the item's price.
	void Unlock(const FString& Item, const FString& Amount = FString());
	void Tip(const FString& Amount);
	void Restore();
	void ExportReceipts();
	void ClearLocalData();
	void ToggleNotify();

	// The harness hook (see the file header): Factory is called once per game instance, in Init(), and returns the
	// harness, or null for none; it sets bOutFailed (having logged why) when a harness was asked for but cannot be made,
	// and the game then quits with exit code 1. An empty factory (the default) is no harness: a game never sets one.
	using FHarnessFactory =
		TFunction<TSharedPtr<IScfgShowcaseHarness>(UScfgShowcaseGameInstance& Game, bool& bOutFailed)>;
	static void SetHarnessFactory(FHarnessFactory Factory);

	// The wallets the showcase pays (THE WALLET in the file header): -scfg-showcase-wallet= and
	// -scfg-showcase-solana-wallet= in CommandLine (either one replaces both of the settings'), else the settings'
	// Options.Wallet and Options.SolanaWallet, trimmed. Empty: no wallet, nothing is configured.
	static FScfgShowcaseWallets FindWallets(const TCHAR* CommandLine, const UScfgSettings& Settings);
	// What the HUD and the log say when no wallet is set.
	static const TCHAR* const NO_WALLET;

	TSharedPtr<SScfgShowcaseHud> GetHud() const { return Hud; }
	UScfgSubsystem* Scfg() const;
	// The game is paused because a sheet is open.
	bool IsPausedForSheet() const { return Busy > 0; }
	AScfgShowcasePlayerController* PlayerController() const;

private:
	// One item of the shop, from wherever the configuration came.
	struct FShopItem
	{
		FString Id;
		FScfgItem Item;
	};

	// Configures the plugin (see CONFIGURATION in the header): false when the plugin refused it (it logged why).
	bool ConfigureScfg();
	bool ConfigureInCode(const FScfgShowcaseWallets& Wallets);
	// Refuses the configuration: Why goes to the HUD and the log (as a warning).
	bool Refuse(const FString& Why);
	bool AddItems();
	void BuildShop();
	void Refresh();
	void SetStatus(const FString& Text);
	void Log(const FString& Text);
	void LogPayment(const FString& Source, const FScfgPaymentResult& Result);
	FString TitleOf(const FString& Item) const;
	void FocusHud();
	void BeginSheet();
	void EndSheet();
	void AfterFlow();
	// The setup failed under a harness: quit with exit code 1.
	void Fail();

	UFUNCTION()
	void HandlePaymentResolved(const FScfgPaymentResult& Result);
	UFUNCTION()
	void HandleReceiptsChanged();

	TSharedPtr<SScfgShowcaseHud> Hud;
	TArray<FShopItem> Items;
	// Where the configuration came from, as the HUD says it.
	FString ConfiguredFrom;
	bool bConfigured = false;
	// No wallet is set (the refusal a harness may check: the game keeps running under it).
	bool bNoWallet = false;
	// What the HUD shows instead of the shop when the configuration was refused.
	FString RefusedText;
	int32 Busy = 0;
	bool bWasPaused = false;
	bool bFailed = false;
	// The harness, when the factory made one (the dev project's e2e driver, with -scfg-e2e=).
	TSharedPtr<IScfgShowcaseHarness> Harness;
};
