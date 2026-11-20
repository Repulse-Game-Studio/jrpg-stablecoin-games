// Copyright 2026 Repulse
// No Python twin: see ScfgGuard.h.
#include "ScfgGuard.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "Containers/Ticker.h"

#include <atomic>

namespace ScfgGuard
{

#if !UE_BUILD_SHIPPING
namespace
{
std::atomic<int32> OffThreadCalls{0};
}  // namespace
#endif

bool OffGameThread(const TCHAR* What)
{
	if (IsInGameThread())
	{
		return false;
	}
#if !UE_BUILD_SHIPPING
	OffThreadCalls.fetch_add(1);
#endif
	UE_LOG(LogScfg, Error, TEXT("scfg: %s must be called on the game thread"), What);
	ensureMsgf(false, TEXT("scfg: %s must be called on the game thread"), What);
	return true;
}

#if !UE_BUILD_SHIPPING
int32 NumOffThreadCalls()
{
	return OffThreadCalls.load();
}
#endif

void OnGameThread(TFunction<void()> Fn)
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Fn = MoveTemp(Fn)](float)
	{
		Fn();
		return false;  // once
	}));
}

FString ErrCode(const scfg::Err& Error)
{
	return ScfgString::FromUtf8(Error.code.empty() ? Error.kind : Error.code);
}

FString ErrCode(const TOptional<scfg::Err>& Error)
{
	return Error.IsSet() ? ErrCode(*Error) : FString();
}

scfg::Err EngineSpelling(const scfg::Err& Error)
{
	scfg::Err Out = Error;
	if (Error.code == "wallet.missing")
	{
		Out.message = "Configure() needs Options.Wallet (an EVM 0x address) and/or Options.SolanaWallet (in the "
					  "project settings: Plugins > SCFG > Wallet / Solana Wallet)";
	}
	else if (Error.code == "chain.needs_evm_wallet")
	{
		const std::string Python = "needs wallet= (an EVM 0x address)";
		if (const size_t At = Out.message.find(Python); At != std::string::npos)
		{
			Out.message.replace(At, Python.size(), "needs Options.Wallet (an EVM 0x address)");
		}
	}
	return Out;
}

FScfgPaymentResult PaymentNotRunning(EScfgPayKind Kind, const FString& Item, const FString& Token)
{
	FScfgPaymentResult R;
	R.Status = EScfgPaymentStatus::Failed;
	R.Kind = Kind;
	R.Item = Item;
	R.Token = Token;
	R.Error = NotRunningCode;
	return R;
}

FScfgRestoreResult RestoreNotRunning()
{
	FScfgRestoreResult R;
	R.Status = EScfgRestoreStatus::Failed;
	R.Error = NotRunningCode;
	return R;
}

}  // namespace ScfgGuard
