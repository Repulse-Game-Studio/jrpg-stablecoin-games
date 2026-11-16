// Twin of scfg_core/receipts.py (and of unity/.../Core/Receipts.cs, rpgmaker/src/core/receipts.ts).
// Receipts: verified on-chain transfers to the developer, stored in `persistent`.
//
// There is deliberately no "owned" flag anywhere. Ownership is always recomputed from receipts, and receipts are
// re-checked against the chain at every launch. A store is a Json dict (Python's MutableMapping) and a receipt a
// Json dict shaped as the frozen receipt schema (conformance/schema/state.json).
//
// Stored receipts are loosely typed, as in Python: a field read back from storage can hold any JSON value.
// A stored log_index is formatted exactly, whatever its type and size (Python's f-string), and a chain's transfer
// carries its log index unbounded (a BigInt, or the Json a Solana node sent), so a receipt is never narrowed. Port
// limit: an int of more than 4300 digits is formatted too, where CPython 3.9.14+ refuses str() of it (ValueError);
// no node or store holds one (int_from refuses to read one from text).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::receipts {

// f"{chain}:{tx}:{log_index}", where tx is tx_hash lowercased when it starts with "0x" (EVM hashes are hex and
// case-insensitive) and kept as it is otherwise (Solana signatures are base58: case matters; "0X..." is kept too).
std::string receipt_key(std::string_view chain, std::string_view tx_hash, const BigInt& log_index);
inline std::string receipt_key(std::string_view chain, std::string_view tx_hash, int64_t log_index) {
    return receipt_key(chain, tx_hash, BigInt(log_index));
}
// The same for values read back from storage: chain and log_index are formatted with str() whatever their type
// (an int of any size exactly), and a tx_hash that is not a str is the AttributeError of its startswith.
Result<std::string> receipt_key(const Json& chain, const Json& tx_hash, const Json& log_index);

// make_receipt(chain=, transfer=, kind=, item=, token=, source=): the receipt dict, in the schema's key order (chain,
// tx_hash, log_index, block, kind, item, amount, token, payer, recipient, source). kind must be "unlock" or "tip" and
// source "payment" or "restore", else ValueError("invalid kind <repr>") / ("invalid source <repr>"). The amount is
// str(units_to_decimal(value)); payer is the sender. A Transfer's carried values (its sender, tx_hash and log_index)
// are stored as they are, whatever their type, as Python's dict holds them.
Result<Json> make_receipt(const Json& chain, const transfers::Transfer& transfer, const Json& kind, const Json& item,
                          const Json& token, const Json& source);
Result<Json> make_receipt(const Json& chain, const evm::TransferLog& transfer, const Json& kind, const Json& item,
                          const Json& token, const Json& source);

// Stores the receipt under its key unless that key is already there; returns whether it was added (the first
// receipt for a key stays). A receipt that is not a dict, or misses chain, tx_hash or log_index, is the TypeError
// or KeyError of Python's receipt["..."]; a non-str tx_hash its AttributeError.
Result<bool> add_receipt(Json::Dict& store, const Json& receipt);

// store.pop(key, None).
void remove_receipt(Json::Dict& store, std::string_view key);

// Python's (owned, tips) tuple.
struct Ownership {
    std::vector<std::string> owned;  // the owned item ids, sorted by code point (Python returns a set)
    Dec tip_total;                   // the Decimal sum of the tips: the most fraction digits, never quantized; "0"
};

// (owned item ids, total tipped). Malformed entries are skipped, never fatal: a value that is not a dict, a missing
// kind or amount, an amount amounts::stored_amount refuses (so the total stays finite), and an unlock whose item is a
// list or a dict (unhashable: items.get raises TypeError). An unlock owns its item when the item is in `items` and the
// amount is at least its min_price. The sum fails only with Decimal's Overflow, which bounded amounts cannot reach.
Result<Ownership> compute_ownership(const Json::Dict& store, const OrderedMap<models::Item>& items);

}  // namespace scfg::receipts
