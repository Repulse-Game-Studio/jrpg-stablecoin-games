// Twin of godot/addons/scfg/ui/default_sheet_host.gd: the sheet host UScfgSubsystem uses when the game set none
// (SetSheetHost). It shows the plugin's own Slate payment sheet and restore screen (SScfgPaymentSheet,
// SScfgRestoreScreen), each as its own widget on the game instance's game viewport
// (UGameViewportClient::AddViewportWidgetContent) at ZORDER, above the game's own widgets, which it never touches;
// in the editor that is the PIE viewport. Notifications are short toasts (SScfgToast) one Z-order above.
//
// No viewport (a game instance without one: a dedicated server, a commandlet, the editor outside PIE): nothing is
// shown, OpenPayment / OpenRestore return null and Pay / Restore resolve Failed "sheet.unavailable" (logged).
//
// Teardown: the facade closes every screen through Close() before its worker goes, also when its game instance is
// collected without Shutdown (UScfgSubsystem::BeginDestroy, inside garbage collection). Close() and the destructor
// therefore create no UObject, read the viewport client only through a weak pointer, and do not hand the focus back
// while garbage is being collected or once the game instance is unreachable (focus handlers of the game's widgets
// could reach objects being destroyed). The host holds no UObject strongly.
//
// The mouse: a screen releases the viewport's mouse capture and cursor lock while it is open and gives them back on
// close (SScfgScreen.h, THE MOUSE); the host tells it whether this viewport locks the cursor with its capture.
//
// Look: the screens' FScfgStyle, with the configured accent and logo; SetStyleOverride() supplies an ISlateStyle whose
// widget styles and brushes win by name (ScfgStyle.h lists the names).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Styling/ISlateStyle.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "IScfgSheetHost.h"

class SScfgScreen;
class SScfgToast;
class SWidget;
class UGameInstance;
class UGameViewportClient;

struct FScfgMouseWish;

namespace ScfgDefaultSheetHostDetail
{
// What Client's game viewport wants of the mouse now, read when it is called (ScfgDefaultSheetHost.cpp).
SCFG_API TFunction<FScfgMouseWish()> WishOf(UGameViewportClient* Client, TWeakObjectPtr<UGameInstance> GameInstance);
}

class SCFG_API FScfgDefaultSheetHost : public IScfgSheetHost, public TSharedFromThis<FScfgDefaultSheetHost>
{
public:
	// The viewport Z-order of the screens (the toasts: one above).
	static constexpr int32 ZORDER = 10000;

	explicit FScfgDefaultSheetHost(UGameInstance* InGameInstance);
	// Removes what is still shown (no UObject created, see above).
	virtual ~FScfgDefaultSheetHost() override;

	virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override;
	virtual TSharedPtr<FScfgScreen> OpenRestore(const TSharedRef<FScfgRestore>& Restore) override;
	virtual void Close(const TSharedRef<FScfgScreen>& Screen) override;
	virtual void Notify(const FString& Text) override;

	void SetStyleOverride(TSharedPtr<const ISlateStyle> InOverride) { StyleOverride = MoveTemp(InOverride); }

	// The screens shown, oldest first, and their widgets (SScfgPaymentSheet / SScfgRestoreScreen): what tests and the
	// e2e driver read.
	TArray<TSharedRef<SScfgScreen>> Screens() const;
	int32 NumToasts() const { return Toasts.Num(); }
	TArray<FString> ToastTexts() const;
	// Every core-ticker frame while something is shown: a screen that left the viewport by another hand (it has no
	// parent widget any more) is released, so its flow resolves as "cancel" and its focus and mouse go back; faded
	// toasts are removed.
	void Tick();
	// The faded toasts leave the viewport (Tick does it).
	void RemoveFinishedToasts();

private:
	struct FShown
	{
		TSharedRef<FScfgScreen> Handle;
		TSharedRef<SScfgScreen> Widget;
		TWeakObjectPtr<UGameViewportClient> Viewport;
	};
	struct FToast
	{
		TSharedRef<SScfgToast> Widget;
		TWeakObjectPtr<UGameViewportClient> Viewport;
	};

	// The game instance's viewport client, or null (logged once per host).
	UGameViewportClient* Viewport();
	TSharedPtr<FScfgScreen> Show(const TSharedRef<SScfgScreen>& Widget);
	bool CanRestoreFocus() const;
	static void Detach(const TSharedRef<SWidget>& Widget, const TWeakObjectPtr<UGameViewportClient>& Viewport);

	TWeakObjectPtr<UGameInstance> GameInstance;
	TSharedPtr<const ISlateStyle> StyleOverride;
	TArray<FShown> Shown;
	TArray<FToast> Toasts;
	void EnsureTicking();

	FTSTicker::FDelegateHandle TickHandle;
	bool bWarnedNoViewport = false;
};
