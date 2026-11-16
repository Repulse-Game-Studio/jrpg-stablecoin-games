// Twin of scfg_core/reverify.py (and of unity/.../Core/Reverify.cs): re-verification of stored receipts at launch.
//
// Task 11 holds only the signature the Engine calls; its body is a stub returning Err NotImplementedError
// ("reverify: task 13"), which never satisfies a vector (the runner refuses NotImplementedError as a recorded
// "other"). Task 13 ports it.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"

#include <string>
#include <vector>

namespace scfg::reverify {

// Remove the receipts the chain contradicts; returns the removed keys (a list, in the store's order). wallets maps a
// family to the developer's wallets for it. Stub until task 13.
Co<Json> reverify_receipts(Json receipts, backends::base::RpcFor rpc_for, OrderedMap<chains::Chain> chains,
                           OrderedMap<std::vector<std::string>> wallets, backends::base::ErrorSink on_error);

}  // namespace scfg::reverify
