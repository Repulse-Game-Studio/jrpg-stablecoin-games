// Twin of scfg_core/receipts.py (see Receipts.h).
#include "Scfg/Receipts.h"

#include "Scfg/Amounts.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <set>
#include <string>
#include <utility>

namespace scfg::receipts::detail {

// str(v): a str itself, anything else its repr (Python's str and repr agree on None, bools, ints, floats, lists and
// dicts).
// An int is py::int_str (CPython's 4300-digit limit for str()).
Result<std::string> str_of(const Json& v) {
    if (v.is_int()) return py::int_str(v.as_bigint());
    if (v.is_str()) return v.as_str();
    return py::repr(v);
}

// The tx part of a key. lower_for_ascii is str.lower() for every hash a chain gives (ASCII).
std::string tx_part(std::string_view tx_hash) {
    return tx_hash.starts_with("0x") ? py::lower_for_ascii(tx_hash) : std::string(tx_hash);
}

Result<Json> make(const Json& chain, const transfers::Transfer& t, const Json& kind, const Json& item,
                  const Json& token, const Json& source) {
    const bool kind_ok = kind.is_str() && (kind.as_str() == "unlock" || kind.as_str() == "tip");
    if (!kind_ok) {
        SCFG_TRY(std::string shown, py::repr(kind));
        return Err::value_error("invalid kind " + shown);
    }
    const bool source_ok = source.is_str() && (source.as_str() == "payment" || source.as_str() == "restore");
    if (!source_ok) {
        SCFG_TRY(std::string shown, py::repr(source));
        return Err::value_error("invalid source " + shown);
    }
    return Json::dict({{"chain", chain},
                       {"tx_hash", t.tx_hash},
                       {"log_index", t.log_index},
                       {"block", Json(t.block_number)},
                       {"kind", kind},
                       {"item", item},
                       {"amount", Json(amounts::units_to_decimal(t.value).str())},
                       {"token", token},
                       {"payer", t.sender},
                       {"recipient", Json(t.recipient)},
                       {"source", source}});
}

// compute_ownership's `except (KeyError, TypeError, ValueError, AttributeError): continue`.
bool skipped(const Err& e) {
    return e.is_a("KeyError") || e.is_a("TypeError") || e.is_a("ValueError") || e.is_a("AttributeError");
}

}  // namespace scfg::receipts::detail

namespace scfg::receipts {

Result<std::string> receipt_key(std::string_view chain, std::string_view tx_hash, const BigInt& log_index) {
    SCFG_TRY(std::string index, py::int_str(log_index));
    return std::string(chain) + ":" + detail::tx_part(tx_hash) + ":" + index;
}

std::string receipt_key(std::string_view chain, std::string_view tx_hash, int64_t log_index) {
    return std::string(chain) + ":" + detail::tx_part(tx_hash) + ":" + py::dec(log_index);  // an int64: 20 digits
}

Result<std::string> receipt_key(const Json& chain, const Json& tx_hash, const Json& log_index) {
    if (!tx_hash.is_str()) {
        return Err::make("AttributeError",
                         "'" + std::string(tx_hash.type_name()) + "' object has no attribute 'startswith'");
    }
    const std::string tx = detail::tx_part(tx_hash.as_str());
    SCFG_TRY(std::string head, detail::str_of(chain));  // the f-string's fields in order
    SCFG_TRY(std::string index, detail::str_of(log_index));
    return head + ":" + tx + ":" + index;
}

Result<Json> make_receipt(const Json& chain, const transfers::Transfer& transfer, const Json& kind, const Json& item,
                          const Json& token, const Json& source) {
    return detail::make(chain, transfer, kind, item, token, source);
}

Result<Json> make_receipt(const Json& chain, const evm::TransferLog& transfer, const Json& kind, const Json& item,
                          const Json& token, const Json& source) {
    return detail::make(chain, transfer.to_transfer(), kind, item, token, source);
}

Result<bool> add_receipt(Json::Dict& store, const Json& receipt) {
    SCFG_TRY(Json chain, py::get(receipt, "chain"));
    SCFG_TRY(Json tx_hash, py::get(receipt, "tx_hash"));
    SCFG_TRY(Json log_index, py::get(receipt, "log_index"));
    SCFG_TRY(std::string key, receipt_key(chain, tx_hash, log_index));
    if (store.contains(key)) return false;
    store.set(std::move(key), receipt);
    return true;
}

void remove_receipt(Json::Dict& store, std::string_view key) { store.erase(key); }

Result<Ownership> compute_ownership(const Json::Dict& store, const OrderedMap<models::Item>& items) {
    std::set<std::string> owned;  // UTF-8 (and WTF-8) byte order is code point order
    Dec tips;
    for (const auto& [unused_key, r] : store) {
        (void)unused_key;
        Result<Json> kind = py::get(r, "kind");
        if (!kind.ok()) {
            if (detail::skipped(kind.err())) continue;
            return kind.err();
        }
        Result<Json> raw = py::get(r, "amount");
        if (!raw.ok()) {
            if (detail::skipped(raw.err())) continue;
            return raw.err();
        }
        Result<Dec> amount = amounts::stored_amount(raw.value());
        if (!amount.ok()) {
            if (detail::skipped(amount.err())) continue;
            return amount.err();
        }
        const bool unlock = kind.value().is_str() && kind.value().as_str() == "unlock";
        const bool tip = kind.value().is_str() && kind.value().as_str() == "tip";
        if (unlock) {
            // items.get(r.get("item")): a list or a dict is unhashable (TypeError, skipped); only a str can name an item.
            Json item_id = r.as_dict().get("item");
            if (item_id.is_list() || item_id.is_dict()) continue;
            const models::Item* item = item_id.is_str() ? items.find(item_id.as_str()) : nullptr;
            if (item != nullptr && amount.value() >= item->min_price) owned.insert(item->item_id);
        } else if (tip) {
            SCFG_TRY(tips, tips + amount.value());
        }
    }
    return Ownership{std::vector<std::string>(owned.begin(), owned.end()), tips};
}

}  // namespace scfg::receipts
