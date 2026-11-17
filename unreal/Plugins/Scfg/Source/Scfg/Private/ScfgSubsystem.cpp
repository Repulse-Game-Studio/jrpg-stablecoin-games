// No Python twin: the subsystem around FScfgFacade (see ScfgSubsystem.h).
#include "ScfgSubsystem.h"

#include "ScfgConvert.h"
#include "ScfgControllers.h"
#include "ScfgFacade.h"
#include "ScfgLog.h"
#include "ScfgString.h"

#include "Containers/Ticker.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"

#include <string>

namespace ScfgSubsystemDetail
{

FString Code(const TOptional<scfg::Err>& Error)
{
	if (!Error.IsSet())
	{
		return FString();
	}
	return ScfgString::FromUtf8(Error->code.empty() ? Error->kind : Error->code);
}

FScfgPaymentResult NotRunning()
{
	FScfgPaymentResult R;
	R.Status = EScfgPaymentStatus::Failed;
	R.Error = TEXT("RuntimeError");
	return R;
}

FScfgRestoreResult RestoreNotRunning()
{
	FScfgRestoreResult R;
	R.Status = EScfgRestoreStatus::Failed;
	R.Error = TEXT("RuntimeError");
	return R;
}

// A call from another thread is misuse: each function logs its own line (an ensure reports only once per process),
// then one ensure with its callstack. Nothing of the subsystem or the facade is touched off the game thread.
bool OffGameThread(const TCHAR* What)
{
	if (IsInGameThread())
	{
		return false;
	}
	UE_LOG(LogScfg, Error, TEXT("scfg: %s must be called on the game thread"), What);
	ensureMsgf(false, TEXT("scfg: %s must be called on the game thread"), What);
	return true;
}

// Fn runs on the game thread at the next tick of the core ticker (FTSTicker::AddTicker may be called from any
// thread): an off-thread Pay or Restore gets its Failed result there, never on the calling thread.
void OnGameThread(TFunction<void()> Fn)
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Fn = MoveTemp(Fn)](float)
	{
		Fn();
		return false;  // once
	}));
}

}  // namespace ScfgSubsystemDetail

void UScfgSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StartFacade(FScfgFacadeOptions());
}

void UScfgSubsystem::Deinitialize()
{
	StopFacade();
	Super::Deinitialize();
}

void UScfgSubsystem::BeginDestroy()
{
	StopFacade();
	Super::BeginDestroy();
}

void UScfgSubsystem::StartFacade(const FScfgFacadeOptions& Options)
{
	StopFacade();
	bStopping = false;
	Facade = MakeShared<FScfgFacade>(Options);
	TWeakObjectPtr<UScfgSubsystem> Weak(this);
	Facade->OnPaymentResolved = [Weak](const FScfgPaymentResult& Result)
	{
		if (UScfgSubsystem* Self = Weak.Get())
		{
			Self->OnPaymentResolved.Broadcast(Result);
		}
	};
	Facade->OnReceiptsChanged = [Weak]()
	{
		if (UScfgSubsystem* Self = Weak.Get())
		{
			Self->OnReceiptsChanged.Broadcast();
		}
	};
	Facade->Start();
}

void UScfgSubsystem::StopFacade()
{
	if (!Facade.IsValid())
	{
		return;
	}
	bStopping = true;
	// Keep it alive through its own teardown, then drop it (a host may still hold a controller: its buttons then do
	// nothing).
	const TSharedPtr<FScfgFacade> Going = Facade;
	Going->Shutdown();
	Facade.Reset();
}

bool UScfgSubsystem::IsShuttingDown() const
{
	return bStopping || (Facade.IsValid() && Facade->IsShuttingDown());
}

FScfgFacade* UScfgSubsystem::Live(const TCHAR* What) const
{
	if (ScfgSubsystemDetail::OffGameThread(What))
	{
		return nullptr;
	}
	if (Facade.IsValid() && !Facade->IsShuttingDown())
	{
		return Facade.Get();
	}
	bool bLogged = false;
	LoggedOutside.Add(What, &bLogged);
	if (!bLogged)
	{
		UE_LOG(LogScfg, Error, TEXT("scfg: %s: the SCFG subsystem is not running (outside its game instance)"), What);
	}
	return nullptr;
}

