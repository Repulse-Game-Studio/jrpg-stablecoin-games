// Copyright 2026 Repulse
// No Python twin: the plugin's own access to FScfgDefaultSheetHost's private parts (its friend), for the plugin's
// tests and the development project's end-to-end driver, never a game's: the mouse hook, which takes the private
// FScfgMouseWish (UI/SScfgScreen.h), and, outside Shipping, what the host shows.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ScfgDefaultSheetHost.h"
#include "UI/SScfgScreen.h"
#include "UI/SScfgToast.h"

class UGameInstance;
class UGameViewportClient;

namespace ScfgDefaultSheetHostDetail
{
// What Client's game viewport wants of the mouse now, read when it is called (ScfgDefaultSheetHost.cpp).
SCFG_API TFunction<FScfgMouseWish()> WishOf(UGameViewportClient* Client, TWeakObjectPtr<UGameInstance> GameInstance);
}  // namespace ScfgDefaultSheetHostDetail

struct FScfgDefaultSheetHostAccess
{
	// What the host's screens take as the game viewport's wish for the mouse (default: WishOf the game viewport
	// client).
	static void SetMouseWishOverride(FScfgDefaultSheetHost& Host, TFunction<FScfgMouseWish()> Wish)
	{
		Host.MouseWishOverride = MoveTemp(Wish);
	}

#if !UE_BUILD_SHIPPING
	// The screens shown, oldest first, as their widgets (SScfgPaymentSheet / SScfgRestoreScreen).
	static TArray<TSharedRef<SScfgScreen>> Screens(const FScfgDefaultSheetHost& Host)
	{
		TArray<TSharedRef<SScfgScreen>> Out;
		for (const FScfgDefaultSheetHost::FShown& Each : Host.Shown)
		{
			Out.Add(Each.Widget);
		}
		return Out;
	}
	// The toasts shown, and their texts, oldest first.
	static int32 NumToasts(const FScfgDefaultSheetHost& Host) { return Host.Toasts.Num(); }
	static TArray<FString> ToastTexts(const FScfgDefaultSheetHost& Host)
	{
		TArray<FString> Out;
		for (const FScfgDefaultSheetHost::FToast& Each : Host.Toasts)
		{
			Out.Add(Each.Widget->GetText());
		}
		return Out;
	}
#endif
};
