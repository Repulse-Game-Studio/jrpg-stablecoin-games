// Twin of scfg_core/restore.py (see Restore.h).
#include "Scfg/Restore.h"

#include "Scfg/Amounts.h"
#include "Scfg/Backends.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"

#include <algorithm>
#include <utility>

namespace scfg::restore::detail {

bool network(const Err& e) { return e.is_a("RpcUnavailable") || e.is_a("RpcError"); }

// make_receipt(chain=c.chain, transfer=c.transfer, ...) of either transfer class.
Result<Json> receipt_of(const RestoreCandidate& c, const Json& kind, const Json& item) {
    if (const auto* log = std::get_if<evm::TransferLog>(&c.transfer)) {
        return receipts::make_receipt(Json(c.chain), *log, kind, item, Json(c.token), Json("restore"));
    }
    return receipts::make_receipt(Json(c.chain), std::get<transfers::Transfer>(c.transfer), kind, item, Json(c.token),
                                  Json("restore"));
}

Json optional_text(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

}  // namespace scfg::restore::detail

namespace scfg::restore {

Result<Classified> classify(const config::Config& config, const BigInt& value) {
    SCFG_TRY(amounts::Decoded decoded, amounts::decode(value));
    const Dec amount = amounts::units_to_decimal(value);
    std::vector<std::string> affordable;
    for (const auto& [id, item] : config.items) {
        (void)id;
        if (item.min_price <= amount) affordable.push_back(item.item_id);
    }
    if (decoded.code == 0 && decoded.nonce == 0) {
        // A whole-cent amount carries no code at all: could be an exchange-rounded purchase or a plain tip. Let the
        // player say which.
        affordable.emplace_back(TIP_CHOICE);
        return Classified{std::nullopt, std::nullopt, std::move(affordable)};
    }
    if (decoded.code == 0) return Classified{std::string("tip"), std::nullopt, {}};
    const models::Item* item = config.item_by_code(decoded.code);
    if (item != nullptr && amount >= item->min_price) return Classified{std::string("unlock"), item->item_id, {}};
    affordable.emplace_back(TIP_CHOICE);
    return Classified{std::nullopt, std::nullopt, std::move(affordable)};
}

Result<std::optional<Classified>> classify_or_skip(const config::Config& config, const BigInt& value,
                                                   const backends::base::ErrorSink& on_error) {
    Result<Classified> c = classify(config, value);
    if (c.ok()) return std::optional<Classified>(c.take());
    if (!c.err().is_a("InvalidOperation")) return std::move(c).err();
    if (on_error) {
        on_error(c.err());
    } else {
        errors::print_error(c.err());
    }
    return std::optional<Classified>();
}

Co<std::optional<std::vector<RestoreCandidate>>> lookup_evm(std::string tx_hash, config::Config config,
                                                            chains::Chain chain, std::shared_ptr<rpc::RpcClient> rpc,
                                                            Json receipts, backends::base::ErrorSink on_error) {
    using Candidates = std::vector<RestoreCandidate>;
    SCFG_CO_TRY(Json receipt, co_await rpc->get_transaction_receipt(Json(tx_hash)));
    if (receipt.is_null()) co_return std::optional<Candidates>();
    if (!receipt.is_dict()) {
        co_return Err::make("AttributeError", "'" + std::string(receipt.type_name()) + "' object has no attribute 'get'");
    }
    if (py::eq(receipt.as_dict().get("status"), Json("0x0"))) co_return Err::restore_error("tx_reverted", "tx_reverted");
    OrderedMap<std::string> by_address;  // {t.address: name}: a later name for the same address wins, as in a dict
    for (const auto& [name, token] : chain.tokens) by_address.set(token.address, name);
    const std::vector<std::string> wallets = config.accepted_wallets();
    Candidates out;
    const Json logs = receipt.as_dict().get("logs");
    std::vector<Json> each;
    if (py::truthy(logs)) {  // receipt.get("logs") or []
        SCFG_CO_TRY(each, py::iterate(logs));
    }
    for (const Json& log : each) {
        SCFG_CO_TRY(std::optional<evm::TransferLog> t, chain.parse_transfer(log));
        if (!t) continue;
        const std::string* name = by_address.find(t->token);
        if (name == nullptr || t->value <= BigInt(0)) continue;
        if (std::find(wallets.begin(), wallets.end(), t->recipient) == wallets.end()) continue;
        SCFG_CO_TRY(std::optional<Classified> classified, classify_or_skip(config, t->value, on_error));
        if (!classified) continue;
        RestoreCandidate c;
        c.chain = chain.key;
        c.token = *name;
        c.already_restored =
            receipts.is_dict() && receipts.as_dict().contains(receipts::receipt_key(chain.key, t->tx_hash, t->log_index));
        c.transfer = std::move(*t);
        c.kind = std::move(classified->kind);
        c.item = std::move(classified->item);
        c.choices = std::move(classified->choices);
        out.push_back(std::move(c));
    }
    if (out.empty()) co_return Err::restore_error("tx_not_a_payment", "tx_not_a_payment");
    co_return std::optional<Candidates>(std::move(out));
}

Co<std::vector<RestoreCandidate>> lookup(Json tx_hash, config::Config config, OrderedMap<chains::Chain> chains,
                                         backends::base::RpcFor rpc_for, Json receipts,
                                         backends::base::ErrorSink on_error) {
    bool network_trouble = false;
    bool understood = false;
    for (const auto& [key, chain] : chains) {
        SCFG_CO_TRY(const backends::base::Backend* backend, backends::backend_for(chain));
        Result<std::string> tx = backend->normalize_tx(tx_hash);
        if (!tx.ok()) {
            if (tx.err().is_a("ValueError")) continue;  // not this family's id
            co_return std::move(tx).err();
        }
        understood = true;
        // try: out = backend.lookup(tx, ..., rpc=rpc_for(key), ...) except (RpcUnavailable, RpcError): continue
        Result<backends::base::Rpc> rpc = rpc_for(key);
        if (!rpc.ok()) {
            if (!detail::network(rpc.err())) co_return std::move(rpc).err();
            network_trouble = true;
            continue;
        }
        Result<std::optional<std::vector<RestoreCandidate>>> out =
            co_await backend->lookup(tx.take(), config, chain, rpc.take(), receipts, on_error);
        if (!out.ok()) {
            if (!detail::network(out.err())) co_return std::move(out).err();
            network_trouble = true;
            continue;
        }
        if (out.value()) co_return std::move(*out.take());
    }
    if (!understood) co_return Err::restore_error("invalid_hash", "invalid_hash");
    const char* code = network_trouble ? "network_unavailable" : "tx_not_found";
    co_return Err::restore_error(code, code);
}

Result<Json> apply(Json::Dict& receipts, const std::vector<RestoreCandidate>& candidates,
                   const std::optional<std::map<int64_t, Json>>& choices) {
    Json added = Json::list();
    for (size_t i = 0; i < candidates.size(); ++i) {
        const RestoreCandidate& c = candidates[i];
        Json kind = detail::optional_text(c.kind);
        Json item = detail::optional_text(c.item);
        if (!c.kind) {
            Json choice;  // choices.get(i)
            if (choices) {
                auto it = choices->find(static_cast<int64_t>(i));
                if (it != choices->end()) choice = it->second;
            }
            if (choice.is_null()) continue;
            const bool allowed =
                choice.is_str() && std::find(c.choices.begin(), c.choices.end(), choice.as_str()) != c.choices.end();
            if (!allowed) return Err::value_error(py::repr(choice) + " is not an allowed choice");
            if (choice.as_str() == TIP_CHOICE) {
                kind = Json("tip");
                item = Json();
            } else {
                kind = Json("unlock");
                item = choice;
            }
        }
        SCFG_TRY(Json r, detail::receipt_of(c, kind, item));
        SCFG_TRY(bool stored, receipts::add_receipt(receipts, r));
        if (stored) added.as_list().push_back(r);
    }
    return added;
}

}  // namespace scfg::restore
