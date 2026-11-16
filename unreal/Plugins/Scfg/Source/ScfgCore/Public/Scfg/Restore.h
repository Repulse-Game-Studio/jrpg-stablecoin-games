// Twin of scfg_core/restore.py (and of unity/.../Core/Restore.cs): restoring purchases from a transaction hash.
//
// Task 11 holds only what the Engine and the Tier 2 runner need to compile against: RestoreCandidate, TIP_CHOICE
// and the signatures of lookup and apply. Their bodies are stubs returning Err NotImplementedError ("restore ...:
// task 13"), which never satisfies a vector (the runner refuses NotImplementedError as a recorded "other"); task 13
// ports them.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Config.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scfg::restore {

inline constexpr std::string_view TIP_CHOICE = "tip";

// Python's RestoreCandidate dataclass: one transfer of a looked-up transaction and what it bought.
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

// The candidates of the first chain that knows tx_hash (RestoreError invalid_hash | tx_not_found | tx_reverted |
// tx_not_a_payment | network_unavailable). Stub until task 13.
Co<std::vector<RestoreCandidate>> lookup(Json tx_hash, config::Config config, OrderedMap<chains::Chain> chains,
                                         backends::base::RpcFor rpc_for, Json receipts,
                                         backends::base::ErrorSink on_error);

// Store receipts for the candidates; choices maps a candidate's index to an item id or "tip" for the candidates
// whose kind is None. Returns the receipts added. Stub until task 13.
Result<Json> apply(Json::Dict& receipts, const std::vector<RestoreCandidate>& candidates,
                   const std::optional<std::map<int64_t, Json>>& choices);

}  // namespace scfg::restore