FString UScfgSubsystem::AddChain(const FString& Key, const FScfgChain& Chain)
{
	FScfgFacade* F = Live(TEXT("AddChain()"));
	return F ? ScfgSubsystemDetail::Code(F->AddChain(Key, Chain)) : FString(TEXT("RuntimeError"));
}

FString UScfgSubsystem::Configure(const FScfgOptions& Options)
{
	FScfgFacade* F = Live(TEXT("Configure()"));
	return F ? ScfgSubsystemDetail::Code(F->Configure(Options)) : FString(TEXT("RuntimeError"));
}

FString UScfgSubsystem::AddItem(const FString& ItemId, const FScfgItem& Item)
{
	FScfgFacade* F = Live(TEXT("AddItem()"));
	return F ? ScfgSubsystemDetail::Code(F->AddItem(ItemId, Item)) : FString(TEXT("RuntimeError"));
}

void UScfgSubsystem::Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, const FOnScfgPaid& OnPaid)
{
	FScfgPayCallback Callback;
	if (OnPaid.IsBound())
	{
		Callback.Fn = [OnPaid](const FScfgPaymentResult& Result) { OnPaid.ExecuteIfBound(Result); };
		Callback.IsValid = [OnPaid]() { return OnPaid.IsBound(); };
	}
	if (ScfgSubsystemDetail::OffGameThread(TEXT("Pay()")))
	{
		ScfgSubsystemDetail::OnGameThread([OnPaid] { OnPaid.ExecuteIfBound(ScfgSubsystemDetail::NotRunning()); });
		return;
	}
	FScfgFacade* F = Live(TEXT("Pay()"));
	if (!F)
	{
		if (Callback.Valid())
		{
			Callback.Fn(ScfgSubsystemDetail::NotRunning());
		}
		return;
	}
	F->Pay(Kind, Request, MoveTemp(Callback), nullptr);
}

void UScfgSubsystem::Pay(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> Callback)
{
	if (ScfgSubsystemDetail::OffGameThread(TEXT("Pay()")))
	{
		if (Callback)
		{
			ScfgSubsystemDetail::OnGameThread([Callback = MoveTemp(Callback)] { Callback(ScfgSubsystemDetail::NotRunning()); });
		}
		return;
	}
	FScfgPayCallback Cb;
	Cb.Fn = MoveTemp(Callback);
	FScfgFacade* F = Live(TEXT("Pay()"));
	if (!F)
	{
		if (Cb.Valid())
		{
			Cb.Fn(ScfgSubsystemDetail::NotRunning());
		}
		return;
	}
	F->Pay(Kind, Request, MoveTemp(Cb), nullptr);
}

void UScfgSubsystem::PayAsync(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> OnResolved)
{
	if (ScfgSubsystemDetail::OffGameThread(TEXT("PayAsync()")))
	{
		if (OnResolved)
		{
			ScfgSubsystemDetail::OnGameThread([OnResolved = MoveTemp(OnResolved)] { OnResolved(ScfgSubsystemDetail::NotRunning()); });
		}
		return;
	}
	FScfgFacade* F = Live(TEXT("PayAsync()"));
	if (!F)
	{
		if (OnResolved)
		{
			OnResolved(ScfgSubsystemDetail::NotRunning());
		}
		return;
	}
	F->Pay(Kind, Request, FScfgPayCallback(), MoveTemp(OnResolved));
}

void UScfgSubsystem::Restore(const FOnScfgRestored& OnRestored)
{
	FScfgRestoreCallback Callback;
	if (OnRestored.IsBound())
	{
		Callback.Fn = [OnRestored](const FScfgRestoreResult& Result) { OnRestored.ExecuteIfBound(Result); };
		Callback.IsValid = [OnRestored]() { return OnRestored.IsBound(); };
	}
	if (ScfgSubsystemDetail::OffGameThread(TEXT("Restore()")))
	{
		ScfgSubsystemDetail::OnGameThread([OnRestored] { OnRestored.ExecuteIfBound(ScfgSubsystemDetail::RestoreNotRunning()); });
		return;
	}
	FScfgFacade* F = Live(TEXT("Restore()"));
	if (!F)
	{
		if (Callback.Valid())
		{
			Callback.Fn(ScfgSubsystemDetail::RestoreNotRunning());
		}
		return;
	}
	F->Restore(MoveTemp(Callback), nullptr);
}

