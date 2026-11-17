// Twin of godot/addons/scfg/sheet_host.gd (ScfgSheetHost) and Unity's IScfgSheetHost / IScfgScreen: the interface
// between the payment and restore flows (UScfgSubsystem's Pay and Restore) and the screens that show them. The
// subsystem holds one (SetSheetHost); task 7's Slate sheet is the default one, a test host scripts the player.
//
// A host shows a screen for a controller and hands back an FScfgScreen (its own, or a subclass), which it keeps alive
// while the screen is shown. The screen calls Close(How) once when the player is done with it:
//   - payment sheet: How = "cancel" (the player gave up), "keep_playing" (close the sheet but keep the payment and its
//     browser page alive; the final result arrives later) or "finish" (close after a final state: paid, underpaid or
//     expired). Any other word counts as "finish". These are Ren'Py's _close_checkout hows.
//   - restore screen: any word (the restore screen only closes).
// The flows pick a Close up on the next frame of their pump (Unity's rule; Godot resumes inside the signal), so Close
// may be called from anywhere, OpenPayment / OpenRestore included, and never re-enters the flows.
//
// Lifetime of the returned screen (the flows hold it weakly):
//   - released by the host (its last TSharedPtr gone) without Close: counts as "cancel", noticed on the next frame,
//     and not handed to Close(). This is Godot's "freed without emitting closed".
//   - the game instance shuts down while it is open: Pay / Restore resolve with what they have so far and nothing is
//     delivered (no callback, event or notification; Ren'Py's _shutdown); Close(screen) is called for a screen that
//     still exists.
//
// The flows poll the controller every frame, so a screen only reads its fields and calls its methods, never poll().
// The buttons that leave the game are the controller wrappers' OpenBrowser, OpenWalletApp, OpenExplorer, Copy and
// Paste (FScfgCheckout / FScfgRestore, ScfgControllers.h). The flows close the controller (stopping its browser page)
// before they call Close(screen).
//
// This base shows nothing: OpenPayment and OpenRestore return null, and Pay / Restore then resolve Failed with error
// "sheet.unavailable" (logged once). A plain C++ interface rather than a UInterface: the screens are Slate (task 7),
// and a UMG host binds through task 7's UScfgCheckoutController.
#pragma once

#include "CoreMinimal.h"

class FScfgCheckout;
class FScfgRestore;

// A screen a host opened (see above).
class FScfgScreen
{
public:
	virtual ~FScfgScreen() = default;

	// The player is done with the screen. Once: a later call does nothing.
	void Close(const FString& InHow)
	{
		if (!How.IsSet())
		{
			How = InHow;
		}
	}
	bool IsClosed() const { return How.IsSet(); }
	// How it was closed ("" before Close).
	FString GetHow() const { return How.Get(FString()); }

private:
	TOptional<FString> How;
};

// Shows the payment sheet, the restore screen and short notices (see above).
class IScfgSheetHost
{
public:
	virtual ~IScfgSheetHost() = default;

	// Show the payment sheet for Checkout; its screen, or null when no sheet can be shown.
	virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout)
	{
		(void)Checkout;
		return nullptr;
	}
	// Show the restore screen for Restore; its screen, or null when none can be shown.
	virtual TSharedPtr<FScfgScreen> OpenRestore(const TSharedRef<FScfgRestore>& Restore)
	{
		(void)Restore;
		return nullptr;
	}
	// Remove a screen opened by OpenPayment / OpenRestore, after its controller was closed.
	virtual void Close(const TSharedRef<FScfgScreen>& Screen) { (void)Screen; }
	// Show a short, already worded message to the player. Payment and restore announcements come only when the
	// subsystem's Notify is on.
	virtual void Notify(const FString& Text) { (void)Text; }
};
