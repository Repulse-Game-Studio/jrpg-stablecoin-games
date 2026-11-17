// No Python twin: the UMG wrapper of the Slate screens (see ScfgSheetWidget.h).
#include "ScfgSheetWidget.h"

#include "UObject/GarbageCollection.h"
#include "Widgets/Layout/SSpacer.h"

#include "ScfgCheckoutController.h"
#include "ScfgControllers.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/SScfgRestoreScreen.h"
#include "UI/ScfgStyle.h"

TSharedRef<FScfgScreen> UScfgSheetWidget::Bind(const TSharedRef<SScfgScreen>& InScreen)
{
	Screen = InScreen;
	const TSharedRef<FScfgScreen> Made = MakeShared<FScfgScreen>();
	Handle = Made;
	InScreen->BindScreen(Made);
	return Made;
}

TSharedRef<FScfgScreen> UScfgSheetWidget::BindPayment(
	const TSharedRef<FScfgCheckout>& Checkout, TSharedPtr<const ISlateStyle> StyleOverride)
{
	const FScfgCheckoutController View(Checkout);
	return Bind(SNew(SScfgPaymentSheet, View, FScfgStyle::Create(View.Accent(), View.LogoPath(), StyleOverride)));
}

TSharedRef<FScfgScreen> UScfgSheetWidget::BindRestore(
	const TSharedRef<FScfgRestore>& Restore, TSharedPtr<const ISlateStyle> StyleOverride)
{
	const FScfgRestoreController View(Restore);
	return Bind(SNew(SScfgRestoreScreen, View, FScfgStyle::Create(View.Accent(), View.LogoPath(), StyleOverride)));
}

TSharedRef<SWidget> UScfgSheetWidget::RebuildWidget()
{
	if (Screen.IsValid())
	{
		return Screen.ToSharedRef();
	}
	return SNew(SSpacer);
}

void UScfgSheetWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (Screen.IsValid() && !bShown)
	{
		bShown = true;
		Screen->OnShown();
	}
}

void UScfgSheetWidget::NativeDestruct()
{
	if (Screen.IsValid() && bShown)
	{
		bShown = false;
		Screen->OnHidden(!IsGarbageCollecting());
	}
	Super::NativeDestruct();
}

void UScfgSheetWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	if (Screen.IsValid() && bShown)
	{
		bShown = false;
		Screen->OnHidden(false);
	}
	Screen.Reset();
}
