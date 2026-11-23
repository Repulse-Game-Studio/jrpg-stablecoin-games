// Copyright 2026 Repulse
// No Python twin: the build_config adapter of tier1/config.json and tier1/custom_chains.json (tier1_config.py's
// _build_config; unity/core-tests/Tier1.cs), and network_view's and card_url's (tier1_config.py's
// _network_view and _card_url). Adapters are keyed by
// fn across topics, so the custom_chains topic's build_config cases run through this one.
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/BigInt.h"
#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

namespace {

Result<std::string> str_of(const Json& in, std::string_view key) {
    SCFG_TRY(Json v, py::get(in, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

Result<std::vector<std::string>> strs_of(const Json& in, std::string_view key) {
    SCFG_TRY(Json v, py::get(in, key));
    if (!v.is_list()) return Err::value_error("vector input " + std::string(key) + " must be a list");
    std::vector<std::string> out;
    for (const Json& e : v.as_list()) {
        if (!e.is_str()) return Err::value_error("vector input " + std::string(key) + " must hold strs");
        out.push_back(e.as_str());
    }
    return out;
}

Result<std::optional<std::string>> opt_str_of(const Json& in, std::string_view key) {
    SCFG_TRY(Json v, py::get(in, key));
    if (v.is_null()) return std::optional<std::string>();
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str or null");
    return std::optional<std::string>(v.as_str());
}

}  // namespace

SCFG_TIER1_TOPIC(config) {
    fn("build_config", [](const Json& in) -> Result<Json> {
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(in));
        return scfg_test::config_json::config_json(c);
    });
    // network_view(_config(i["config"]), option_keys=..., chain_key=..., expanded=...)
    fn("network_view", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json config_kw, py::get(in, "config"));
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(config_kw));
        SCFG_TRY(std::vector<std::string> keys, strs_of(in, "option_keys"));
        SCFG_TRY(std::optional<std::string> chain_key, opt_str_of(in, "chain_key"));
        SCFG_TRY(Json expanded, py::get(in, "expanded"));
        if (!expanded.is_bool()) return Err::value_error("vector input expanded must be a bool");
        SCFG_TRY(scfg::checkout::NetworkView view, scfg::checkout::network_view(c, keys, chain_key, expanded.as_bool()));
        auto list = [](const std::vector<std::string>& v) {
            Json out = Json::list({});
            for (const std::string& k : v) out.as_list().push_back(Json(k));
            return out;
        };
        return Json::dict({{"main", list(view.main)},
                           {"more", list(view.more)},
                           {"expanded", view.expanded},
                           {"card", list(view.card)}});
    });
    // card_url(_config(i["config"]), token=..., chain_key=..., units=None or int(i["units"]))
    fn("card_url", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json config_kw, py::get(in, "config"));
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(config_kw));
        SCFG_TRY(std::string token, str_of(in, "token"));
        SCFG_TRY(std::optional<std::string> chain_key, opt_str_of(in, "chain_key"));
        SCFG_TRY(Json units_in, py::get(in, "units"));
        std::optional<BigInt> units;
        if (!units_in.is_null()) {
            SCFG_TRY(BigInt u, py::py_int(units_in));
            units = u;
        }
        SCFG_TRY(std::optional<std::string> url, scfg::checkout::card_url(c, token, chain_key, units));
        return url ? Json(*url) : Json();
    });
}
