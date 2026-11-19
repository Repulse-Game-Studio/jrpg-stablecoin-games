// Copyright 2026 Repulse
// No Python twin: the subsystem around FScfgFacade (see ScfgSubsystem.h).
#include "ScfgSubsystem.h"

#include "ScfgConvert.h"
#include "ScfgControllers.h"
#include "ScfgDefaultSheetHost.h"
#include "ScfgFacade.h"
#include "ScfgGuard.h"
#include "ScfgLog.h"
#include "ScfgSettings.h"
#include "ScfgString.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"

namespace ScfgSubsystemDetail
{

// The game's C++ OnResolved, dropped during the teardown, as the async nodes drop theirs (UScfgPayAsyncAction::Finish):
// Deinitialize runs after the world's EndPlay and BeginDestroy inside garbage collection, where game code that makes
// a widget or a UObject would assert. The subsystem is gone (collected) or shutting down then.
template <class TResult>
TFunction<void(const TResult&)> UnlessTeardown(const UScfgSubsystem* Subsystem, TFunction<void(const TResult&)> Fn)
{
	if (!Fn)
	{
		return nullptr;
	}
	TWeakObjectPtr<const UScfgSubsystem> Weak(Subsystem);
	return [Weak, Fn = MoveTemp(Fn)](const TResult& Result)
	{
		const UScfgSubsystem* Self = Weak.Get();
		if (Self && !Self->IsShuttingDown())
		{
			Fn(Result);
		}
	};
}

}  // namespace ScfgSubsystemDetail

bool UScfgSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// No subsystem on a dedicated server (a server process, or a play-in-editor server instance): it can never show a
	// sheet, and its worker would poll the RPC urls and write the one state file of the clients.
	if (IsRunningDedicatedServer())
	{
		return false;
	}
	if (const UGameInstance* GameInstance = Cast<UGameInstance>(Outer))
	{
		if (GameInstance->IsDedicatedServerInstance())
		{
			return false;
		}
		const UWorld* World = GameInstance->GetWorld();
		if (World && World->GetNetMode() == NM_DedicatedServer)
		{
			return false;
		}
	}
	return true;
}

void UScfgSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StartFacade(FScfgFacadeOptions(), GetDefault<UScfgSettings>());
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

void UScfgSubsystem::StartFacade(const FScfgFacadeOptions& Options, const UScfgSettings* Settings)
{
	StopFacade();
	bStopping = false;
	Facade = MakeShared<FScfgFacade>(Options);
	// The plugin's own Slate sheet on this game instance's viewport, unless the game sets a host of its own.
	Facade->DefaultSheetHost = MakeShared<FScfgDefaultSheetHost>(GetGameInstance());
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
	if (!Settings)
	{
		return;
	}
	if (Settings->IsEmpty())
	{
		if (Settings->HasRows())
		{
			// Only if nothing configured SCFG by the end of the first frame: a game that applies the settings with a
			// runtime wallet (ConfigureWithSettings) in its Init() or first BeginPlay gets no warning (the opt-out).
			Facade->WarnUnlessConfiguredAfterAFrame(TEXT("scfg: the SCFG settings (Project Settings > Plugins > SCFG) "
				"have chains or items but no wallet, so they were not applied at start, and nothing configured SCFG in "
				"the first frame: set a wallet there, configure from code, or call ConfigureWithSettings(Wallet, SolanaWallet) with a "
				"wallet supplied at runtime."));
		}
		return;
	}
	(void)Facade->ApplySettings(Settings->Options, Settings->Chains, Settings->Items);
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
	if (ScfgGuard::OffGameThread(TEXT("IsShuttingDown()")))
	{
		return false;
	}
	return bStopping || (Facade.IsValid() && Facade->IsShuttingDown());
}

FScfgFacade* UScfgSubsystem::Live(const TCHAR* What) const
{
	if (ScfgGuard::OffGameThread(What))
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
	return F ? ScfgGuard::ErrCode(F->AddChain(Key, Chain)) : FString(ScfgGuard::NotRunningCode);
}

FString UScfgSubsystem::Configure(const FScfgOptions& Options)
{
	FScfgFacade* F = Live(TEXT("Configure()"));
	return F ? ScfgGuard::ErrCode(F->Configure(Options)) : FString(ScfgGuard::NotRunningCode);
}

FString UScfgSubsystem::AddItem(const FString& ItemId, const FScfgItem& Item)
{
	FScfgFacade* F = Live(TEXT("AddItem()"));
	return F ? ScfgGuard::ErrCode(F->AddItem(ItemId, Item)) : FString(ScfgGuard::NotRunningCode);
}

FString UScfgSubsystem::ConfigureWithSettings(const FString& Wallet, const FString& SolanaWallet)
{
	return ApplySettingsWith(*GetDefault<UScfgSettings>(), Wallet, SolanaWallet);
}

FString UScfgSubsystem::ApplySettingsWith(const UScfgSettings& Settings, const FString& Wallet, const FString& SolanaWallet)
{
	FScfgFacade* F = Live(TEXT("ConfigureWithSettings()"));
	if (!F)
	{
		return FString(ScfgGuard::NotRunningCode);
	}
	FScfgOptions Options = Settings.Options;
	Options.Wallet = Wallet.TrimStartAndEnd();
	Options.SolanaWallet = SolanaWallet.TrimStartAndEnd();
	return ScfgGuard::ErrCode(F->ConfigureWithSettings(Options, Settings.Chains, Settings.Items));
}