void UScfgSubsystem::RestoreAsync(
	TFunction<void(const FScfgRestoreResult&)> OnResolved, TFunction<void(const FScfgRestoreResult&)> Callback)
{
	if (ScfgSubsystemDetail::OffGameThread(TEXT("RestoreAsync()")))
	{
		if (OnResolved)
		{
			ScfgSubsystemDetail::OnGameThread([OnResolved = MoveTemp(OnResolved)] { OnResolved(ScfgSubsystemDetail::RestoreNotRunning()); });
		}
		return;
	}
	FScfgFacade* F = Live(TEXT("RestoreAsync()"));
	if (!F)
	{
		if (OnResolved)
		{
			OnResolved(ScfgSubsystemDetail::RestoreNotRunning());
		}
		return;
	}
	FScfgRestoreCallback Cb;
	Cb.Fn = MoveTemp(Callback);
	F->Restore(MoveTemp(Cb), MoveTemp(OnResolved));
}

bool UScfgSubsystem::Owns(const FString& ItemId)
{
	FScfgFacade* F = Live(TEXT("Owns()"));
	return F && F->Owns(ItemId);
}

bool UScfgSubsystem::HasTipped()
{
	FScfgFacade* F = Live(TEXT("HasTipped()"));
	return F && F->HasTipped();
}

FString UScfgSubsystem::TipTotal()
{
	FScfgFacade* F = Live(TEXT("TipTotal()"));
	return F ? F->TipTotal() : FString(TEXT("0"));
}

TMap<FString, FScfgReceipt> UScfgSubsystem::Receipts()
{
	TMap<FString, FScfgReceipt> Out;
	if (FScfgFacade* F = Live(TEXT("Receipts()")))
	{
		const scfg::Json All = F->Receipts();
		for (const auto& [Key, Record] : All.as_dict())
		{
			Out.Add(ScfgString::FromUtf8(Key), ScfgConvert::Receipt(Record));
		}
	}
	return Out;
}

TMap<FString, FScfgPendingPayment> UScfgSubsystem::Pending()
{
	TMap<FString, FScfgPendingPayment> Out;
	if (FScfgFacade* F = Live(TEXT("Pending()")))
	{
		const scfg::Json All = F->Pending();
		for (const auto& [Sid, Record] : All.as_dict())
		{
			Out.Add(ScfgString::FromUtf8(Sid), ScfgConvert::PendingPayment(Record));
		}
	}
	return Out;
}

bool UScfgSubsystem::Supported() const
{
	return !Facade.IsValid() || Facade->Supported();
}

FString UScfgSubsystem::ExportReceipts(const FString& Path)
{
	FScfgFacade* F = Live(TEXT("ExportReceipts()"));
	if (!F)
	{
		return FString();
	}
	scfg::Result<FString> Written = F->ExportReceipts(Path);
	return Written.ok() ? Written.value() : FString();
}

void UScfgSubsystem::ClearLocalData()
{
	if (FScfgFacade* F = Live(TEXT("ClearLocalData()")))
	{
		F->ClearLocalData();
	}
}

void UScfgSubsystem::SetNotify(bool bInNotify)
{
	if (FScfgFacade* F = Live(TEXT("SetNotify()")))
	{
		F->bNotify = bInNotify;
	}
}

bool UScfgSubsystem::GetNotify() const
{
	if (ScfgSubsystemDetail::OffGameThread(TEXT("GetNotify()")))
	{
		return false;
	}
	return Facade.IsValid() && Facade->bNotify;
}

void UScfgSubsystem::SetSheetHost(TSharedPtr<IScfgSheetHost> Host)
{
	if (FScfgFacade* F = Live(TEXT("SetSheetHost()")))
	{
		F->SheetHost = MoveTemp(Host);
	}
}

TSharedPtr<IScfgSheetHost> UScfgSubsystem::GetSheetHost() const
{
	if (ScfgSubsystemDetail::OffGameThread(TEXT("GetSheetHost()")))
	{
		return nullptr;
	}
	return Facade.IsValid() ? Facade->SheetHost : nullptr;
}
