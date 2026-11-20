// Copyright 2026 Repulse
// No Python twin: see ScfgPayAsyncAction.h.
#include "ScfgPayAsyncAction.h"

#include "ScfgBlueprintLibrary.h"
#include "ScfgGuard.h"
#include "ScfgLog.h"
#include "ScfgSubsystem.h"
#include "ScfgSubsystemAccess.h"

#include "Engine/GameInstance.h"

namespace ScfgAsyncDetail
{

const TCHAR* const NoSubsystem = TEXT("scfg: no SCFG subsystem here (the node needs a game instance)");
const TCHAR* const NoSubsystemOnServer =
	TEXT("scfg: no SCFG subsystem on a dedicated server (SCFG runs on the players' game clients only)");

// A subsystem that is shutting down, or gone although the node had one: the teardown (maybe inside garbage
// collection) resolves the call without delivering anything.
bool IsTeardown(bool bHadSubsystem, const TWeakObjectPtr<UScfgSubsystem>& Subsystem)
{
	const UScfgSubsystem* Scfg = Subsystem.Get();
	return bHadSubsystem && (!Scfg || Scfg->IsShuttingDown());
}

}  // namespace ScfgAsyncDetail

UScfgPayAsyncAction* UScfgPayAsyncAction::Pay(UObject* WorldContextObject, EScfgPayKind Kind, FScfgPayRequest Request)
{
	UScfgPayAsyncAction* Action = PayWith(UScfgBlueprintLibrary::GetSubsystem(WorldContextObject), Kind, Request);
	Action->bOnDedicatedServer = !Action->bHadSubsystem && UScfgBlueprintLibrary::OnDedicatedServer(WorldContextObject);
	if (!Action->RegisteredWithGameInstance.IsValid())
	{
		Action->RegisterWithGameInstance(WorldContextObject);
	}
	return Action;
}

UScfgPayAsyncAction* UScfgPayAsyncAction::PayWith(
	UScfgSubsystem* Subsystem, EScfgPayKind Kind, const FScfgPayRequest& Request)
{
	UScfgPayAsyncAction* Action = NewObject<UScfgPayAsyncAction>();
	Action->Subsystem = Subsystem;
	Action->bHadSubsystem = Subsystem != nullptr;
	Action->Kind = Kind;
	Action->Request = Request;
	if (Subsystem && Subsystem->GetGameInstance())
	{
		Action->RegisterWithGameInstance(Subsystem->GetGameInstance());
	}
	return Action;
}

void UScfgPayAsyncAction::Activate()
{
	UScfgSubsystem* Scfg = Subsystem.Get();
	if (!Scfg && bHadSubsystem)
	{
		SetReadyToDestroy();  // its game instance went away before the node ran: nothing to deliver
		return;
	}
	if (!Scfg)
	{
		UE_LOG(LogScfg, Error, TEXT("%s"),
			bOnDedicatedServer ? ScfgAsyncDetail::NoSubsystemOnServer : ScfgAsyncDetail::NoSubsystem);
		Finish(ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token));
		return;
	}
	TWeakObjectPtr<UScfgPayAsyncAction> Weak(this);
	// (the unguarded start: Finish itself drops a teardown result, and releases the node)
	FScfgSubsystemAccess::PayAsyncForNode(*Scfg, Kind, Request,
		[Weak](const FScfgPaymentResult& Result)
		{
			if (UScfgPayAsyncAction* Self = Weak.Get())
			{
				Self->Finish(Result);
			}
		});
}

void UScfgPayAsyncAction::Finish(const FScfgPaymentResult& Result)
{
	if (!ScfgAsyncDetail::IsTeardown(bHadSubsystem, Subsystem))
	{
		OnResolved.Broadcast(Result);
	}
	SetReadyToDestroy();
}

UScfgRestoreAsyncAction* UScfgRestoreAsyncAction::Restore(UObject* WorldContextObject)
{
	UScfgRestoreAsyncAction* Action = RestoreWith(UScfgBlueprintLibrary::GetSubsystem(WorldContextObject));
	Action->bOnDedicatedServer = !Action->bHadSubsystem && UScfgBlueprintLibrary::OnDedicatedServer(WorldContextObject);
	if (!Action->RegisteredWithGameInstance.IsValid())
	{
		Action->RegisterWithGameInstance(WorldContextObject);
	}
	return Action;
}

UScfgRestoreAsyncAction* UScfgRestoreAsyncAction::RestoreWith(UScfgSubsystem* Subsystem)
{
	UScfgRestoreAsyncAction* Action = NewObject<UScfgRestoreAsyncAction>();
	Action->Subsystem = Subsystem;
	Action->bHadSubsystem = Subsystem != nullptr;
	if (Subsystem && Subsystem->GetGameInstance())
	{
		Action->RegisterWithGameInstance(Subsystem->GetGameInstance());
	}
	return Action;
}

void UScfgRestoreAsyncAction::Activate()
{
	UScfgSubsystem* Scfg = Subsystem.Get();
	if (!Scfg && bHadSubsystem)
	{
		SetReadyToDestroy();
		return;
	}
	if (!Scfg)
	{
		UE_LOG(LogScfg, Error, TEXT("%s"),
			bOnDedicatedServer ? ScfgAsyncDetail::NoSubsystemOnServer : ScfgAsyncDetail::NoSubsystem);
		Finish(ScfgGuard::RestoreNotRunning());
		return;
	}
	TWeakObjectPtr<UScfgRestoreAsyncAction> Weak(this);
	FScfgSubsystemAccess::RestoreAsyncForNode(*Scfg,
		[Weak](const FScfgRestoreResult& Result)
		{
			if (UScfgRestoreAsyncAction* Self = Weak.Get())
			{
				Self->Finish(Result);
			}
		});
}

void UScfgRestoreAsyncAction::Finish(const FScfgRestoreResult& Result)
{
	if (!ScfgAsyncDetail::IsTeardown(bHadSubsystem, Subsystem))
	{
		OnResolved.Broadcast(Result);
	}
	SetReadyToDestroy();
}
