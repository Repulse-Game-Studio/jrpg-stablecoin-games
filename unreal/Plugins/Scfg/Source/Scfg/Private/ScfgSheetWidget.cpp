// No Python twin: the UMG wrapper of the Slate screens (see ScfgSheetWidget.h).
#include "ScfgSheetWidget.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Engine/GameViewportClient.h"
#include "UObject/GarbageCollection.h"
#include "Widgets/Layout/SSpacer.h"

#include "ScfgCheckoutController.h"
#include "ScfgControllers.h"
#include "ScfgDefaultSheetHost.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/SScfgRestoreScreen.h"
#include "UI/ScfgStyle.h"

TSharedRef<FScfgScreen> UScfgSheetWidget::Bind(const TSharedRef<SScfgScreen>& InScreen)
{
	Screen = InScreen;
	const TSharedRef<FScfgScreen> Made = MakeShared<FScfgScreen>();
	Handle = Made;
	InScreen->BindScreen(Made);
	// UMG may keep its Slate wrapper across a removal and an add, or make a new one: the screen shows itself when it
	// is painted again, and the ticker below notices when it left every window.
	InScreen->SetShowWhenPainted(true);
	if (!TickHandle.IsValid())
	{
		TWeakObjectPtr<UScfgSheetWidget> Weak(this);
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
		{
			UScfgSheetWidget* Self = Weak.Get();
			if (!Self)
			{
				return false;
			}
			Self->HideIfRemoved();
			return true;
		}));
	}
	return Made;
}

void UScfgSheetWidget::HideIfRemoved()
{
	FWidgetPath Path;
	if (Screen.IsValid() && Screen->IsOpen() && FSlateApplication::IsInitialized() &&
		!FSlateApplication::Get().FindPathToWidget(Screen.ToSharedRef(), Path))
	{
		Hide(CanRestoreFocus());
	}
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

bool UScfgSheetWidget::CanRestoreFocus() const
{
	// FScfgDefaultSheetHost's guard: not inside garbage collection, and neither this widget nor its game instance is
	// being collected (BeginDestroy runs in the incremental purge, where the collection flag is already false).
	const UGameInstance* Instance = GetGameInstance();
	return !IsGarbageCollecting() && !IsUnreachable() && !HasAnyFlags(RF_BeginDestroyed) &&
		(!Instance || (!Instance->IsUnreachable() && !Instance->HasAnyFlags(RF_BeginDestroyed)));
}

void UScfgSheetWidget::Show()
{
	if (!Screen.IsValid() || Screen->IsOpen())
	{
		return;
	}
	const UGameInstance* Instance = GetGameInstance();
	if (UGameViewportClient* Client = Instance ? Instance->GetGameViewportClient() : nullptr)
	{
		Screen->SetMouseLockHint(Client->LockDuringCapture());
		Screen->SetMouseWish(ScfgDefaultSheetHostDetail::WishOf(Client, const_cast<UGameInstance*>(Instance)));
	}
	Screen->OnShown();
}

void UScfgSheetWidget::Hide(bool bRestoreFocus)
{
	if (Screen.IsValid() && Screen->IsOpen())
	{
		Screen->OnHidden(bRestoreFocus);
	}
}

void UScfgSheetWidget::NativeConstruct()
{
	Super::NativeConstruct();
	Show();
}

void UScfgSheetWidget::NativeDestruct()
{
	Hide(CanRestoreFocus());
	Super::NativeDestruct();
}

void UScfgSheetWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	// Removed from its parent (or its Slate wrapper destroyed): hidden, but the screen is kept, so a host that adds
	// the widget again (moved between containers, a widget stack) shows the same screen, its flow still waiting.
	Hide(CanRestoreFocus());
	Super::ReleaseSlateResources(bReleaseChildren);
}

void UScfgSheetWidget::BeginDestroy()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Hide(false);
	Screen.Reset();
	Super::BeginDestroy();
}
