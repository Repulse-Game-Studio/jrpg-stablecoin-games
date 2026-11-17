// Twin of godot/addons/scfg/ui/default_sheet_host.gd (see ScfgDefaultSheetHost.h).
#include "ScfgDefaultSheetHost.h"

#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "UObject/GarbageCollection.h"

#include "ScfgCheckoutController.h"
#include "ScfgControllers.h"
#include "ScfgLog.h"
#include "ScfgString.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/SScfgRestoreScreen.h"
#include "UI/SScfgToast.h"
#include "UI/ScfgStyle.h"

#include "Scfg/Config.h"

FScfgDefaultSheetHost::FScfgDefaultSheetHost(UGameInstance* InGameInstance) : GameInstance(InGameInstance) {}

bool FScfgDefaultSheetHost::CanRestoreFocus() const
{
	// Inside garbage collection, or the game instance is being collected (its BeginDestroy runs in the incremental
	// purge, after the collection proper): the game's widgets may be going too.
	return !IsGarbageCollecting() && GameInstance.IsValid();
}

FScfgDefaultSheetHost::~FScfgDefaultSheetHost()
{
	const bool bFocus = CanRestoreFocus();
	for (const FShown& S : Shown)
	{
		S.Widget->OnHidden(bFocus);
		Detach(S.Widget, S.Viewport);
	}
	Shown.Reset();
	for (const FToast& T : Toasts)
	{
		Detach(T.Widget, T.Viewport);
	}
	Toasts.Reset();
}

UGameViewportClient* FScfgDefaultSheetHost::Viewport()
{
	UGameInstance* Instance = GameInstance.Get();
	UGameViewportClient* Client = Instance ? Instance->GetGameViewportClient() : nullptr;
	if (!Client && !bWarnedNoViewport)
	{
		bWarnedNoViewport = true;
		UE_LOG(LogScfg, Warning,
			TEXT("scfg: the payment sheet needs a game viewport, and this game instance has none (set your own sheet host "
				 "with SetSheetHost to show it elsewhere)"));
	}
	return Client;
}

void FScfgDefaultSheetHost::Detach(const TSharedRef<SWidget>& Widget, const TWeakObjectPtr<UGameViewportClient>& Viewport)
{
	// The client may be gone (its overlay, and our widget with it) or being collected: never revived here.
	if (UGameViewportClient* Client = Viewport.Get())
	{
		Client->RemoveViewportWidgetContent(Widget);
	}
}

TSharedPtr<FScfgScreen> FScfgDefaultSheetHost::Show(const TSharedRef<SScfgScreen>& Widget)
{
	UGameViewportClient* Client = Viewport();
	if (!Client)
	{
		return nullptr;
	}
	const TSharedRef<FScfgScreen> Handle = MakeShared<FScfgScreen>();
	Widget->BindScreen(Handle);
	Client->AddViewportWidgetContent(Widget, ZORDER);
	Shown.Add(FShown{Handle, Widget, Client});
	Widget->OnShown();
	return Handle;
}

TSharedPtr<FScfgScreen> FScfgDefaultSheetHost::OpenPayment(const TSharedRef<FScfgCheckout>& Checkout)
{
	const FScfgCheckoutController View(Checkout);
	const TSharedRef<FScfgStyle> Style = FScfgStyle::Create(View.Accent(), View.LogoPath(), StyleOverride);
	return Show(SNew(SScfgPaymentSheet, View, Style));
}

TSharedPtr<FScfgScreen> FScfgDefaultSheetHost::OpenRestore(const TSharedRef<FScfgRestore>& Restore)
{
	const FScfgRestoreController View(Restore);
	const TSharedRef<FScfgStyle> Style = FScfgStyle::Create(View.Accent(), View.LogoPath(), StyleOverride);
	return Show(SNew(SScfgRestoreScreen, View, Style));
}

void FScfgDefaultSheetHost::Close(const TSharedRef<FScfgScreen>& Screen)
{
	const int32 I = Shown.IndexOfByPredicate([&Screen](const FShown& S) { return S.Handle == Screen; });
	if (I == INDEX_NONE)
	{
		return;
	}
	const FShown Going = Shown[I];
	Shown.RemoveAt(I);
	Going.Widget->OnHidden(CanRestoreFocus());
	Detach(Going.Widget, Going.Viewport);
}

void FScfgDefaultSheetHost::Notify(const FString& Text)
{
	UGameViewportClient* Client = Viewport();
	if (!Client)
	{
		return;
	}
	TWeakPtr<FScfgDefaultSheetHost> Weak = AsShared();
	const TSharedRef<FScfgStyle> Style =
		FScfgStyle::Create(ScfgString::FromUtf8(scfg::config::DEFAULT_ACCENT), FString(), StyleOverride);
	const TSharedRef<SScfgToast> Toast = SNew(SScfgToast, Style)
		.Text(Text)
		.OnFinished_Lambda([Weak]()
		{
			// Not from inside the toast's own tick (a paint pass): the next core-ticker frame removes it.
			const TSharedPtr<FScfgDefaultSheetHost> Host = Weak.Pin();
			if (!Host.IsValid() || Host->bSweepQueued)
			{
				return;
			}
			Host->bSweepQueued = true;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
			{
				if (const TSharedPtr<FScfgDefaultSheetHost> Again = Weak.Pin())
				{
					Again->RemoveFinishedToasts();
				}
				return false;
			}));
		});
	Client->AddViewportWidgetContent(Toast, ZORDER + 1);
	Toasts.Add(FToast{Toast, Client});
}

void FScfgDefaultSheetHost::RemoveFinishedToasts()
{
	bSweepQueued = false;
	for (int32 I = Toasts.Num() - 1; I >= 0; --I)
	{
		if (Toasts[I].Widget->IsFinished())
		{
			const FToast Going = Toasts[I];
			Toasts.RemoveAt(I);
			Detach(Going.Widget, Going.Viewport);
		}
	}
}

TArray<TSharedRef<SScfgScreen>> FScfgDefaultSheetHost::Screens() const
{
	TArray<TSharedRef<SScfgScreen>> Out;
	for (const FShown& S : Shown)
	{
		Out.Add(S.Widget);
	}
	return Out;
}

TArray<FString> FScfgDefaultSheetHost::ToastTexts() const
{
	TArray<FString> Out;
	for (const FToast& T : Toasts)
	{
		Out.Add(T.Widget->GetText());
	}
	return Out;
}
