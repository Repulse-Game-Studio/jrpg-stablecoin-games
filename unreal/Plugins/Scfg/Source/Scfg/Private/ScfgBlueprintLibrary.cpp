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

// The node kinds that logged "no subsystem" this session (game thread only).
TSet<FString>& Logged()
{
	static TSet<FString> Names;
	return Names;
}

UScfgSubsystem* Find(const UObject* WorldContextObject, const TCHAR* What)
{
	UScfgSubsystem* Scfg = UScfgBlueprintLibrary::GetSubsystem(WorldContextObject);
	if (!Scfg)
	{
		bool bLogged = false;
		Logged().Add(What, &bLogged);
		if (!bLogged && UScfgBlueprintLibrary::OnDedicatedServer(WorldContextObject))
		{
			UE_LOG(LogScfg, Error, TEXT("scfg: %s: no SCFG subsystem on a dedicated server (SCFG runs on the players' "
				"game clients only)"), What);
		}
		else if (!bLogged)
		{
			UE_LOG(LogScfg, Error, TEXT("scfg: %s: no SCFG subsystem here (it needs a game instance)"), What);
		}
	}
	return Scfg;
}

}  // namespace ScfgLibraryDetail

bool UScfgBlueprintLibrary::OnDedicatedServer(const UObject* WorldContextObject)
{
	if (IsRunningDedicatedServer())
	{
		return true;
	}
	const UWorld* World =
		GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return false;
	}
	if (World->GetNetMode() == NM_DedicatedServer)
	{
		return true;
	}
	if (const UGameInstance* GameInstance = World->GetGameInstance(); GameInstance && GameInstance->IsDedicatedServerInstance())
	{
		return true;
	}
	// (a game instance that is shut down has let its world context go: the engine still has it)
	const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	return Context && Context->RunAsDedicated;
}

void UScfgBlueprintLibrary::ResetLogOnceForTests()
{
	ScfgLibraryDetail::Logged().Reset();
}

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
