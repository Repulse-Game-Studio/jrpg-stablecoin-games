// Twin of scfg_core/reverify.py (see Reverify.h).
#include "Scfg/Reverify.h"

#include "Scfg/Amounts.h"
#include "Scfg/Backends.h"
#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Evm.h"
#include "Scfg/Ints.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace scfg::reverify::detail {

// What one pass of reverify_receipts hands each receipt's judgement (shared by its coroutines).
struct Pass {
    backends::base::RpcFor rpc_for;
    OrderedMap<chains::Chain> chains;
    OrderedMap<std::vector<std::string>> wallets;
};

Err no_attribute(const Json& v, std::string_view attribute) {
    return Err::make("AttributeError",
                     "'" + std::string(v.type_name()) + "' object has no attribute '" + std::string(attribute) + "'");
}

// hash(v): a list or a dict is unhashable.
Status hashable(const Json& v) {
    SCFG_TRY(std::optional<std::string> key, py::str_key(v));
    (void)key;
    return ok();
}

// chains.get(v) for a hashable v: only a str can name a chain.
const chains::Chain* chain_named(const OrderedMap<chains::Chain>& chains, const Json& v) {
    return v.is_str() ? chains.find(v.as_str()) : nullptr;
}

// The body of _invalid_fields' try: the first problem, in Python's order, as the Err Python raises.
Status check_fields(const Json& r, const OrderedMap<chains::Chain>& chains) {
    SCFG_TRY(Json chain_key, py::get(r, "chain"));
    SCFG_TRY_VOID(hashable(chain_key));
    SCFG_TRY(Json token, py::get(r, "token"));
    SCFG_TRY_VOID(hashable(token));
    SCFG_TRY_VOID(hashable(r.as_dict().get("item")));  // r["chain"] worked: r is a dict
    SCFG_TRY(Json tx, py::get(r, "tx_hash"));
    if (!tx.is_str()) return no_attribute(tx, "lower");
    // int_from(r["log_index"]) < 0 or int_from(r["block"]) < 0: the block is read only for a log index >= 0. Both
    // are ints of any size: a stored 2**63 is valid, and its check decides.
    SCFG_TRY(Json stored_index, py::get(r, "log_index"));
    SCFG_TRY(BigInt log_index, ints::int_from(stored_index));
    bool negative = log_index < BigInt(0);
    if (!negative) {
        SCFG_TRY(Json stored_block, py::get(r, "block"));
        SCFG_TRY(BigInt block, ints::int_from(stored_block));
        negative = block < BigInt(0);
    }
    if (negative) return Err::value_error("log index and block must be >= 0");
    SCFG_TRY(Json stored_amount, py::get(r, "amount"));
    SCFG_TRY(Dec amount, amounts::stored_amount(stored_amount));
    SCFG_TRY(BigInt units, amounts::decimal_to_units(amount));
    (void)units;
    if (const chains::Chain* chain = chain_named(chains, chain_key)) {
        if (tx.as_str() != py::strip(tx.as_str())) {
            return Err::value_error("tx id with surrounding whitespace: " + py::repr(tx));
        }
        SCFG_TRY(const backends::base::Backend* backend, backends::backend_for(*chain));
        SCFG_TRY(std::string normalized, backend->normalize_tx(tx));
        (void)normalized;
    }
    return ok();
}

// _invalid_fields' except clause: (KeyError, TypeError, ValueError, AttributeError, ArithmeticError).
bool invalid(const Err& e) {
    return e.is_a("KeyError") || e.is_a("TypeError") || e.is_a("ValueError") || e.is_a("AttributeError") ||
           e.is_a("ArithmeticError");
}

// repr(e) of the exception an Err stands for: KeyError('amount') (a KeyError's message is already its key's repr),
// ValueError('...').
std::string exception_repr(const Err& e) {
    return e.kind + "(" + (e.kind == "KeyError" ? e.message : py::repr_str(e.message)) + ")";
}

