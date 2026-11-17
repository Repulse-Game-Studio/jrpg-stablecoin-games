// Twin of godot/addons/scfg/ui/screen_base.gd: what the payment sheet (SScfgPaymentSheet) and the restore screen
// (SScfgRestoreScreen) share. A full-screen widget that dims the game and takes its input, with the light sheet docked
// on the right (FScfgStyle), keyboard and gamepad focus, and small builders for labels, buttons, options and fields.
//
// A screen rebuilds its content when the subclass's Signature() changes, from Tick (every painted frame: Slate's
// real-time tick, so a paused game or a time dilation of 0 changes nothing) or Refresh(). Every text comes from
// ScfgTexts by id; amounts, addresses and names are data.
//
// FOCUS. Each build lists its focusable controls in visual order. Up and down (the arrow keys, the d-pad, the left
// stick), Tab and Shift+Tab move along that list (the ends stay put for up and down, Tab wraps), left and right move
// within a row of buttons (not in a text field, where they move the caret), so the focus never leaves the screen for
// the game behind it; Slate's own navigation (an input preprocessor's, the stick where the engine turns it into
// navigation) asks OnNavigation and follows the same order. Enter, Space and the gamepad's confirm (face button bottom) press the focused button or submit
// the focused field; Escape and the gamepad's back (face button right) call OnBack(). Letters always go to a focused
// field, even when the game binds them to navigation. The screen draws its own focus ring (the accent). Slate's user
// focus is the source of truth where the screen is in a window (a click moves it too); a screen outside any window
// (a test) keeps its own.
//
// INPUT. While the screen is open the game gets no keys, characters, gamepad buttons or sticks, mouse buttons, wheel
// or moves that reach it: they are consumed here (the confirm and back keys first of all), the engine's own viewport
// keys too (the console key, Alt+Enter, the screenshot and debug keys do nothing while it is open). Keys are handled
// in the tunnelling phase (OnPreviewKeyDown), before the focused button sees them, so a press is never doubled. A
// held key's repeats move the focus but never press or go back (Godot's allow_echo false): holding Escape goes back
// one step, not out of the checkout.
//
// THE MOUSE. A game viewport that captured the mouse (a game that hides the cursor: the default capture mode, high
// precision movement, the cursor locked to the window) would keep every click and move, and hide the cursor over the
// sheet. OnShown therefore releases the mouse user's capture and lock (through FSlateApplication::ProcessReply, which
// also leaves high-precision mode), remembering who held the capture, whether in high precision, and whether the cursor
// was locked with it (SetMouseLockHint: the viewport client's LockDuringCapture; else assumed with high precision);
// OnCursorQuery shows the default cursor over the screen. OnHidden, after giving the keyboard focus back, gives the
// capture, high precision and lock back exactly so, by a reply on the captor's path: this does not depend on the focus
// cause, so a PIE viewport (which re-captures only on a mouse focus) gets its capture back too. It gives back what the
// game wants then (SetMouseWish: the viewport client's capture mode, cursor and lock mode, and the player controller's
// cursor), not what it recorded, so a game that changed its input mode meanwhile keeps its new one; only without a
// wish is the recorded state replayed. The lock is recorded and given back with or without a capture (a LockAlways
// viewport). While it is open and the topmost screen, every Tick takes the mouse and the keyboard focus again when it
// finds them outside: the engine gives both to the game viewport when the window is activated again (Alt+Tab back
// from the wallet: FSceneViewport::OnViewportActivated), and so does a SetInputMode(GameOnly) under the sheet. A screen
// below another never takes them, so two stacked screens do not fight. A screen closing below another hands the mouse
// over with the focus; a game that hid the platform cursor itself (SetPlatformCursorVisibility) is left as it is.
//
// FOCUS AND OTHER SCREENS. The focus the game had (every Slate user's) goes back to it when the screen closes
// (OnHidden). Several screens can be open at once, the later one on top: a rebuild never takes the focus from a
// screen on top; when that one closes the focus returns to the screen below it (to the control of the same key in
// its current build); and a screen closing below one that has the focus leaves it there, handing over where it would
// have given the focus back.
#pragma once

#include "CoreMinimal.h"
#include "Input/CursorReply.h"
#include "Input/Events.h"
#include "Input/NavigationReply.h"
#include "Input/Reply.h"
#include "Layout/Geometry.h"
#include "Types/SlateEnums.h"
#include "Types/SlateStructs.h"
#include "Widgets/SCompoundWidget.h"

#include "IScfgSheetHost.h"
#include "UI/ScfgStyle.h"

class SEditableTextBox;
class SScrollBox;
class STextBlock;
class SVerticalBox;

// What the game's viewport wants of the mouse now (FScfgDefaultSheetHost and UScfgSheetWidget read it from the game
// viewport client and its player controller): the screen gives the mouse back by it, not by what it recorded at open.
struct FScfgMouseWish
{
	// The viewport's widget (the captor and the lock target); null: unknown, the recorded state is replayed.
	TSharedPtr<SWidget> Viewport;
	bool bCapture = false;
	bool bHighPrecision = false;
	bool bLock = false;
};

