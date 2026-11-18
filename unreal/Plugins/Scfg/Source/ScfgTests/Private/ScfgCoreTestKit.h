// Copyright 2026 Repulse
// No Python twin: what the store, worker and facade tests share about the core: the JSON comparison of the
// conformance rule, a payout wallet, a fixed clock, and the base testnet configuration with one item.
#pragma once

#include "CoreMinimal.h"

#include "ScfgString.h"

#include "Scfg/Config.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <string>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgCoreTestKit
{

// The payout wallet of the store and worker tests, and their clock (Unix seconds).
inline constexpr const char* kWallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
inline constexpr int64_t kNow = 1700000100;

// Two JSON values print the same with sorted keys (the conformance rule: key order ignored, true is not 1).
inline bool Same(const scfg::Json& A, const scfg::Json& B)
{
	scfg::json::DumpOptions Sorted;
	Sorted.sort_keys = true;
	scfg::Result<std::string> X = scfg::json::dumps(A, Sorted);
	scfg::Result<std::string> Y = scfg::json::dumps(B, Sorted);
	return X.ok() && Y.ok() && X.value() == Y.value();
}

// V as json.dumps prints it (for a failure message).
inline FString Text(const scfg::Json& V)
{
	scfg::Result<std::string> Got = scfg::json::dumps(V);
	return Got.ok() ? ScfgString::FromUtf8(Got.value()) : FString(TEXT("<dumps failed>"));
}

// A testnet configuration paying kWallet on base, with chapter2 (code 37, 4.99 USDC).
inline scfg::config::Config BaseConfig()
{
	scfg::Result<scfg::config::Config> Built = scfg::config::build_config(
		scfg::Json::dict({{"wallet", kWallet}, {"chains", scfg::Json::list({"base"})}, {"testnet", true}}));
	scfg::config::Config Out = Built.take();
	scfg::Result<scfg::models::Item> Item = scfg::config::add_item(Out, "chapter2",
		scfg::Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
	(void)Item.take();
	return Out;
}

}  // namespace ScfgCoreTestKit

#endif
