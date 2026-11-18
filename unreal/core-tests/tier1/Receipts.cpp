// Copyright 2026 Repulse
// No Python twin: the tier1/receipts.json adapters (conformance/python/scfg_conformance/tier1.py's receipts block;
// unity/core-tests/Tier1.cs). Stores are shallow copies (dict(i["store"])); a transfer is rebuilt from its "class".
#include "support/Tier1.h"

#include "Scfg/Dec.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using scfg::Dec;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;
namespace receipts = scfg::receipts;

namespace scfg_test::tier1::receipts_adapters {

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

Result<scfg::BigInt> int_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_int()) return Err::value_error("vector input " + std::string(key) + " must be an int");
    return v.as_bigint();
}

// dict(i[key]).
Result<Json::Dict> store_copy(const Json& in, std::string_view key) {
    SCFG_TRY(Json v, py::get(in, key));
    if (!v.is_dict()) return Err::value_error("vector input " + std::string(key) + " must be a dict");
    return v.as_dict();
}

// Decimal(x[key]) for a finite decimal string.
Result<Dec> dec_field(const Json& d, std::string_view key) {
    SCFG_TRY(std::string text, str_field(d, key));
    SCFG_TRY(std::optional<Dec> v, Dec::parse(text));
    if (!v) return Err::value_error("vector input " + std::string(key) + " must be a finite decimal");
    return *v;
}

// tier1.py's _transfer, then make_receipt.
Result<Json> make_receipt(const Json& in) {
    SCFG_TRY(Json t, py::get(in, "transfer"));
    SCFG_TRY(std::string cls, str_field(t, "class"));
    SCFG_TRY(Json chain, py::get(in, "chain"));
    SCFG_TRY(Json kind, py::get(in, "kind"));
    SCFG_TRY(Json item, py::get(in, "item"));
    SCFG_TRY(Json token, py::get(in, "token"));
    SCFG_TRY(Json source, py::get(in, "source"));
    SCFG_TRY(Json value, py::get(t, "value"));
    SCFG_TRY(scfg::BigInt units, py::py_int(value));
    SCFG_TRY(std::string ttoken, str_field(t, "token"));
    SCFG_TRY(std::string recipient, str_field(t, "recipient"));
    SCFG_TRY(std::string tx_hash, str_field(t, "tx_hash"));
    SCFG_TRY(scfg::BigInt log_index, int_field(t, "log_index"));
    SCFG_TRY(scfg::BigInt block_number, int_field(t, "block_number"));
    SCFG_TRY(Json sender, py::get(t, "sender"));
    if (cls == "TransferLog") {
        if (!sender.is_str()) return Err::value_error("vector input sender must be a str");
        SCFG_TRY(std::string block_hash, str_field(t, "block_hash"));
        scfg::evm::TransferLog log{ttoken, sender.as_str(), recipient, units, tx_hash, log_index, block_number,
                                   block_hash};
        return receipts::make_receipt(chain, log, kind, item, token, source);
    }
    if (cls == "Transfer") {
        scfg::transfers::Transfer transfer{ttoken, sender, recipient, units, Json(tx_hash), Json(log_index), block_number};
        return receipts::make_receipt(chain, transfer, kind, item, token, source);
    }
    return Err::value_error("unknown transfer class " + py::repr_str(cls));
}

// tier1.py's _compute_ownership: items keyed by item_id.
Result<Json> compute_ownership(const Json& in) {
    SCFG_TRY(Json list, py::get(in, "items"));
    if (!list.is_list()) return Err::value_error("vector input items must be a list");
    scfg::OrderedMap<scfg::models::Item> items;
    for (const Json& x : list.as_list()) {
        scfg::models::Item item;
        SCFG_TRY(item.item_id, str_field(x, "item_id"));
        SCFG_TRY(scfg::BigInt code, int_field(x, "code"));
        if (!code.fits_int64()) return Err::value_error("vector input code must be an int64");
        item.code = code.to_int64().value();
        SCFG_TRY(item.price, dec_field(x, "price"));
        SCFG_TRY(item.token, str_field(x, "token"));
        SCFG_TRY(item.title, str_field(x, "title"));
        SCFG_TRY(item.min_price, dec_field(x, "min_price"));
        items.set(item.item_id, item);
    }
    SCFG_TRY(Json store, py::get(in, "store"));
    if (!store.is_dict()) return Err::value_error("vector input store must be a dict");
    SCFG_TRY(receipts::Ownership o, receipts::compute_ownership(store.as_dict(), items));
    Json owned = Json::list();
    for (const std::string& id : o.owned) owned.as_list().push_back(Json(id));
    return Json::dict({{"owned", owned}, {"tip_total", Json(o.tip_total.str())}});
}

}  // namespace scfg_test::tier1::receipts_adapters

SCFG_TIER1_TOPIC(receipts) {
    namespace a = scfg_test::tier1::receipts_adapters;
    fn("receipt_key", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json chain, py::get(in, "chain"));
        SCFG_TRY(Json tx_hash, py::get(in, "tx_hash"));
        SCFG_TRY(Json log_index, py::get(in, "log_index"));
        SCFG_TRY(std::string key, receipts::receipt_key(chain, tx_hash, log_index));
        return Json(key);
    });
    fn("make_receipt", [](const Json& in) -> Result<Json> { return a::make_receipt(in); });
    fn("add_receipt", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json::Dict store, a::store_copy(in, "store"));
        SCFG_TRY(Json receipt, py::get(in, "receipt"));
        SCFG_TRY(bool added, receipts::add_receipt(store, receipt));
        return Json::dict({{"added", Json(added)}, {"store", Json(std::move(store))}});
    });
    fn("remove_receipt", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json::Dict store, a::store_copy(in, "store"));
        SCFG_TRY(std::string key, a::str_field(in, "key"));
        receipts::remove_receipt(store, key);
        return Json(std::move(store));
    });
    fn("compute_ownership", [](const Json& in) -> Result<Json> { return a::compute_ownership(in); });
}
