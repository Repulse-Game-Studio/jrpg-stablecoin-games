// Copyright 2026 Repulse
// No Python twin: the tier1/paylinks.json adapters (conformance/python/scfg_conformance/tier1.py's
// eip681_transfer_uri, onramp_url and solana_pay_uri; unity/core-tests/Tier1.cs). Units arrive as
// decimal strings, read with int(); chain_id is the JSON int itself.
#include "support/Tier1.h"

#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Paylinks.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaPaylinks.h"

#include <string>
#include <string_view>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

namespace scfg_test::tier1::paylinks_adapters {

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

// int(i["units"]).
Result<BigInt> units(const Json& in) {
    SCFG_TRY(Json v, py::get(in, "units"));
    return py::py_int(v);
}

}  // namespace scfg_test::tier1::paylinks_adapters

SCFG_TIER1_TOPIC(paylinks) {
    namespace a = scfg_test::tier1::paylinks_adapters;
    fn("eip681_transfer_uri", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string token, a::str_field(in, "token_address"));
        SCFG_TRY(Json chain_id, py::get(in, "chain_id"));
        if (!chain_id.is_int()) return Err::value_error("vector input chain_id must be an int");
        SCFG_TRY(std::string recipient, a::str_field(in, "recipient"));
        SCFG_TRY(BigInt units, a::units(in));
        SCFG_TRY(std::string uri, scfg::paylinks::eip681_transfer_uri(token, chain_id.as_bigint(), recipient, units));
        return Json(std::move(uri));
    });
    fn("onramp_url", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string tmpl, a::str_field(in, "template"));
        SCFG_TRY(std::string chain, a::str_field(in, "chain"));
        SCFG_TRY(Json chain_id, py::get(in, "chain_id"));
        if (!chain_id.is_int()) return Err::value_error("vector input chain_id must be an int");
        SCFG_TRY(std::string token, a::str_field(in, "token"));
        SCFG_TRY(std::string address, a::str_field(in, "token_address"));
        SCFG_TRY(std::string amount, a::str_field(in, "amount"));
        SCFG_TRY(std::string recipient, a::str_field(in, "recipient"));
        SCFG_TRY(std::string url,
                 scfg::paylinks::onramp_url(tmpl, chain, chain_id.as_bigint(), token, address, amount, recipient));
        return Json(std::move(url));
    });
    fn("solana_pay_uri", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string recipient, a::str_field(in, "recipient"));
        SCFG_TRY(BigInt units, a::units(in));
        SCFG_TRY(std::string mint, a::str_field(in, "mint"));
        SCFG_TRY(std::string reference, a::str_field(in, "reference"));
        SCFG_TRY(std::string label, a::str_field(in, "label"));
        SCFG_TRY(std::string message, a::str_field(in, "message"));
        SCFG_TRY(std::string uri,
                 scfg::solana::paylinks::solana_pay_uri(recipient, units, mint, reference, label, message));
        return Json(std::move(uri));
    });
}
