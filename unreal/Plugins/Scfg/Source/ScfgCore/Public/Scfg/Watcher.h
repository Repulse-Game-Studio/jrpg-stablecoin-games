// Twin of scfg_core/watcher.py (and of unity/.../Core/Watcher.cs): the payment watcher, advancing one pending EVM
// session per call.
//
//     awaiting --(hinted tx verified | exact amount found by scan)--> confirming
//     confirming --(N confirmations, block hash unchanged)--> paid (receipt stored)
//     confirming --(block hash changed: reorg)--> awaiting
//     awaiting --(hinted tx pays less than min_price)--> underpaid
//     awaiting --(24 h, nothing found)--> closed
//
// advance() is logic over plain Json dicts plus an RpcClient. State is only mutated after RPC calls succeed, so a
// network outage never corrupts a session. Stored records are loosely typed, as in Python: a field is read with
// Python's rec["..."] (a KeyError or TypeError for a malformed record, which the Engine contains per session),
// compared and subtracted with Python's number rules (py::compare, py::sub), and a value Python only carries along
// (kind, item, token, id, a match's payer) is carried as it is. Port limit: a match whose value is not an int is a
// TypeError where Python's Decimal() would read a str or a float.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Dec.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace scfg::watcher {

inline constexpr int64_t SCAN_STEP_BLOCKS = 10'000;  // upper bound on blocks scanned per advance()
inline constexpr int64_t MAX_SCAN_CALLS = 5;         // eth_getLogs calls per advance(), to be gentle with public RPCs

// Python's StepOutcome dataclass.
struct StepOutcome {
    std::string session_id;
    // missing | waiting | confirming | reorged | paid | underpaid | closed | expired
    std::string status;
    BigInt confirmations = 0;
    BigInt required = 0;
    std::optional<models::PaymentResult> result;
    // network_unavailable | tx_reverted | tx_not_a_payment | tx_not_landed (Solana) | internal_error (Engine::tick:
    // the session's step crashed)
    std::optional<std::string> error;
    // The hint a tx_reverted / tx_not_a_payment / tx_not_landed verdict is about (the stored hint_tx, as it was),
    // so the screen can tell it from a newer hint reported meanwhile; None otherwise.
    Json hint_tx;
};

// The least an unlock must pay: its item's min_price when the game still sells the item, else the session's own
// price (stored_amount of rec["price"]); a tip's least is one base unit, Decimal("0.000001").
Result<Dec> min_amount(const Json& rec, const OrderedMap<models::Item>& items);

// The PaymentResult of a matched session: kind, item, token and id from rec, amount, tx_hash and payer from the
// match m, read in Python's keyword order.
Result<models::PaymentResult> result_from_match(const Json& rec, std::string status, std::string chain_key,
                                                const Json& m);

// Advance pending[sid] by one step (nothing for a sid that is not there: status "missing"). A network failure
// (RpcUnavailable, RpcError) is the outcome's error "network_unavailable", never an Err; any other failure is the
// Err, for the Engine to contain. pending and receipts are dicts (shared handles); now is the Engine's clock value.
Co<StepOutcome> advance(Json pending, Json receipts, std::string sid, std::shared_ptr<rpc::RpcClient> rpc,
                        chains::Chain chain, std::string wallet, OrderedMap<models::Item> items, Json now);

}  // namespace scfg::watcher
