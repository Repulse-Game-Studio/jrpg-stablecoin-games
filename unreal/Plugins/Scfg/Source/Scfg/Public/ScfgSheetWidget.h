// Copyright 2026 Repulse
// No Python twin: the plugin's own payment sheet or restore screen as a code-built UUserWidget, for a game whose
// sheet host shows its UI through UMG (no .uasset: RebuildWidget returns the Slate screen).
//
//     TWeakObjectPtr<UScfgSheetWidget> Widget;   // weak: a plain C++ host does not keep a UObject alive
//
//     virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override
//     {
//         UScfgSheetWidget* Made = CreateWidget<UScfgSheetWidget>(PlayerController);
//         TSharedRef<FScfgScreen> Screen = Made->BindPayment(Checkout);
//         Made->AddToViewport(10000);           // or into the game's own UMG tree
//         Widget = Made;
//         return Screen;
//     }
//     virtual void Close(const TSharedRef<FScfgScreen>& Screen) override
//     {
//         // may run at game-instance shutdown, inside garbage collection: check the widget, create no UObject
//         if (UScfgSheetWidget* Shown = Widget.Get()) { Shown->RemoveFromParent(); }
//     }
//
// Bind once, before the widget is shown; it can then be removed and added again (moved between containers, a widget
// stack): the same screen comes back, its flow still waiting (it shows itself when painted again; a core-ticker check
// notices when it left every window). The screen takes the focus and the mouse when it is
// constructed (NativeConstruct) and gives them back when it is destructed (NativeDestruct, also on removal; not while
// garbage is being collected or this widget or its game instance is being collected). Closing it creates no UObject.
// Without a binding it shows nothing.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "Styling/ISlateStyle.h"

#include "IScfgSheetHost.h"

#include "ScfgSheetWidget.generated.h"

class FScfgCheckout;
class FScfgRestore;
class SScfgScreen;

UCLASS(NotBlueprintable)
class SCFG_API UScfgSheetWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// Show the payment sheet of Checkout; the screen handle to return from OpenPayment. StyleOverride as
	// FScfgDefaultSheetHost::SetStyleOverride.
	TSharedRef<FScfgScreen> BindPayment(const TSharedRef<FScfgCheckout>& Checkout,
		TSharedPtr<const ISlateStyle> StyleOverride = nullptr);
	// Show the restore screen of Restore.
	TSharedRef<FScfgScreen> BindRestore(const TSharedRef<FScfgRestore>& Restore,
		TSharedPtr<const ISlateStyle> StyleOverride = nullptr);
	// The bound handle, or null.
	TSharedPtr<FScfgScreen> GetScreen() const { return Handle; }
	// The Slate screen (SScfgPaymentSheet / SScfgRestoreScreen), or null before a binding.
	TSharedPtr<SScfgScreen> GetSlateScreen() const { return Screen; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void BeginDestroy() override;

private:
	bool CanRestoreFocus() const;
	void Show();
	void Hide(bool bRestoreFocus);
	void HideIfRemoved();
	TSharedRef<FScfgScreen> Bind(const TSharedRef<SScfgScreen>& InScreen);

	TSharedPtr<SScfgScreen> Screen;
	TSharedPtr<FScfgScreen> Handle;
	FTSTicker::FDelegateHandle TickHandle;
};
