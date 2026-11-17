// Twin of godot/addons/scfg/ui/default_sheet_host.gd (see ScfgDefaultSheetHost.h).
#include "ScfgDefaultSheetHost.h"

#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SViewport.h"
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

namespace ScfgDefaultSheetHostDetail
{

// What Client's viewport wants of the mouse now: FSceneViewport::AcquireFocusAndCapture's rules (SceneViewport.cpp:
// 731-790, OnFocusReceived :1419-1437): a permanent capture mode and input not ignored capture, with a player
// controller (or in PIE); hidden (high precision) unless the controller shows the cursor and the client does not hide
// it during capture; locked with the capture by the lock mode (bLock), or always (bAlwaysLock).
TFunction<FScfgMouseWish()> WishOf(UGameViewportClient* Client, TWeakObjectPtr<UGameInstance> GameInstance)
{
	TWeakObjectPtr<UGameViewportClient> Weak(Client);
	return [Weak, GameInstance]()
	{
		FScfgMouseWish Wish;
		UGameViewportClient* C = Weak.Get();
		if (!C)
		{
			return Wish;
		}
		Wish.Viewport = C->GetGameViewportWidget();
		UGameInstance* Instance = GameInstance.Get();
		APlayerController* PC = Instance ? Instance->GetFirstLocalPlayerController() : nullptr;
		const UWorld* World = C->GetWorld();
		const EMouseCaptureMode Mode = C->GetMouseCaptureMode();
		Wish.bCapture = !C->IgnoreInput() && (PC || (World && World->IsPlayInEditor())) &&
			(Mode == EMouseCaptureMode::CapturePermanently || Mode == EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
		const bool bShowsCursor = PC && PC->ShouldShowMouseCursor();
		Wish.bHighPrecision = Wish.bCapture && (!bShowsCursor || C->HideCursorDuringCapture());
		Wish.bLock = Wish.bCapture && C->LockDuringCapture();
		Wish.bAlwaysLock = C->ShouldAlwaysLockMouse();
		return Wish;
	};
}

}  // namespace ScfgDefaultSheetHostDetail

FScfgDefaultSheetHost::FScfgDefaultSheetHost(UGameInstance* InGameInstance) : GameInstance(InGameInstance) {}

bool FScfgDefaultSheetHost::CanRestoreFocus() const
{
	// Inside garbage collection, or the game instance is being collected (its BeginDestroy runs in the incremental
	// purge, after the collection proper): the game's widgets may be going too.
	return !IsGarbageCollecting() && GameInstance.IsValid();
}

FScfgDefaultSheetHost::~FScfgDefaultSheetHost()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
	}
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
	// the viewport locks the cursor with its capture unless its lock mode says not to: given back so on close
	Widget->SetMouseLockHint(Client->LockDuringCapture());
	Widget->SetMouseWish(MouseWishOverride ? MouseWishOverride : ScfgDefaultSheetHostDetail::WishOf(Client, GameInstance));
	Client->AddViewportWidgetContent(Widget, ZORDER);
	Shown.Add(FShown{Handle, Widget, Client});
	Widget->OnShown();
	EnsureTicking();
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
	const TSharedRef<FScfgStyle> Style =
		FScfgStyle::Create(ScfgString::FromUtf8(scfg::config::DEFAULT_ACCENT), FString(), StyleOverride);
	// removed by Tick once it has faded, not from inside its own tick (a paint pass)
	const TSharedRef<SScfgToast> Toast = SNew(SScfgToast, Style).Text(Text);
	Client->AddViewportWidgetContent(Toast, ZORDER + 1);
	Toasts.Add(FToast{Toast, Client});
	EnsureTicking();
}

void FScfgDefaultSheetHost::EnsureTicking()
{
	if (TickHandle.IsValid())
	{
		return;
	}
	TWeakPtr<FScfgDefaultSheetHost> Weak = AsShared();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
	{
		const TSharedPtr<FScfgDefaultSheetHost> Host = Weak.Pin();
		if (!Host.IsValid())
		{
			return false;
		}
		Host->Tick();
		return true;
	}));
}

void FScfgDefaultSheetHost::Tick()
{
	// A screen someone else took off the viewport (a "Remove All Widgets" node, a level script) no longer ticks or
	// takes input: it counts as released without Close, so its flow ends as "cancel" at the next pump (the handle
	// goes with it), and the focus and the mouse it holds go back.
	for (int32 I = Shown.Num() - 1; I >= 0; --I)
	{
		if (!Shown[I].Widget->GetParentWidget().IsValid())
		{
			const FShown Gone = Shown[I];
			Shown.RemoveAt(I);
			Gone.Widget->OnHidden(CanRestoreFocus());
		}
	}
	RemoveFinishedToasts();
}

void FScfgDefaultSheetHost::RemoveFinishedToasts()
{
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
