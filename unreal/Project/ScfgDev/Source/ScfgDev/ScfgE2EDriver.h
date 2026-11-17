// The showcase's end-to-end driver: tools/run_unreal_e2e.py plays the showcase through it the way a player does, as
// godot/demo/e2e/e2e_driver.gd does for the Godot demo. Compiled only outside Shipping, and made only when the command
// line has -scfg-e2e=<config.json>; without it the showcase runs as a player gets it.
//
// The config (JSON object, written by the runner):
//   configure  {wallet?, solana_wallet?, testnet?, chains?, tokens?}: Configure()'s options (Python's keyword names)
//   items      {id: {code, price, token?, title?, min_price?}}: AddItem() for each
//   chain      {network: "mainnet"|"testnet", key, rpc: [url], confirmations?, tokens?: {name: address}} (optional):
//              the built-in chain entry is pointed at the local test node and its mock token, as the integration
//              driver's override (unreal/core-driver/Main.cpp, apply_override) and the Godot driver's SCFG_E2E_CHAIN do
//   control    {port, token}: the runner's control socket on 127.0.0.1 and the token the driver greets it with
//   draw_world false (optional): UGameViewportClient::bDisableWorldRendering, for a software renderer (the runner's
//              lavapipe draws even the empty Entry map at under one frame a second; the showcase itself always
//              draws its world, Shipping included)
// A config that is not a JSON object of that shape is logged ("LogScfgDev: Error: e2e config <path>: <why>") and the
// showcase quits with exit code 1.
//
// Protocol: the driver connects to 127.0.0.1:<port> (never anything else) and sends {"hello": <token>, "pid": <pid>}.
// Then one JSON command per line, {"id": n, "op": ..., ...}, one reply per command in order: {"id": n, "ok": <result>}
// or {"id": n, "error": "<why>"}. A command may take several frames (a click is a move, then a press, then a release on
// later frames). The connection closing quits the game the normal way.
//
// Commands (input goes through FSlateApplication's platform message handler, the entry the SDL events of a real
// window use: OnMouseMove after the cursor was warped there, OnMouseDown / OnMouseUp, OnMouseWheel, OnKeyDown /
// OnKeyUp, OnKeyChar):
//   snapshot             what the player sees: the open screens (oldest first: kind "sheet" or "restore", the step,
//                        the texts, the controls {name, text, disabled, field, rect}, the focused control), the HUD
//                        ({texts, buttons}), the toasts, the URLs the game opened, the clipboard, whether the game is
//                        paused, the clicks the world got, the window size and the input state (see `input`)
//   click {name, in}     a real left click at the middle of the control `name` of the newest screen (in = "screen")
//                        or of the HUD button `name` (in = "game"); a control below the fold of the sheet's scrolling
//                        body is scrolled into view with the wheel first. Replies {at, wheel_notches}
//   click_at {x, y}      a real left click at a desktop position (the world's part of the screen); replies {under}
//   key {key, repeat?}   a key pressed and released (an FKey name: Enter, Escape, Tab, Down, ...)
//   type {text}          characters typed
//   owns {item}          UScfgSubsystem::Owns
//   saved_dir            FPaths::ProjectSavedDir(), absolute (Saved/Scfg/state.json is under it)
//   input                who has the keyboard focus and the mouse capture ("sheet", "viewport", "hud", "none" or a
//                        widget type), high-precision mode, whether the cursor is locked to the window (SDL's mouse
//                        rect; null where unknown), whether the application and the game window are active
//   input_mode {mode}    the game's input mode: "ui" (the cursor shown, game and UI: the showcase's own), "game"
//                        (game only: the cursor hidden, the mouse captured permanently), "lock_always" (the cursor
//                        shown and locked to the window)
//   open {what}          the showcase's Unlock / Tip / Restore action (for the input modes whose cursor cannot click
//                        the HUD)
//   activation_cycle     a real native window opens over the game (the game window is deactivated), then closes (the
//                        engine activates the game window again: FSceneViewport::OnViewportActivated); replies what
//                        happened and the focus changes the engine made meanwhile, and the input state 5 frames later
//   wish                 the sheet's mouse wish (ScfgDefaultSheetHostDetail::WishOf) and whether its viewport is the
//                        game window's real SViewport
//   screenshot {path}    a screenshot with the UI (FScreenshotRequest), replied once the file is written
//   quit                 reply, then quit the normal way (the subsystem saves its state)
//
// The browser and the clipboard are real: FCoreDelegates::ShouldLaunchUrl records every URL the game opens and refuses
// the launch (no xdg-open, no browser of the developer's), and the clipboard is the X server's (Xvfb's).
#pragma once