void UScfgSubsystem::Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, const FOnScfgPaid& OnPaid)
{
	if (ScfgGuard::OffGameThread(TEXT("Pay()")))
	{
		// (copied, not read: IsBound() reads a weak object pointer, which only the game thread may)
		ScfgGuard::OnGameThread([OnPaid, Failed = ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token)]
			{ OnPaid.ExecuteIfBound(Failed); });
		return;
	}
	FScfgPayCallback Callback;
	if (OnPaid.IsBound())
	{
		Callback.Fn = [OnPaid](const FScfgPaymentResult& Result) { OnPaid.ExecuteIfBound(Result); };
		Callback.IsValid = [OnPaid]() { return OnPaid.IsBound(); };
	}
	FScfgFacade* F = Live(TEXT("Pay()"));
	if (!F)
	{
		if (Callback.Valid())
		{
			Callback.Fn(ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token));
		}
		return;
	}
	F->Pay(Kind, Request, MoveTemp(Callback), nullptr);
}

void UScfgSubsystem::Pay(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> Callback)
{
	if (ScfgGuard::OffGameThread(TEXT("Pay()")))
	{
		if (Callback)
		{
			ScfgGuard::OnGameThread([Callback = MoveTemp(Callback), Failed = ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token)]
				{ Callback(Failed); });
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
			Cb.Fn(ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token));
		}
		return;
	}
	F->Pay(Kind, Request, MoveTemp(Cb), nullptr);
}

void UScfgSubsystem::PayAsync(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> OnResolved)
{
	// (off the game thread StartPayAsync refuses the call: no weak pointer is made there)
	StartPayAsync(Kind, Request, IsInGameThread() ? ScfgSubsystemDetail::UnlessTeardown(this, MoveTemp(OnResolved))
		: MoveTemp(OnResolved));
}

void UScfgSubsystem::StartPayAsync(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> OnResolved)
{
	if (ScfgGuard::OffGameThread(TEXT("PayAsync()")))
	{
		if (OnResolved)
		{
			ScfgGuard::OnGameThread([OnResolved = MoveTemp(OnResolved), Failed = ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token)]
				{ OnResolved(Failed); });
		}
		return;
	}
	FScfgFacade* F = Live(TEXT("PayAsync()"));
	if (!F)
	{
		if (OnResolved)
		{
			OnResolved(ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token));
		}
		return;
	}
	F->Pay(Kind, Request, FScfgPayCallback(), MoveTemp(OnResolved));
}

void UScfgSubsystem::Restore(const FOnScfgRestored& OnRestored)
{
	if (ScfgGuard::OffGameThread(TEXT("Restore()")))
	{
		ScfgGuard::OnGameThread([OnRestored] { OnRestored.ExecuteIfBound(ScfgGuard::RestoreNotRunning()); });
		return;
	}
	FScfgRestoreCallback Callback;
	if (OnRestored.IsBound())
	{
		Callback.Fn = [OnRestored](const FScfgRestoreResult& Result) { OnRestored.ExecuteIfBound(Result); };
		Callback.IsValid = [OnRestored]() { return OnRestored.IsBound(); };
	}
	FScfgFacade* F = Live(TEXT("Restore()"));
	if (!F)
	{
		if (Callback.Valid())
		{
			Callback.Fn(ScfgGuard::RestoreNotRunning());
		}
		return;
	}
	F->Restore(MoveTemp(Callback), nullptr);
}

void UScfgSubsystem::RestoreAsync(
	TFunction<void(const FScfgRestoreResult&)> OnResolved, TFunction<void(const FScfgRestoreResult&)> Callback)
{
	StartRestoreAsync(IsInGameThread() ? ScfgSubsystemDetail::UnlessTeardown(this, MoveTemp(OnResolved))
		: MoveTemp(OnResolved), MoveTemp(Callback));
}

void UScfgSubsystem::StartRestoreAsync(
	TFunction<void(const FScfgRestoreResult&)> OnResolved, TFunction<void(const FScfgRestoreResult&)> Callback)
{
	if (ScfgGuard::OffGameThread(TEXT("RestoreAsync()")))
	{
		if (OnResolved)
		{
			ScfgGuard::OnGameThread([OnResolved = MoveTemp(OnResolved)] { OnResolved(ScfgGuard::RestoreNotRunning()); });
		}
		return;
	}
	FScfgFacade* F = Live(TEXT("RestoreAsync()"));
	if (!F)
	{
		if (OnResolved)
		{
			OnResolved(ScfgGuard::RestoreNotRunning());
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
	// Every platform the plugin builds for; no facade read, so no thread check either.
	return FScfgFacade::Supported();
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
	if (ScfgGuard::OffGameThread(TEXT("GetNotify()")))
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
	if (ScfgGuard::OffGameThread(TEXT("GetSheetHost()")))
	{
		return nullptr;
	}
	return Facade.IsValid() ? Facade->SheetHost : nullptr;
}
