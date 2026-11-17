// No Python twin: see ScfgPayAsyncAction.h.
#include "ScfgPayAsyncAction.h"

#include "ScfgBlueprintLibrary.h"
#include "ScfgLog.h"
#include "ScfgSubsystem.h"

#include "Engine/GameInstance.h"

namespace ScfgAsyncDetail
{

const TCHAR* const NoSubsystem = TEXT("scfg: no SCFG subsystem here (the node needs a game instance)");

}  // namespace ScfgAsyncDetail

UScfgPayAsyncAction* UScfgPayAsyncAction::Pay(UObject* WorldContextObject, EScfgPayKind Kind, FScfgPayRequest Request)
{
	UScfgPayAsyncAction* Action = PayWith(UScfgBlueprintLibrary::GetSubsystem(WorldContextObject), Kind, Request);
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
	if (!Scfg)
	{
		UE_LOG(LogScfg, Error, TEXT("%s"), ScfgAsyncDetail::NoSubsystem);
		FScfgPaymentResult Failed;
		Failed.Status = EScfgPaymentStatus::Failed;
		Failed.Kind = Kind;
		Failed.Item = Request.Item;
		Failed.Token = Request.Token;
		Failed.Error = TEXT("RuntimeError");
		Finish(Failed);
		return;
	}
	TWeakObjectPtr<UScfgPayAsyncAction> Weak(this);
	Scfg->PayAsync(Kind, Request,
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
	const UScfgSubsystem* Scfg = Subsystem.Get();
	if (!(Scfg && Scfg->IsShuttingDown()))
	{
		OnResolved.Broadcast(Result);
	}
	SetReadyToDestroy();
}

UScfgRestoreAsyncAction* UScfgRestoreAsyncAction::Restore(UObject* WorldContextObject)
{
	UScfgRestoreAsyncAction* Action = RestoreWith(UScfgBlueprintLibrary::GetSubsystem(WorldContextObject));
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
	if (Subsystem && Subsystem->GetGameInstance())
	{
		Action->RegisterWithGameInstance(Subsystem->GetGameInstance());
	}
	return Action;
}

void UScfgRestoreAsyncAction::Activate()
{
	UScfgSubsystem* Scfg = Subsystem.Get();
	if (!Scfg)
	{
		UE_LOG(LogScfg, Error, TEXT("%s"), ScfgAsyncDetail::NoSubsystem);
		FScfgRestoreResult Failed;
		Failed.Status = EScfgRestoreStatus::Failed;
		Failed.Error = TEXT("RuntimeError");
		Finish(Failed);
		return;
	}
	TWeakObjectPtr<UScfgRestoreAsyncAction> Weak(this);
	Scfg->RestoreAsync(
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
	const UScfgSubsystem* Scfg = Subsystem.Get();
	if (!(Scfg && Scfg->IsShuttingDown()))
	{
		OnResolved.Broadcast(Result);
	}
	SetReadyToDestroy();
}
