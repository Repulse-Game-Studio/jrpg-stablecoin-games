// Twin of scfg_core/reverify.py (and of unity/.../Core/Reverify.cs): re-verification of stored receipts at launch.
//
// Task 11 holds only the signature the Engine calls; its body is a stub returning Err NotImplementedError
// ("reverify: task 13"), which never satisfies a vector (the runner refuses NotImplementedError as a recorded
// "other"). Task 13 ports it. Task 12 adds FUTURE_TOLERANCE_BLOCKS (the Solana backend's reverify reads it) and the
// one case the stub can answer as Python does: an empty store, which has nothing to check (no RPC, no report).
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scfg::reverify {

// A receipt whose block (slot) is this far beyond the endpoint's head is impossible, not a lagging endpoint.
inline constexpr int64_t FUTURE_TOLERANCE_BLOCKS = 10'000;

// Remove the receipts the chain contradicts; returns the removed keys (a list, in the store's order). wallets maps a
// family to the developer's wallets for it. Stub until task 13, except for an empty store (an empty list).
Co<Json> reverify_receipts(Json receipts, backends::base::RpcFor rpc_for, OrderedMap<chains::Chain> chains,
                           OrderedMap<std::vector<std::string>> wallets, backends::base::ErrorSink on_error);

}  // namespace scfg::reverify