class SCFG_API SScfgScreen : public SCompoundWidget
{
public:
	virtual ~SScfgScreen() override;

	// --- for the host ---

	// The facade's handle for this screen: CloseWith(how) closes it.
	void BindScreen(const TSharedRef<FScfgScreen>& InScreen) { Screen = InScreen; }
	TSharedPtr<FScfgScreen> GetScreen() const { return Screen.Pin(); }
	// The widget is now in the host's layer: remember who had the focus, take it (Godot's _ready).
	void OnShown();
	// The widget is about to leave the layer: give the focus back (Godot's _exit_tree). bRestoreFocus false: only
	// forget the screen (a teardown inside garbage collection must not run focus handlers of game widgets).
	void OnHidden(bool bRestoreFocus = true);
	// Whether the captor's viewport locks the cursor while it captures (UGameViewportClient::LockDuringCapture), set by
	// the host before OnShown; unset: locked when the capture was a high-precision one.
	void SetMouseLockHint(bool bLocks) { CaptorLocks = bLocks; }
	// What the game's viewport wants of the mouse at a given moment (see FScfgMouseWish), set by the host.
	void SetMouseWish(TFunction<FScfgMouseWish()> InWish) { MouseWish = MoveTemp(InWish); }
	// Shown (OnShown) and not hidden since.
	bool IsOpen() const;
	// A screen in a host that cannot say when it is shown again (UScfgSheetWidget: UMG may keep or replace its Slate
	// wrapper): the first Tick after it is painted again calls OnShown.
	void SetShowWhenPainted(bool bShow) { bShowWhenPainted = bShow; }
	// Who held the mouse capture when the screen opened (null: nobody, or the screen gave it back).
	TSharedPtr<SWidget> HeldMouseCaptor() const { return bMouseHeld ? MouseCaptor.Pin() : nullptr; }
	// The player is done: once, FScfgScreen::Close(How).
	void CloseWith(const FString& How);
	bool IsEnded() const { return bEnded; }
	// The screens shown and not hidden yet, oldest first.
	static TArray<TSharedRef<SScfgScreen>> OpenScreens();

	// --- what tests and drivers read (Godot's control(), focus_order(), ...) ---

	// Rebuild now if what the screen shows changed (Godot's _update; Tick calls it).
	void Refresh();
	// The control registered under Key in the current build, or null.
	TSharedPtr<SWidget> Control(const FString& Key) const;
	// Key is in the current build and shown (not collapsed).
	bool IsShown(const FString& Key) const;
	// The focusable controls of the current build that are shown, in focus order.
	TArray<FString> FocusOrder() const;
	FString FocusedKey() const;
	// Presses the button (or submits the field) Key, as a click would; false when there is none, or it is disabled.
	bool Press(const FString& Key);
	// The text of a label, of a button's label or of a field.
	FString TextOf(const FString& Key) const;
	// Every text the current build shows (labels, buttons, fields), in layout order.
	TArray<FString> VisibleTexts() const;
	// Move the focus to Key (Godot's grab_focus()).
	void FocusKey(const FString& Key);
	// The navigation and actions of a key press (OnPreviewKeyDown); true when the screen used it.
	bool HandleKey(const FKeyEvent& Event);
	int32 NumBuilds() const { return Builds; }
	const FScfgStyle& GetStyle() const { return *Style; }

	// --- SWidget ---

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

protected:
	// Builds the frame (dim, docked sheet, empty layout); the subclass's Construct calls it.
	void ConstructScreen(const TSharedRef<FScfgStyle>& InStyle);

	// --- the subclass's part ---

	virtual bool ReadyToShow() const { return true; }
	// What the current build shows: a rebuild when it changes.
	virtual FString Signature() const = 0;
	// Fills the layout (GetLayout()) with the builders below.
	virtual void BuildContent() = 0;
	// The step the build shows: a change moves the focus to the new step's primary control instead of keeping it.
	virtual FString StepKey() const { return FString(); }
	virtual FString PrimaryKey() const { return FString(); }
	// Copies the controller's field values into the text fields when they changed from outside.
	virtual void Sync() {}
	virtual void OnBack() {}

	// --- builders (each keyed one is registered; a focusable one joins the focus order) ---

