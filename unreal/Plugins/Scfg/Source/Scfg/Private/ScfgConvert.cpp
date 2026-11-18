// Copyright 2026 Repulse
// No Python twin: see ScfgConvert.h.
#include "ScfgConvert.h"

#include "ScfgString.h"

#include <utility>

namespace ScfgConvertDetail
{

scfg::Json Text(const FString& S)
{
	return scfg::Json(ScfgString::ToUtf8(S));
}

// None for an empty string.
scfg::Json TextOrNone(const FString& S)
{
	return S.IsEmpty() ? scfg::Json() : Text(S);
}

scfg::Json List(const TArray<FString>& Values)
{
	scfg::Json::List Out;
	Out.reserve(Values.Num());
	for (const FString& Each : Values)
	{
		Out.push_back(Text(Each));
	}
	return scfg::Json(std::move(Out));
}

}  // namespace ScfgConvertDetail

scfg::Json ScfgConvert::ConfigKwargs(const FScfgOptions& Options)
{
	using namespace ScfgConvertDetail;
	scfg::Json Out = scfg::Json::dict();
	scfg::Json::Dict& D = Out.as_dict();
	if (!Options.Wallet.IsEmpty())
	{
		D.set("wallet", Text(Options.Wallet));
	}
	if (!Options.SolanaWallet.IsEmpty())
	{
		D.set("solana_wallet", Text(Options.SolanaWallet));
	}
	D.set("testnet", scfg::Json(Options.bTestnet));
	if (Options.Chains.Num() > 0)
	{
		D.set("chains", List(Options.Chains));
	}
	if (Options.Tokens.Num() > 0)
	{
		D.set("tokens", List(Options.Tokens));
	}
	const FScfgMethods& M = Options.Methods;
	D.set("methods",
		scfg::Json::dict({
			{"browser", scfg::Json::dict({{"enabled", scfg::Json(M.Browser.bEnabled)},
							{"timeout_min", scfg::Json(static_cast<int64_t>(M.Browser.TimeoutMin))}})},
			{"qr", scfg::Json::dict({{"enabled", scfg::Json(M.Qr.bEnabled)},
					   {"wallet_button", scfg::Json(M.Qr.bWalletButton)},
					   {"chains", M.Qr.Chains.Num() > 0 ? List(M.Qr.Chains) : scfg::Json()}})},
			{"manual", scfg::Json::dict({{"enabled", scfg::Json(M.Manual.bEnabled)},
						   {"allow_tx_paste", scfg::Json(M.Manual.bAllowTxPaste)}})},
		}));
	if (Options.RpcUrls.Num() > 0)
	{
		scfg::Json Urls = scfg::Json::dict();
		for (const TPair<FString, FScfgRpcUrls>& Each : Options.RpcUrls)
		{
			Urls.as_dict().set(ScfgString::ToUtf8(Each.Key), List(Each.Value.Urls));
		}
		D.set("rpc_urls", Urls);
	}
	if (Options.Confirmations.Num() > 0)
	{
		scfg::Json Confirmations = scfg::Json::dict();
		for (const TPair<FString, int32>& Each : Options.Confirmations)
		{
			Confirmations.as_dict().set(ScfgString::ToUtf8(Each.Key), scfg::Json(static_cast<int64_t>(Each.Value)));
		}
		D.set("confirmations", Confirmations);
	}
	if (Options.PreviousWallets.Num() > 0)
	{
		D.set("previous_wallets", List(Options.PreviousWallets));
	}
	if (!Options.Logo.IsEmpty())
	{
		D.set("logo", Text(Options.Logo));
	}
	if (!Options.Accent.IsEmpty())
	{
		D.set("accent", Text(Options.Accent));
	}
	return Out;
}

scfg::Json ScfgConvert::ChainKwargs(const FString& Key, const FScfgChain& Chain)
{
	using namespace ScfgConvertDetail;
	scfg::Json Tokens = scfg::Json::dict();
	for (const TPair<FString, FString>& Each : Chain.Tokens)
	{
		Tokens.as_dict().set(ScfgString::ToUtf8(Each.Key), Text(Each.Value));
	}
	return scfg::Json::dict({
		{"key", Text(Key)},
		{"name", Text(Chain.Name)},
		{"chain_id", scfg::Json(static_cast<int64_t>(Chain.ChainId))},
		{"rpc", List(Chain.Rpc)},
		{"tokens", Tokens},
		{"confirmations", scfg::Json(static_cast<int64_t>(Chain.Confirmations))},
		{"explorer_tx", Text(Chain.ExplorerTx)},
		{"native_symbol", Text(Chain.NativeSymbol)},
		{"fee_hint", Text(Chain.FeeHint)},
		{"max_log_range", scfg::Json(static_cast<int64_t>(Chain.MaxLogRange))},
		{"testnet", scfg::Json(Chain.bTestnet)},
	});
}

scfg::Json ScfgConvert::ItemKwargs(const FScfgItem& Item)
{
	using namespace ScfgConvertDetail;
	return scfg::Json::dict({
		{"code", scfg::Json(static_cast<int64_t>(Item.Code))},
		{"price", Text(Item.Price)},
		{"token", Text(Item.Token)},
		{"title", Text(Item.Title)},
		{"min_price", TextOrNone(Item.MinPrice)},
	});
}

