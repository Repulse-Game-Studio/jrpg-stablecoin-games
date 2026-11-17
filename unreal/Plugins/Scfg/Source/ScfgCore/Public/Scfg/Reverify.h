// Twin of scfg_core/reverify.py (and of unity/.../Core/Reverify.cs): launch-time re-verification of stored receipts
// against the chain.
//
// Each receipt is checked by its chain family's backend:
//
// - EVM: one eth_getLogs call scoped to the receipt's own block (check_evm_receipt). Block-scoped lookups keep
//   working years later, even on nodes that no longer index old transaction hashes.
// - Solana: getTransaction on the receipt's signature, asked of each endpoint in turn; a missing transaction only
//   counts when an endpoint whose history reaches back to the receipt's slot says so (BackendsSolana.h).
//
// Only a *definitive* mismatch removes a receipt; network trouble never does. Stored receipts are read as Python
// reads them: a log index or block is int_from's int of any size (a stored 2**63 is valid, and the node's answer
// decides), compared exactly with what the node sent.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Rpc.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scfg::reverify {

// A receipt whose block (slot) is this far beyond the endpoint's head is impossible, not a lagging endpoint.
inline constexpr int64_t FUTURE_TOLERANCE_BLOCKS = 10'000;

// Python reverify_receipts(receipts, rpc_for=, chains=, wallets=, on_error=): remove the receipts the chain
// contradicts and return the removed keys (a list of strs, in the store's iteration order). wallets maps a family to
// the developer's wallets for it; a family with none keeps its receipts, which cannot be judged.
//
// A stored entry whose own fields are invalid is removed and the reason reported to on_error (errors::print_error
// when it is empty) as ValueError("stored receipt <key repr> is invalid and was removed: <repr of the reason>"): a
// forged receipt cannot escape verification by making its check fail. The fields are checked before any RPC, in
// Python's order: a missing field; a chain, token or item that cannot be a key (a list or a dict); a tx hash that is
// not a str; a log index, then a block, that is not an int >= 0 as int_from reads it; an amount that is not a number
// of base units within stored_amount's range; and, on a chain this build knows, a tx id with surrounding whitespace
// or not of its family's format. A receipt naming a chain this build does not know is kept, its family fields
// unchecked: a configuration change must not erase purchases.
//
// A check that fails because of what a node returned (any Exception, as Python's `except Exception`) is not the
// receipt's fault: that receipt is kept as unverifiable, the failure is reported, and the receipts after it are
// still checked. A BaseException that is not an Exception (CancelledError) leaves at once.
SCFGCORE_API Co<Json> reverify_receipts(Json receipts, backends::base::RpcFor rpc_for, OrderedMap<chains::Chain> chains,
                                        OrderedMap<std::vector<std::string>> wallets,
                                        backends::base::ErrorSink on_error);

// Python check_evm_receipt(r, rpc=, chain=, wallets=): judge one stored EVM receipt (its fields validated): "drop"
// if the chain contradicts it, else "keep". The receipt's block must not be past the head (more than
// FUTURE_TOLERANCE_BLOCKS past it drops; less is a lagging endpoint, kept); then a Transfer log in that block, from
// the token's log source, with the receipt's tx hash, log index and amount, to one of the wallets, keeps it. A
// token whose payments are logged elsewhere (Arc) also accepts the token contract's own log, which a game that added
// the chain itself before it was built in stored. RpcUnavailable and RpcError keep it; a malformed log is its Err
// (parsed only as far as Python's any() reads).
SCFGCORE_API Co<std::string> check_evm_receipt(Json r, std::shared_ptr<rpc::RpcClient> rpc, chains::Chain chain,
                                               std::vector<std::string> wallets);

}  // namespace scfg::reverify
