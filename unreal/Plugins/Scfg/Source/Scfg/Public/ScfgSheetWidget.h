// No Python twin: the plugin's own payment sheet or restore screen as a code-built UUserWidget, for a game whose
// sheet host shows its UI through UMG (no .uasset: RebuildWidget returns the Slate screen).
//
//     virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override
//     {
//         Widget = CreateWidget<UScfgSheetWidget>(PlayerController);
//         TSharedRef<FScfgScreen> Screen = Widget->BindPayment(Checkout);
//         Widget->AddToViewport(10000);       // or into the game's own UMG tree
//         return Screen;
//     }
//     virtual void Close(const TSharedRef<FScfgScreen>& Screen) override { Widget->RemoveFromParent(); }
//
// Bind once, before the widget is shown. The screen takes the focus when it is constructed (NativeConstruct) and
// gives it back when it is destructed (NativeDestruct; not while garbage is being collected). Closing it creates no
// UObject (the facade may close it inside garbage collection). Without a binding it shows nothing.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
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

private:
	TSharedRef<FScfgScreen> Bind(const TSharedRef<SScfgScreen>& InScreen);

	TSharedPtr<SScfgScreen> Screen;
	TSharedPtr<FScfgScreen> Handle;
	bool bShown = false;
};
