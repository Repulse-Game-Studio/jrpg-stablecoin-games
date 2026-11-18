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
// CONFIGURATION. By default the plugin configures itself from the project settings (Project Settings > Plugins > SCFG,
// committed in Config/DefaultGame.ini: the testnet example wallet and the three items), when the game instance starts;
// the showcase only reads them. With -scfg-showcase-code on the command line, or when the project's settings are
// empty, it configures in code instead (ConfigureInCode: the same values), which replaces the settings ("a code call
// after it wins", ScfgSettings.h).
//
// Wiring (Config/DefaultEngine.ini): GameInstanceClass is UScfgShowcaseGameInstance and GameDefaultMap /Engine/Maps/Entry;
// the game instance picks its own game mode (OverrideGameModeClass), so other game instances (the automation tests')
// are unaffected.
//
// The e2e hook (not in Shipping): with -scfg-e2e=<config.json> on the command line, FScfgE2EDriver (ScfgE2EDriver.h)
// reads the config. A config with "configure" and "items" points the built-in chain at the local test node, and the
// game is configured from it instead (tools/run_unreal_e2e.py's payments); one with "own_config": true leaves the
// configuration to the showcase (the tour of the chain-free buttons, tests/unreal_showcase). A config that cannot be
// read, or that Configure / AddItem refuses, makes the game quit with exit code 1 (a test run never falls back to
// another setup). Without the driver a refused configuration is shown on the HUD instead, and the game keeps running.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"

#include "ScfgTypes.h"

#include "ScfgShowcase.generated.h"

class FScfgE2EDriver;
class SButton;
class SHorizontalBox;
class STextBlock;
class SVerticalBox;
class UScfgSubsystem;

// The showcase's HUD: a view the game instance fills (ScfgShowcase.cpp, "2. The shop"). Every button has a name for
// keyboard focus and the e2e driver: "Unlock.<item>", "Pay.<item>", "Tip.<amount>", "Restore", "Export", "Clear",
// "Notify". Only the buttons take clicks: a click anywhere else is the world's.
class SScfgShowcaseHud final : public SCompoundWidget
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
class AScfgShowcasePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

	int32 WorldClicks = 0;
	// Keyboard keys the world got (pressed), for the e2e driver: the hidden-sheet step's key.
	int32 WorldKeys = 0;
};

UCLASS()
class AScfgShowcaseGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AScfgShowcaseGameMode();
};

UCLASS()
class UScfgShowcaseGameInstance : public UGameInstance
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
	bool ConfigureInCode();
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
	// The setup failed under the e2e driver: quit with exit code 1.
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
	int32 Busy = 0;
	bool bWasPaused = false;
	bool bFailed = false;
	// The e2e driver, outside Shipping when -scfg-e2e= is given (never made in Shipping, where the class is not built).
	TSharedPtr<FScfgE2EDriver> Driver;
};
