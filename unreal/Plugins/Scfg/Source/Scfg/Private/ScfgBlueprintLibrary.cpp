// No Python twin: see ScfgBlueprintLibrary.h.
#include "ScfgBlueprintLibrary.h"

#include "ScfgLog.h"
#include "ScfgSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

UScfgSubsystem* UScfgBlueprintLibrary::GetSubsystem(const UObject* WorldContextObject)
{
	const UWorld* World =
		GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UScfgSubsystem>() : nullptr;
}

namespace ScfgLibraryDetail
{

UScfgSubsystem* Find(const UObject* WorldContextObject, const TCHAR* What)
{
	UScfgSubsystem* Scfg = UScfgBlueprintLibrary::GetSubsystem(WorldContextObject);
	if (!Scfg)
	{
		UE_LOG(LogScfg, Error, TEXT("scfg: %s: no SCFG subsystem here (it needs a game instance)"), What);
	}
	return Scfg;
}

}  // namespace ScfgLibraryDetail

bool UScfgBlueprintLibrary::Owns(const UObject* WorldContextObject, const FString& ItemId)
{
	UScfgSubsystem* Scfg = ScfgLibraryDetail::Find(WorldContextObject, TEXT("Owns"));
	return Scfg && Scfg->Owns(ItemId);
}

bool UScfgBlueprintLibrary::HasTipped(const UObject* WorldContextObject)
{
	UScfgSubsystem* Scfg = ScfgLibraryDetail::Find(WorldContextObject, TEXT("HasTipped"));
	return Scfg && Scfg->HasTipped();
}

FString UScfgBlueprintLibrary::TipTotal(const UObject* WorldContextObject)
{
	UScfgSubsystem* Scfg = ScfgLibraryDetail::Find(WorldContextObject, TEXT("TipTotal"));
	return Scfg ? Scfg->TipTotal() : FString(TEXT("0"));
}

TMap<FString, FScfgReceipt> UScfgBlueprintLibrary::Receipts(const UObject* WorldContextObject)
{
	UScfgSubsystem* Scfg = ScfgLibraryDetail::Find(WorldContextObject, TEXT("Receipts"));
	return Scfg ? Scfg->Receipts() : TMap<FString, FScfgReceipt>();
}

TMap<FString, FScfgPendingPayment> UScfgBlueprintLibrary::Pending(const UObject* WorldContextObject)
{
	UScfgSubsystem* Scfg = ScfgLibraryDetail::Find(WorldContextObject, TEXT("Pending"));
	return Scfg ? Scfg->Pending() : TMap<FString, FScfgPendingPayment>();
}

bool UScfgBlueprintLibrary::Supported()
{
	return true;
}