#if !UE_BUILD_SHIPPING

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ScfgTypes.h"

class FSocket;
class SWidget;
class SWindow;
class UScfgShowcaseGameInstance;
struct FFocusEvent;
class FWeakWidgetPath;
class FWidgetPath;

class FScfgE2EDriver : public TSharedFromThis<FScfgE2EDriver>
{
public:
	// The command line asks for the driver: OutPath is the config's path.
	static bool Requested(FString& OutPath);
	// Reads and checks the config and points the chain at the local node; null (logged) when the config is unusable.
	static TSharedPtr<FScfgE2EDriver> Create(const FString& Path, UScfgShowcaseGameInstance* InGame);
	~FScfgE2EDriver();

	const FScfgOptions& Options() const { return ConfigOptions; }
	const TMap<FString, FScfgItem>& Items() const { return ConfigItems; }
	// Connects to the runner and serves it from the core ticker; false (logged) when it cannot connect.
	bool Start();

private:
	using FStep = TFunction<bool()>;  // true: done

	bool Tick(float DeltaTime);
	void ReadCommands();
	void Flush();
	void Send(const TSharedRef<FJsonObject>& Message);
	void Reply(int64 Id, const TSharedPtr<FJsonValue>& Ok);
	void ReplyError(int64 Id, const FString& Why);
	// Runs one command: replies now, or queues the steps that will.
	void Run(int64 Id, const TSharedRef<FJsonObject>& Command);

	TSharedRef<FJsonObject> Snapshot();
	TSharedRef<FJsonObject> InputState();
	TSharedRef<FJsonObject> Wish();
	FString Owner(const TSharedPtr<SWidget>& Widget) const;
	TSharedPtr<SWindow> GameWindow() const;
	TSharedPtr<SWidget> ViewportWidget() const;
	bool Click(int64 Id, const FString& Name, const FString& Where);
	void ClickAt(int64 Id, const FVector2D& At);
	void Key(const FString& Name, bool bRepeat);
	void SetInputMode(const FString& Mode);
	void ActivationCycle(int64 Id);
	void OnFocusChanging(const FFocusEvent& Event, const FWeakWidgetPath& OldPath, const TSharedPtr<SWidget>& OldWidget,
		const FWidgetPath& NewPath, const TSharedPtr<SWidget>& NewWidget);

	FScfgOptions ConfigOptions;
	TMap<FString, FScfgItem> ConfigItems;
	int32 ControlPort = 0;
	// The config's draw_world (default true); false: the game viewport draws no world (set once it exists).
	bool bDrawWorld = true;
	bool bWorldSet = false;
	FString ControlToken;

	TWeakObjectPtr<UScfgShowcaseGameInstance> Game;
	FSocket* Socket = nullptr;
	TArray<uint8> Inbox;
	TArray<uint8> Outbox;
	TArray<TSharedRef<FJsonObject>> Commands;
	TArray<FStep> Steps;
	// A step replied with an error: the rest of its command's steps are dropped.
	bool bAbortSteps = false;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle FocusHandle;
	TArray<FString> Opened;
	bool bRecordFocus = false;
	TArray<TSharedPtr<FJsonValue>> FocusEvents;
	bool bQuitting = false;
};

#endif  // !UE_BUILD_SHIPPING
