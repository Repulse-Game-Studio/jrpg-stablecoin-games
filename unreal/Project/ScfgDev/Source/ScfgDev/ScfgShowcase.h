// The showcase: a minimal game over the Scfg plugin, the Unreal twin of godot/demo (main.gd). A title, a locked Chapter
// 2, an Unlock button, a Tip button and a Restore button on a Slate HUD over the engine's Entry map, with the cursor
// shown. It uses only the plugin's public API (UScfgSubsystem: Configure, AddItem, PayAsync, RestoreAsync, Owns,
// OnPaymentResolved, OnReceiptsChanged) and pauses the game while a sheet is open. Testnet only: no real money moves.
//
// Wiring (Config/DefaultEngine.ini): GameInstanceClass is UScfgShowcaseGameInstance and GameDefaultMap /Engine/Maps/Entry;
// the game instance picks its own game mode (OverrideGameModeClass), so other game instances (the automation tests')
// are unaffected.
//
// The e2e hook (not in Shipping): with -scfg-e2e=<config.json> on the command line, FScfgE2EDriver (ScfgE2EDriver.h)
// reads the config, points the built-in chain at the local test node, and the game is configured from the config
// instead of the built-in testnet setup. A config that cannot be read, or that Configure / AddItem refuses, makes the
// game quit with exit code 1 (a test run never falls back to the built-in setup).
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SCompoundWidget.h"

#include "ScfgTypes.h"

#include "ScfgShowcase.generated.h"

class FScfgE2EDriver;
class SButton;
class STextBlock;
class UScfgSubsystem;

// The showcase's HUD: texts and buttons, each button keyed ("Unlock", "Tip", "Restore") for keyboard focus and the
// e2e driver.
class SScfgShowcaseHud final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScfgShowcaseHud) {}
		SLATE_EVENT(FSimpleDelegate, OnUnlock)
		SLATE_EVENT(FSimpleDelegate, OnTip)
		SLATE_EVENT(FSimpleDelegate, OnRestore)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetChapter(const FString& Text, bool bShowUnlock);
	void SetStatus(const FString& Text);

	struct FButtonView
	{
		FString Name;
		FString Text;
		TSharedRef<SButton> Widget;
	};
	// The buttons shown, in layout order.
	TArray<FButtonView> Buttons() const;
	// Every text shown (labels and button texts), in layout order.
	TArray<FString> Texts() const;
	// The keyboard focus goes to the first button shown (the game's own focus, as Godot's _restore_focus()).
	TSharedPtr<SWidget> FirstButton() const;

private:
	TSharedRef<SButton> MakeButton(const FString& Name, const FString& Text, FSimpleDelegate OnClick);

	TSharedPtr<STextBlock> Chapter;
	TSharedPtr<STextBlock> Status;
	TArray<FButtonView> All;
	TArray<FString> Labels;
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
	static constexpr const TCHAR* ITEM = TEXT("chapter2");
	static constexpr const TCHAR* UNLOCKED = TEXT("Chapter 2: unlocked. Thank you for supporting the game!");

	virtual void Init() override;
	virtual void OnStart() override;
	virtual void Shutdown() override;
	virtual TSubclassOf<AGameModeBase> OverrideGameModeClass(TSubclassOf<AGameModeBase> GameModeClass,
		const FString& MapName, const FString& Options, const FString& Portal) const override;

	// The HUD's actions (a sheet or the restore screen, the game paused meanwhile).
	void Unlock();
	void Tip();
	void Restore();

	TSharedPtr<SScfgShowcaseHud> GetHud() const { return Hud; }
	UScfgSubsystem* Scfg() const;
	// The game is paused because a sheet is open.
	bool IsPausedForSheet() const { return Busy > 0; }
	AScfgShowcasePlayerController* PlayerController() const;

private:
	// Configures the plugin: the built-in testnet setup, or the e2e config's. False when the plugin refused it (it
	// logged why).
	bool ConfigureScfg();
	void Refresh();
	void SetStatus(const FString& Text);
	void FocusHud();
	void BeginSheet();
	void EndSheet();
	// The setup failed: quit with exit code 1.
	void Fail();

	UFUNCTION()
	void HandlePaymentResolved(const FScfgPaymentResult& Result);
	UFUNCTION()
	void HandleReceiptsChanged();

	TSharedPtr<SScfgShowcaseHud> Hud;
	int32 Busy = 0;
	bool bWasPaused = false;
	bool bFailed = false;
	// The e2e driver, outside Shipping when -scfg-e2e= is given (never made in Shipping, where the class is not built).
	TSharedPtr<FScfgE2EDriver> Driver;
};
