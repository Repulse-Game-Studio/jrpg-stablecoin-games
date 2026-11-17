// No Python twin: the bridge between the API's value types (ScfgTypes.h) and the core's Json keyword arguments and
// result models: Godot passes its option dictionaries to the core as they are (ScfgConfig.build_config(o)), and
// payment_result.gd / restore_result.gd's from_core() are the other direction.
//
// An empty text field becomes None where Python has a default (wallet, logo, accent, min_price, a pay() item ...), an
// empty list or map is left out (the default), every other field is passed as it is, so the core validates it and its
// ConfigError codes come back unchanged.
#pragma once

#include "CoreMinimal.h"

#include "ScfgTypes.h"

#include "Scfg/Json.h"
#include "Scfg/Models.h"

#include <string>

namespace ScfgConvert
{

// build_config's keyword arguments (without custom_chains).
SCFG_API scfg::Json ConfigKwargs(const FScfgOptions& Options);
// build_chain's keyword arguments, key included.
SCFG_API scfg::Json ChainKwargs(const FString& Key, const FScfgChain& Chain);
// add_item's keyword arguments.
SCFG_API scfg::Json ItemKwargs(const FScfgItem& Item);
// prepare_payment's keyword arguments (item, amount, token).
SCFG_API scfg::Json PayKwargs(const FScfgPayRequest& Request);
// "unlock" / "tip"; any other value of the enum is its int, which prepare_payment refuses (payment.bad_kind).
SCFG_API scfg::Json KindJson(EScfgPayKind Kind);

// The str, else "" (None, or any other type).
SCFG_API FString Str(const scfg::Json& Value);
// The int, else 0.
SCFG_API int64 Int(const scfg::Json& Value);

SCFG_API EScfgPaymentStatus PaymentStatus(const std::string& Status);
SCFG_API EScfgRestoreStatus RestoreStatus(const std::string& Status);
SCFG_API FScfgPaymentResult PaymentResult(const scfg::models::PaymentResult& Core);
SCFG_API FScfgRestoreResult RestoreResult(const scfg::models::RestoreResult& Core);
// A stored receipt (a dict; any other value gives an empty receipt).
SCFG_API FScfgReceipt Receipt(const scfg::Json& Record);
// A pending session record.
SCFG_API FScfgPendingPayment PendingPayment(const scfg::Json& Record);

}  // namespace ScfgConvert
