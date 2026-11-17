// No Python twin: the rig of the Slate screen tests (Scfg.Glue.Sheet.*), the twin of godot/tests/ui_env.gd. The
// editor runs them under -nullrhi: FSlateApplication is up with the null renderer (real fonts, no GPU), so widgets are
// built, laid out, painted (which ticks them) and receive input through FSlateApplication::ProcessKeyDownEvent /
// ProcessKeyCharEvent / ProcessAnalogInputEvent, exactly the routing a game gets (engine-facts.md, "Slate under
// -nullrhi").
//
// FSheetRig: an SVirtualWindow (registered with FSlateApplication, never a native window) holding the overlay a game
// viewport draws its widgets in; a UGameViewportClient of the tests whose overlay is that one, set as the game
// viewport of a real game instance (InitializeStandalone); the plugin's FScfgDefaultSheetHost on that instance, so the
// screens go through UGameViewportClient::AddViewportWidgetContent as in a game; the facade of FWorld (fake nodes,
// fake platform) with that host as its default host and no game host; and a focusable widget of "the game" behind the
// sheet that records every key it gets and stands for the game viewport widget (the host's mouse wish names it).
#pragma once

#include "CoreMinimal.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

#include "ScfgDefaultSheetHost.h"
#include "ScfgFacadeTestKit.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/SScfgRestoreScreen.h"
#include "UI/SScfgScreen.h"

#if WITH_DEV_AUTOMATION_TESTS

class SOverlay;
class SVirtualWindow;
class UGameInstance;
class UGameViewportClient;
class UWorld;

namespace ScfgSheetTests
{

// "The game": a focusable widget that records the keys, characters and stick moves it gets.
class SGameSink final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGameSink) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	TArray<FString> Got;
};

struct FSheetRig
{
	ScfgFacadeTests::FWorld W;
	TSharedPtr<SVirtualWindow> Window;
	TSharedPtr<SOverlay> Layer;
	TSharedPtr<SGameSink> Game;
	TStrongObjectPtr<UGameInstance> Instance;
	TStrongObjectPtr<UGameViewportClient> Viewport;
	TSharedPtr<FScfgDefaultSheetHost> Host;
	TSharedPtr<SWidget> FocusBefore;
	TWeakObjectPtr<UWorld> GameWorld;
	double Now = 0;
	bool bReady = false;
	// The test shut the instance down itself (the destructor then only destroys its world).
	bool bInstanceShutDown = false;

	// Size: the virtual window's (the game's resolution).
	explicit FSheetRig(FAutomationTestBase& Test, FVector2D Size = FVector2D(1280, 720));
	~FSheetRig();
	FSheetRig(const FSheetRig&) = delete;
	FSheetRig& operator=(const FSheetRig&) = delete;

	// FWorld::Started with this rig's host as the facade's default host (no game host).
	FScfgFacade& Started(FAutomationTestBase& Test);
	// Frames: the facade's pump, then the window painted (which ticks the screens), then GFrameCounter advanced, as
	// the engine loop does after Slate's draw.
	void Frame(int32 N = 1, float Dt = 1.f / 60.f);
	// A worker pass (FWorld::Pass), then two frames.
	void Pass();
	// A key pressed and released through FSlateApplication; true when something handled the press.
	bool Key(const FKey& Key, bool bShift = false, bool bRepeat = false);
	// Characters typed (FSlateApplication::ProcessKeyCharEvent each).
	void Type(const FString& Text);
	// The left stick's axis at Value (then back to 0).
	void Stick(const FKey& Axis, float Value);
	// A left click at the middle of Widget, routed as FSlateApplication routes it: to the mouse captor through
	// ProcessMouseButtonDown/UpEvent while a widget has the capture (the engine's own path), else to the widgets the
	// virtual window's hittest grid finds there (RoutePointerDown/UpEvent: a virtual window is not located under the
	// cursor by ProcessMouseButtonDownEvent).
	void Click(const TSharedRef<SWidget>& Widget);
	// The game's widget takes the mouse capture, as a game viewport that hides the cursor does.
	void GameCapturesTheMouse();
	TSharedPtr<SWidget> MouseCaptor() const;
	TSharedPtr<SWidget> Focused() const;
	// The newest screen the host shows, as a payment sheet / restore screen, or null.
	TSharedPtr<SScfgPaymentSheet> Sheet() const;
	TSharedPtr<SScfgRestoreScreen> RestoreScreen() const;
	// Slate's focus (user 0) is on Screen's control Key.
	bool HasFocus(const SScfgScreen& Screen, const FString& Key) const;
	// Slate's focus is somewhere inside Screen.
	bool FocusInside(const SScfgScreen& Screen) const;
	// Presses Direction until Screen's control Key has the focus (at most Max presses).
	bool KeyTo(const SScfgScreen& Screen, const FString& Key, const FKey& Direction, int32 Max = 16);
	// The widgets in the viewport's overlay, besides the game's.
	int32 NumLayered() const;
};

// The text of messages.json (conformance/vectors), found as the core tests find the vectors: $SCFG_VECTORS, else
// walking up from the plugin's folder; empty when not found.
FString MessagesJson();

// Text matches a ScfgTexts template ("%s" is any run of characters).
bool MatchesTemplate(const FString& Text, const FString& Template);

}  // namespace ScfgSheetTests

#endif  // WITH_DEV_AUTOMATION_TESTS
