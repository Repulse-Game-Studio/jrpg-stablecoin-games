// Copyright 2026 Repulse
// Twin of scfg_core/restore.py (and of unity/.../Core/Restore.cs): restoring purchases from a transaction hash, the
// chain being the ledger.
//
// The player pastes a tx hash. We find the chain it lives on, pick out the USDC/USDT transfers to the developer, and
// work out what each one bought from the code encoded in its amount. Amounts that carry no code (e.g. an exchange
// sent a round 5.00) are ambiguous, so the player chooses which item it paid for, or keeps it as a tip.
//
// Errors are Python's: RestoreError (Err::restore_error, its code also its message) invalid_hash | tx_not_found |
// tx_reverted | tx_not_a_payment | network_unavailable. lookup swallows only RpcUnavailable and RpcError; anything
// else (a malformed log or receipt) fails the whole lookup, except a transfer of 10**32 units or more, which
// amounts::decode cannot price: that one is skipped and reported (classify_or_skip).
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Config.h"
#include "Scfg/Evm.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scfg::restore {

inline constexpr std::string_view TIP_CHOICE = "tip";

// Python's RestoreCandidate dataclass: one transfer of a looked-up transaction and what it bought. transfer is a
// TransferLog (EVM) or a Transfer (Solana), the node's values carried as Python carries them (Transfers.h).
struct RestoreCandidate {
    std::string chain;
    std::string token;
    std::variant<transfers::Transfer, evm::TransferLog> transfer;
    std::optional<std::string> kind;  // "unlock" | "tip" | None (the player must choose)
    std::optional<std::string> item;
    std::vector<std::string> choices;  // item ids (+ "tip") offered when kind is None
    bool already_restored = false;

    friend bool operator==(const RestoreCandidate&, const RestoreCandidate&) = default;
};

// What a transfer of some base units paid for: kind "unlock" (item set), "tip", or None, when the player must choose
// among choices (the item ids whose min_price the amount reaches, in the config's order, then TIP_CHOICE).
struct Classified {
    std::optional<std::string> kind;
    std::optional<std::string> item;
    std::vector<std::string> choices;

    friend bool operator==(const Classified&, const Classified&) = default;
};

// Python classify(config, value): decode's ValueError for negative units, its InvalidOperation from 10**32 units.
// A whole-cent amount (code 0, nonce 0) is ambiguous; code 0 with a nonce is a tip; an item code whose item the
// amount pays for is that unlock; anything else is ambiguous.
SCFGCORE_API Result<Classified> classify(const config::Config& config, const BigInt& value);

// Python classify_or_skip: classify(), or nullopt for a value decode() cannot price (InvalidOperation, 10**32 units
// or more), which is reported to on_error (errors::print_error when it is empty). Any other Err is returned.
SCFGCORE_API Result<std::optional<Classified>> classify_or_skip(const config::Config& config, const BigInt& value,
                                                                const backends::base::ErrorSink& on_error);

// Python lookup_evm(tx_hash, config=, chain=, rpc=, receipts=, on_error=): the candidates of a normalized tx hash on
// one EVM chain, nullopt if the chain lacks it. Each Transfer log (chain.parse_transfer) of one of the chain's tokens,
// of a positive value, to an accepted wallet is one candidate, already_restored when its receipt key is in receipts.
// RestoreError tx_reverted (status 0x0) or tx_not_a_payment (no candidate); a receipt that is not a dict is the
// AttributeError of its .get.
SCFGCORE_API Co<std::optional<std::vector<RestoreCandidate>>> lookup_evm(std::string tx_hash, config::Config config,
                                                                         chains::Chain chain,
                                                                         std::shared_ptr<rpc::RpcClient> rpc,
                                                                         Json receipts,
                                                                         backends::base::ErrorSink on_error);

// Python lookup(tx_hash, config=, chains=, rpc_for=, receipts=, on_error=): the candidates of the first chain, in
// chains' order, whose family reads tx_hash (normalize_tx) and that knows the transaction. RestoreError invalid_hash
// when no family reads it, else network_unavailable when a chain could not be asked (RpcUnavailable, RpcError), else
// tx_not_found; a backend's tx_reverted / tx_not_a_payment, and any other Err, leave at once.
SCFGCORE_API Co<std::vector<RestoreCandidate>> lookup(Json tx_hash, config::Config config,
                                                      OrderedMap<chains::Chain> chains, backends::base::RpcFor rpc_for,
                                                      Json receipts, backends::base::ErrorSink on_error);

// Python apply(receipts, candidates, choices): store a receipt (source "restore") for each candidate and return the
// receipts added (a list; the dicts the store now holds), in candidate order. choices maps a candidate's index to an
// item id or TIP_CHOICE for the candidates whose kind is None; one without a choice (or a None choice) is skipped,
// and a choice it does not offer is ValueError("<repr> is not an allowed choice"), raised with the earlier
// candidates' receipts already stored. A receipt whose key is already stored is not added again.
SCFGCORE_API Result<Json> apply(Json::Dict& receipts, const std::vector<RestoreCandidate>& candidates,
                                const std::optional<std::map<int64_t, Json>>& choices);

}  // namespace scfg::restore