// Python _judge: a well-formed receipt's verdict, "drop" or "keep".
Co<std::string> judge(Json r, std::shared_ptr<Pass> pass) {
    const chains::Chain* chain = chain_named(pass->chains, r.as_dict().get("chain"));
    if (chain == nullptr) co_return std::string("keep");  // chain unknown to this build: cannot judge, keep
    const Json token = r.as_dict().get("token");
    if (!token.is_str() || chain->tokens.find(token.as_str()) == nullptr) co_return std::string("drop");
    const std::vector<std::string>* family_wallets = pass->wallets.find(chain->family);
    // no wallet of this family configured any more: cannot judge, keep
    if (family_wallets == nullptr || family_wallets->empty()) co_return std::string("keep");
    SCFG_CO_TRY(const backends::base::Backend* backend, backends::backend_for(*chain));
    SCFG_CO_TRY(backends::base::Rpc rpc, pass->rpc_for(chain->key));
    co_return co_await backend->reverify(r, std::move(rpc), *chain, *family_wallets);
}

// chain.tokens[r["token"]]
Result<chains::Token> token_of(const chains::Chain& chain, const Json& r) {
    SCFG_TRY(Json key, py::get(r, "token"));
    SCFG_TRY(std::optional<std::string> name, py::str_key(key));
    if (name) {
        if (const chains::Token* t = chain.tokens.find(*name)) return *t;
    }
    return Err::key_error(py::repr(key));
}

// What check_evm_receipt's matches() compares a log with.
struct Wanted {
    std::string tx_hash;
    BigInt log_index;
    BigInt units;
    std::string token;
    std::vector<std::string> wallets;
};

// any(t is not None and t.tx_hash == tx_hash and t.log_index == log_index and t.value == units and t.recipient in
// wallets and t.token == token.address for t in (parse(lg) for lg in logs)): lazy, so the logs after the first
// match are never parsed. parse is chain.parse_transfer (via_chain) or evm.parse_transfer_log.
Result<bool> matches(const std::vector<Json>& logs, const chains::Chain& chain, bool via_chain, const Wanted& w) {
    for (const Json& lg : logs) {
        SCFG_TRY(std::optional<evm::TransferLog> t, via_chain ? chain.parse_transfer(lg) : evm::parse_transfer_log(lg));
        if (t && t->tx_hash == w.tx_hash && t->log_index == w.log_index && t->value == w.units &&
            std::find(w.wallets.begin(), w.wallets.end(), t->recipient) != w.wallets.end() && t->token == w.token) {
            return true;
        }
    }
    return false;
}

bool network(const Err& e) { return e.is_a("RpcUnavailable") || e.is_a("RpcError"); }

// `on_error or print_error`
void report(const backends::base::ErrorSink& on_error, const Err& e) {
    if (on_error) {
        on_error(e);
    } else {
        errors::print_error(e);
    }
}

// receipts.pop(key, None); removed.append(key)
void drop(Json& receipts, Json& removed, const std::string& key) {
    receipts.as_dict().erase(key);
    removed.as_list().push_back(Json(key));
}

}  // namespace scfg::reverify::detail