scfg::Json ScfgConvert::PayKwargs(const FScfgPayRequest& Request)
{
	using namespace ScfgConvertDetail;
	return scfg::Json::dict({
		{"item", TextOrNone(Request.Item)},
		{"amount", TextOrNone(Request.Amount)},
		{"token", TextOrNone(Request.Token)},
	});
}

scfg::Json ScfgConvert::KindJson(EScfgPayKind Kind)
{
	switch (Kind)
	{
	case EScfgPayKind::Unlock:
		return scfg::Json("unlock");
	case EScfgPayKind::Tip:
		return scfg::Json("tip");
	default:
		return scfg::Json(static_cast<int64_t>(Kind));
	}
}

FString ScfgConvert::Str(const scfg::Json& Value)
{
	return Value.is_str() ? ScfgString::FromUtf8(Value.as_str()) : FString();
}

int64 ScfgConvert::Int(const scfg::Json& Value)
{
	return Value.is_int64() ? Value.as_int64() : 0;
}

EScfgPaymentStatus ScfgConvert::PaymentStatus(const std::string& Status)
{
	if (Status == "paid")
	{
		return EScfgPaymentStatus::Paid;
	}
	if (Status == "already_owned")
	{
		return EScfgPaymentStatus::AlreadyOwned;
	}
	if (Status == "cancelled")
	{
		return EScfgPaymentStatus::Cancelled;
	}
	if (Status == "expired")
	{
		return EScfgPaymentStatus::Expired;
	}
	if (Status == "underpaid")
	{
		return EScfgPaymentStatus::Underpaid;
	}
	if (Status == "pending")
	{
		return EScfgPaymentStatus::Pending;
	}
	return EScfgPaymentStatus::Failed;
}

EScfgRestoreStatus ScfgConvert::RestoreStatus(const std::string& Status)
{
	if (Status == "restored")
	{
		return EScfgRestoreStatus::Restored;
	}
	if (Status == "nothing_found")
	{
		return EScfgRestoreStatus::NothingFound;
	}
	if (Status == "cancelled")
	{
		return EScfgRestoreStatus::Cancelled;
	}
	return EScfgRestoreStatus::Failed;
}

FScfgPaymentResult ScfgConvert::PaymentResult(const scfg::models::PaymentResult& Core)
{
	FScfgPaymentResult Out;
	Out.Status = PaymentStatus(Core.status);
	Out.Kind = Core.kind.is_str() && Core.kind.as_str() == "tip" ? EScfgPayKind::Tip : EScfgPayKind::Unlock;
	Out.Item = Str(Core.item);
	Out.Amount = Core.amount ? ScfgString::FromUtf8(Core.amount->str()) : FString();
	Out.Token = Str(Core.token);
	Out.Chain = Str(Core.chain);
	Out.TxHash = Str(Core.tx_hash);
	Out.Payer = Str(Core.payer);
	Out.SessionId = Str(Core.session_id);
	Out.Error = Core.error ? ScfgString::FromUtf8(*Core.error) : FString();
	return Out;
}

FScfgRestoreResult ScfgConvert::RestoreResult(const scfg::models::RestoreResult& Core)
{
	FScfgRestoreResult Out;
	Out.Status = RestoreStatus(Core.status);
	for (const scfg::Json& Each : Core.restored)
	{
		Out.Restored.Add(Receipt(Each));
	}
	Out.Error = Core.error ? ScfgString::FromUtf8(*Core.error) : FString();
	return Out;
}

FScfgReceipt ScfgConvert::Receipt(const scfg::Json& Record)
{
	FScfgReceipt Out;
	if (!Record.is_dict())
	{
		return Out;
	}
	const scfg::Json::Dict& R = Record.as_dict();
	Out.Chain = Str(R.get("chain"));
	Out.TxHash = Str(R.get("tx_hash"));
	Out.LogIndex = Int(R.get("log_index"));
	Out.Block = Int(R.get("block"));
	Out.Kind = Str(R.get("kind"));
	Out.Item = Str(R.get("item"));
	Out.Amount = Str(R.get("amount"));
	Out.Token = Str(R.get("token"));
	Out.Payer = Str(R.get("payer"));
	Out.Recipient = Str(R.get("recipient"));
	Out.Source = Str(R.get("source"));
	return Out;
}

FScfgPendingPayment ScfgConvert::PendingPayment(const scfg::Json& Record)
{
	FScfgPendingPayment Out;
	if (!Record.is_dict())
	{
		return Out;
	}
	const scfg::Json::Dict& R = Record.as_dict();
	Out.Id = Str(R.get("id"));
	Out.Kind = Str(R.get("kind"));
	Out.Item = Str(R.get("item"));
	Out.Title = Str(R.get("title"));
	Out.Token = Str(R.get("token"));
	Out.Chain = Str(R.get("chain"));
	Out.Price = Str(R.get("price"));
	Out.State = Str(R.get("state"));
	Out.UiStatus = Str(R.get("ui_status"));
	Out.HintTx = Str(R.get("hint_tx"));
	const scfg::Json CreatedAt = R.get("created_at");
	Out.CreatedAt = CreatedAt.is_int64() ? static_cast<double>(CreatedAt.as_int64())
		: CreatedAt.is_float()           ? CreatedAt.as_float()
										 : 0.0;
	Out.Cursor = Int(R.get("cursor"));
	return Out;
}
