// No Python twin: the plugin's own access to FScfgDefaultSheetHost's mouse hooks (its friend), which take the private
// FScfgMouseWish (UI/SScfgScreen.h), for the plugin's tests and the development project's e2e driver.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ScfgDefaultSheetHost.h"
#include "UI/SScfgScreen.h"

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
};