	SVerticalBox& GetLayout() const { return *Layout; }
	// The docked sheet's width now (FScfgStyle::SheetWidthFor the screen's last size; 520 before the first tick).
	float PanelWidthNow() const { return FScfgStyle::SheetWidthFor(LastSize); }
	// Its inner width (the panel's 44-unit side margins taken off).
	float ContentWidthNow() const { return FMath::Max(0.f, PanelWidthNow() - 88.f); }
	TSharedRef<STextBlock> Label(const FString& Text, FName StyleName = TEXT("Scfg.Text"), bool bWrap = true);
	// A value that may be long and has no spaces (an address, a hash): wraps anywhere.
	TSharedRef<STextBlock> Mono(const FString& Text);
	// Variation: Scfg.Button, Scfg.Button.Small, Scfg.Button.Primary, Scfg.Button.Accent. Row groups buttons side by
	// side for left and right.
	TSharedRef<SWidget> Button(const FString& Key, const FString& Text, FName Variation, TFunction<void()> OnPress,
		const FString& Row = FString(), bool bEnabled = true, bool bVisible = true);
	// A segmented option: a label and an optional smaller second line, filled with the accent when selected.
	TSharedRef<SWidget> Option(const FString& Key, const FString& Text, const FString& Sub, bool bSelected,
		TFunction<void()> OnPress, const FString& Row = FString());
	// A text field: at most MaxLength characters, only those in Allowed; OnText gets the cleaned text, OnSubmit the
	// Enter key (and the gamepad's confirm).
	TSharedRef<SWidget> Field(const FString& Key, const FString& Text, int32 MaxLength, const FString& Allowed,
		TFunction<void(const FString&)> OnText, TFunction<void()> OnSubmit, const FString& Row = FString(),
		bool bReadOnly = false);
	// The back arrow (drawn, no font needs the glyph).
	TSharedRef<SWidget> BackArrow(const FString& Key, TFunction<void()> OnPress, bool bVisible);
	// Registers a widget that is not focusable under Key (a title, the price, the QR view).
	TSharedRef<SWidget> Reg(const FString& Key, const TSharedRef<SWidget>& Widget);
	TSharedRef<SWidget> Rule() const;
	TSharedRef<SWidget> Dot(const FLinearColor& Color) const;
	// The status line: a dot in Color and the text.
	TSharedRef<SWidget> StatusRow(const FString& Text, const FLinearColor& Color);
	// The developer's logo, fitted into 240x64, registered as "logo"; null without one.
	TSharedPtr<SWidget> Logo();
	// A vertical scrolling body (OutBody) that keeps the focused control in view.
	TSharedRef<SWidget> ScrollingBody(TSharedPtr<SVerticalBox>& OutBody);
	// Replaces a field's text (Sync) without calling its OnText.
	void SetFieldText(const FString& Key, const FString& Text);

private:
	struct FEntry
	{
		FString Key;
		TSharedRef<SWidget> Focus;  // what takes Slate's focus
		FString Row;
		TFunction<void()> Activate;
		bool bField = false;
		bool bEnabled = true;
		bool bVisible = true;
	};

	TSharedRef<SWidget> Ring(const FString& Key, const TSharedRef<SWidget>& Inner);
	void AddEntry(FEntry Entry);
	const FEntry* Find(const FString& Key) const;
	TArray<const FEntry*> Shown() const;
	void Move(EUINavigation Direction);
	// The control Direction leads to from the current one (the primary one when none has the focus).
	const FEntry* Target(EUINavigation Direction);
	void FocusEntry(const FEntry& Entry);
	void FocusKeyOrPrimary(const FString& Keep);
	// Slate's focus of user 0, if it is on one of this build's controls: CurrentKey follows it.
	void ResyncFocus();
	// Another screen opened after this one has the focus.
	bool Covered() const;
	bool HasFocusWithin(int32 UserIndex) const;
	FOptionalSize PanelWidth() const;

	TSharedPtr<FScfgStyle> Style;
	TWeakPtr<FScfgScreen> Screen;
	TSharedPtr<SVerticalBox> Layout;
	TArray<TSharedPtr<SScrollBox>> Scrolls;
	TArray<FEntry> Entries;
	TMap<FString, TSharedRef<SWidget>> Keyed;
	TMap<FString, TSharedRef<SEditableTextBox>> Fields;
	TOptional<FString> LastSignature;
	FString LastStep;
	FString CurrentKey;
	FVector2D LastSize = FVector2D::ZeroVector;
	int32 Builds = 0;
	bool bEnded = false;
	bool bSettingField = false;
	// The focus could not be given to CurrentKey's control yet (no path to it): Tick tries again.
	bool bFocusPending = false;
	// Godot's _back_focus / _back_screen / _back_key.
	TMap<int32, TWeakPtr<SWidget>> BackFocus;
	TWeakPtr<SScfgScreen> BackScreen;
	FString BackKey;
	// The mouse capture released in OnShown (see THE MOUSE).
	void TakeMouse();
	void GiveMouseBack();
	TWeakPtr<SWidget> MouseCaptor;
	bool bMouseHeld = false;
	bool bMouseHighPrecision = false;
	bool bMouseLocked = false;
	TOptional<bool> CaptorLocks;
	TFunction<FScfgMouseWish()> MouseWish;
	// The open screen drawn over all others (the one that takes the mouse and the focus back).
	bool IsTopmost() const;
	// Tick: on top, open, and the mouse capture or the keyboard focus is outside: take them again.
	void KeepInput();
	bool bShowWhenPainted = false;
	// The left stick moved past the threshold on an axis (one step per push).
	bool bStickX = false;
	bool bStickY = false;
};