namespace scfg::reverify {

Co<Json> reverify_receipts(Json receipts, backends::base::RpcFor rpc_for, OrderedMap<chains::Chain> chains,
                           OrderedMap<std::vector<std::string>> wallets, backends::base::ErrorSink on_error) {
    if (!receipts.is_dict()) co_return detail::no_attribute(receipts, "items");
    auto pass = std::make_shared<detail::Pass>(detail::Pass{std::move(rpc_for), std::move(chains), std::move(wallets)});
    Json removed = Json::list();
    std::vector<std::pair<std::string, Json>> entries;  // list(receipts.items())
    for (const auto& [key, r] : receipts.as_dict()) entries.emplace_back(key, r);
    for (const auto& [key, r] : entries) {
        Status fields = detail::check_fields(r, pass->chains);
        if (!fields.ok()) {
            if (!detail::invalid(fields.err())) co_return std::move(fields).err();  // not caught by _invalid_fields
            detail::drop(receipts, removed, key);
            detail::report(on_error, Err::value_error("stored receipt " + py::repr_str(key) +
                                                      " is invalid and was removed: " +
                                                      detail::exception_repr(fields.err())));
            continue;
        }
        Result<std::string> verdict = co_await detail::judge(r, pass);
        if (!verdict.ok()) {
            if (!verdict.err().is_a("Exception")) co_return std::move(verdict).err();
            detail::report(on_error, verdict.err());  // node data the check could not read: unverifiable, kept
            continue;
        }
        if (verdict.value() == "drop") detail::drop(receipts, removed, key);
    }
    co_return removed;
}

Co<std::string> check_evm_receipt(Json r, std::shared_ptr<rpc::RpcClient> rpc, chains::Chain chain,
                                  std::vector<std::string> wallets) {
    detail::Wanted w;
    SCFG_CO_TRY(Json tx, py::get(r, "tx_hash"));
    if (!tx.is_str()) co_return detail::no_attribute(tx, "lower");
    w.tx_hash = py::lower_for_ascii(tx.as_str());  // an EVM hash (validated): ASCII hex
    SCFG_CO_TRY(Json stored_index, py::get(r, "log_index"));
    SCFG_CO_TRY(w.log_index, ints::int_from(stored_index));
    SCFG_CO_TRY(Json stored_block, py::get(r, "block"));
    SCFG_CO_TRY(BigInt block, ints::int_from(stored_block));
    SCFG_CO_TRY(Json stored_amount, py::get(r, "amount"));
    SCFG_CO_TRY(Dec amount, amounts::stored_amount(stored_amount));
    SCFG_CO_TRY(w.units, amounts::decimal_to_units(amount));
    SCFG_CO_TRY(chains::Token token, detail::token_of(chain, r));
    w.token = token.address;
    Json addressed = Json::list();
    for (const std::string& wallet : wallets) {
        SCFG_CO_TRY(std::string topic, evm::address_topic(Json(wallet)));
        addressed.as_list().push_back(Json(topic));
    }
    w.wallets = std::move(wallets);
    const Json topics = Json::list({Json(evm::TRANSFER_TOPIC), Json(), addressed});

    // try: ... except (RpcUnavailable, RpcError): return "keep"
    Result<BigInt> head = co_await rpc->block_number();
    if (!head.ok()) {
        if (detail::network(head.err())) co_return std::string("keep");
        co_return std::move(head).err();
    }
    if (block > head.value()) {
        // probably a lagging endpoint: keep, unless the block is impossibly far ahead
        co_return std::string(block > head.value() + BigInt(FUTURE_TOLERANCE_BLOCKS) ? "drop" : "keep");
    }
    Result<std::vector<Json>> logs = co_await rpc->get_logs(token.log_source(), topics, block, block);
    if (!logs.ok()) {
        if (detail::network(logs.err())) co_return std::string("keep");
        co_return std::move(logs).err();
    }
    SCFG_CO_TRY(bool found, detail::matches(logs.value(), chain, true, w));
    if (found) co_return std::string("keep");
    if (token.log_address.empty()) co_return std::string("drop");
    // A receipt stored by a game that added this chain itself before it was built in (add_chain("arc", ...)) points
    // at the token contract's own log. That log is still a genuine transfer, so it keeps the purchase.
    logs = co_await rpc->get_logs(token.address, topics, block, block);
    if (!logs.ok()) {
        if (detail::network(logs.err())) co_return std::string("keep");
        co_return std::move(logs).err();
    }
    SCFG_CO_TRY(bool own, detail::matches(logs.value(), chain, false, w));
    co_return std::string(own ? "keep" : "drop");
}

}  // namespace scfg::reverify
