// Copyright 2026 Repulse
// No Python twin: the build_config adapter of tier1/config.json and tier1/custom_chains.json (tier1_config.py's
// _build_config; unity/core-tests/Tier1.cs), and onramp_view's (tier1_config.py's _onramp_view). Adapters are keyed by
// fn across topics, so the custom_chains topic's build_config cases run through this one.
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/BigInt.h"
#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using scfg::BigInt;
using scfg::Dec;
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

}  // namespace

SCFG_TIER1_TOPIC(config) {
    fn("build_config", [](const Json& in) -> Result<Json> {
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(in));
        return scfg_test::config_json::config_json(c);
    });
    // onramp_view(_config(i["config"]), token=..., chain_key=..., units=None or int(i["units"]),
    // amount=Decimal(i["amount"]), step=..., path=..., can_go_back=..., option_keys=...)
    fn("onramp_view", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json config_kw, py::get(in, "config"));
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(config_kw));
        SCFG_TRY(std::string token, str_of(in, "token"));
        SCFG_TRY(Json key, py::get(in, "chain_key"));
        std::optional<std::string> chain_key;
        if (!key.is_null()) {
            if (!key.is_str()) return Err::value_error("vector input chain_key must be a str or null");
            chain_key = key.as_str();
        }
        SCFG_TRY(Json units_in, py::get(in, "units"));
        std::optional<BigInt> units;
        if (!units_in.is_null()) {
            SCFG_TRY(BigInt u, py::py_int(units_in));
            units = u;
        }
        SCFG_TRY(Json amount_in, py::get(in, "amount"));
        SCFG_TRY(std::optional<Dec> amount, Dec::from_json(amount_in));
        if (!amount) return Err::value_error("vector input amount must be a finite decimal");
        SCFG_TRY(std::string step, str_of(in, "step"));
        SCFG_TRY(std::vector<std::string> path, strs_of(in, "path"));
        SCFG_TRY(Json back, py::get(in, "can_go_back"));
        if (!back.is_bool()) return Err::value_error("vector input can_go_back must be a bool");
        SCFG_TRY(std::vector<std::string> keys, strs_of(in, "option_keys"));
        SCFG_TRY(scfg::checkout::OnrampView view, scfg::checkout::onramp_view(c, token, chain_key, units, *amount, step,
                                                                              path, back.as_bool(), keys));
        return Json::dict({{"url", view.url ? Json(*view.url) : Json()},
                           {"shows_onramp", view.shows_onramp},
                           {"shows_onramp_hint", view.shows_onramp_hint}});
    });
}
