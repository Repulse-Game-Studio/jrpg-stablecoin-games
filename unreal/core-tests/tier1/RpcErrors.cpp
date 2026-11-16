// No Python twin: the tier1/rpc_errors.json adapters (conformance/python/scfg_conformance/tier1.py's
// _suggested_range and _named_version; unity/core-tests/Tier1.cs's RegisterRpcErrors).
#include "support/Tier1.h"

#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Rpc.h"
#include "Scfg/SolanaRpc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

namespace scfg_test::tier1::rpc_errors_adapters {

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

Result<int64_t> int_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_int64()) return Err::value_error("vector input " + std::string(key) + " must be an int64");
    return v.as_int64();
}

}  // namespace scfg_test::tier1::rpc_errors_adapters

SCFG_TIER1_TOPIC(rpc_errors) {
    namespace a = scfg_test::tier1::rpc_errors_adapters;
    // rpc.suggested_range(rpc.RpcError(-32005, i["message"]), i["tried"])
    fn("suggested_range", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string message, a::str_field(in, "message"));
        SCFG_TRY(Json tried, py::get(in, "tried"));
        if (!tried.is_int()) return Err::value_error("vector input tried must be an int");
        return Json(scfg::rpc::suggested_range(Err::rpc_error(-32005, message), tried.as_bigint()));
    });
    // SolanaRpc(None) with tx_version = i["tx_version"], then _named_version(rpc.RpcError(i["code"], i["message"]))
    fn("named_version", [](const Json& in) -> Result<Json> {
        SCFG_TRY(int64_t code, a::int_field(in, "code"));
        SCFG_TRY(std::string message, a::str_field(in, "message"));
        SCFG_TRY(int64_t tx_version, a::int_field(in, "tx_version"));
        std::optional<int64_t> v = scfg::solana::rpc::named_version(Err::rpc_error(code, message), tx_version);
        return v ? Json(*v) : Json();
    });
}
