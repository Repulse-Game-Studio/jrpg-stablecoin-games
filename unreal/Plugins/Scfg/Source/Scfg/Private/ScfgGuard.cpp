// No Python twin: see ScfgGuard.h.
#include "ScfgGuard.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "Containers/Ticker.h"

#include <atomic>

namespace ScfgGuard
{

namespace
{
std::atomic<int32> OffThreadCalls{0};
}  // namespace

bool OffGameThread(const TCHAR* What)
{
	if (IsInGameThread())
	{
		return false;
	}
	OffThreadCalls.fetch_add(1);
	UE_LOG(LogScfg, Error, TEXT("scfg: %s must be called on the game thread"), What);
	ensureMsgf(false, TEXT("scfg: %s must be called on the game thread"), What);
	return true;
}

int32 NumOffThreadCalls()
{
	return OffThreadCalls.load();
}

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

FScfgPaymentResult PaymentNotRunning(EScfgPayKind Kind, const FString& Item, const FString& Token)
{
	FScfgPaymentResult R;
	R.Status = EScfgPaymentStatus::Failed;
	R.Kind = Kind;
	R.Item = Item;
	R.Token = Token;
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

}  // namespace ScfgGuard
