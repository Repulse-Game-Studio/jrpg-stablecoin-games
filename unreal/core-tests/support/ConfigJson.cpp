// Copyright 2026 Repulse
// No Python twin: tier1_config.py's adapters and canonical.py's to_jsonable of the config dataclasses (see
// ConfigJson.h).
#include "support/ConfigJson.h"

#include "support/Tier1.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <string>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace chains = scfg::chains;
namespace config = scfg::config;
namespace py = scfg::py;

namespace scfg_test::config_json {

namespace detail {

Json strings(const std::vector<std::string>& xs) {
    Json out = Json::list();
    for (const std::string& x : xs) out.as_list().emplace_back(x);
    return out;
}

Json optional_str(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

}  // namespace detail

Json token_json(const chains::Token& t) {
    return Json::dict({{"address", t.address},
                       {"decimals", t.decimals},
                       {"log_address", t.log_address},
                       {"log_decimals", t.log_decimals}});
}

Json chain_json(const chains::Chain& c) {
    Json tokens = Json::dict();
    for (const auto& [symbol, t] : c.tokens) tokens.as_dict().set(symbol, token_json(t));
    return Json::dict({{"key", c.key},
                       {"chain_id", c.chain_id},
                       {"name", c.name},
                       {"rpc", detail::strings(c.rpc)},
                       {"confirmations", c.confirmations},
                       {"max_log_range", c.max_log_range},
                       {"explorer_tx", c.explorer_tx},
                       {"native_symbol", c.native_symbol},
                       {"fee_hint", c.fee_hint},
                       {"tokens", tokens},
                       {"testnet", c.testnet},
                       {"family", c.family},
                       {"wallet_chain", c.wallet_chain},
                       {"native_token", c.native_token},
                       {"min_fee_wei", c.min_fee_wei}});
}

Json chains_json(const scfg::OrderedMap<chains::Chain>& all) {
    Json out = Json::dict();
    for (const auto& [key, c] : all) out.as_dict().set(key, chain_json(c));
    return out;
}

Json item_json(const scfg::models::Item& it) {
    return Json::dict({{"item_id", it.item_id},
                       {"code", it.code},
                       {"price", it.price.str()},
                       {"token", it.token},
                       {"title", it.title},
                       {"min_price", it.min_price.str()}});
}

Json config_json(const config::Config& c) {
    Json rpc_urls = Json::dict();
    for (const auto& [key, urls] : c.rpc_urls) rpc_urls.as_dict().set(key, detail::strings(urls));
    Json confirmations = Json::dict();
    for (const auto& [key, n] : c.confirmations) confirmations.as_dict().set(key, n);
    Json items = Json::dict();
    for (const auto& [key, it] : c.items) items.as_dict().set(key, item_json(it));
    return Json::dict({{"wallet", detail::optional_str(c.wallet)},
                       {"testnet", c.testnet},
                       {"chains", detail::strings(c.chains)},
                       {"tokens", detail::strings(c.tokens)},
                       {"methods", c.methods},
                       {"rpc_urls", rpc_urls},
                       {"confirmations", confirmations},
                       {"previous_wallets", detail::strings(c.previous_wallets)},
                       {"solana_wallet", detail::optional_str(c.solana_wallet)},
                       {"previous_solana_wallets", detail::strings(c.previous_solana_wallets)},
                       {"logo", detail::optional_str(c.logo)},
                       {"accent", c.accent},
                       {"custom_chains", chains_json(c.custom_chains)},
                       {"onramp", Json::dict({{"enabled", c.onramp.enabled},
                                              {"url", c.onramp.url},
                                              {"chains", detail::strings(c.onramp.chains)}})},
                       {"items", items}});
}

Json spec_json(const scfg::payments::PaymentSpec& s) {
    return Json::dict({{"kind", s.kind},
                       {"item", s.item},
                       {"price", s.price.str()},
                       {"token", s.token},
                       {"code", s.code},
                       {"title", s.title},
                       {"chains", detail::strings(s.chains)}});
}

Result<config::Config> config_from(const Json& kwargs) {
    if (!kwargs.is_dict() || !kwargs.as_dict().contains("custom_chains")) return config::build_config(kwargs);
    Json kw = Json::dict();
    for (const auto& [key, value] : kwargs.as_dict()) {
        if (key != "custom_chains") kw.as_dict().set(key, value);
    }
    const Json& given = *kwargs.as_dict().find("custom_chains");
    if (!given.is_list()) return Err::type_error("vector input custom_chains must be a list");
    std::vector<config::CustomChain> custom;
    for (const Json& entry : given.as_list()) {
        if (entry.is_dict()) {
            SCFG_TRY(chains::Chain c, config::build_chain(entry));
            custom.emplace_back(std::move(c));
        } else {
            custom.emplace_back(entry);
        }
    }
    return config::build_config(kw, std::move(custom));
}

Result<std::pair<Json, Json>> item_kwargs(const Json& d) {
    if (!d.is_dict()) return Err::type_error("vector input item must be an object");
    Json item_id;
    bool found = false;
    Json rest = Json::dict();
    for (const auto& [key, value] : d.as_dict()) {
        SCFG_TRY(Json decoded, scfg_test::tier1::decode_value(value));
        if (key == "item_id") {
            item_id = decoded;
            found = true;
        } else {
            rest.as_dict().set(key, decoded);
        }
    }
    if (!found) return py::key_error("item_id");
    return std::pair<Json, Json>(item_id, rest);
}

Result<config::Config> with_items(const Json& config_kw, const Json& items) {
    SCFG_TRY(config::Config cfg, config_from(config_kw));
    if (!items.is_list()) return Err::type_error("vector input items must be a list");
    for (const Json& item : items.as_list()) {
        SCFG_TRY(auto kw, item_kwargs(item));
        SCFG_TRY_VOID(config::add_item(cfg, kw.first, kw.second));
    }
    return cfg;
}

}  // namespace scfg_test::config_json
