// No Python twin: the tier1/evm_address.json adapters (conformance/python/scfg_conformance/tier1.py's evm block;
// unity/core-tests/Tier1.cs's RegisterEvm). The address and the log are passed through untouched, as tier1.py
// does: a non-str address or a non-dict log is the core's own error.
#include "support/Tier1.h"

#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>
#include <string>

using scfg::Json;
using scfg::Result;
namespace evm = scfg::evm;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(evm_address) {
    fn("to_checksum_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json address, py::get(in, "address"));
        SCFG_TRY(std::string out, evm::to_checksum_address(address));
        return Json(out);
    });
    fn("is_checksum_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json address, py::get(in, "address"));
        return Json(evm::is_checksum_address(address));
    });
    fn("normalize_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json address, py::get(in, "address"));
        SCFG_TRY(std::string out, evm::normalize_address(address));
        return Json(out);
    });
    fn("address_topic", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json address, py::get(in, "address"));
        SCFG_TRY(std::string out, evm::address_topic(address));
        return Json(out);
    });
    // The TransferLog as a dict, its value written as a decimal string (str(t.value)).
    fn("parse_transfer_log", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json log, py::get(in, "log"));
        SCFG_TRY(std::optional<evm::TransferLog> t, evm::parse_transfer_log(log));
        if (!t) return Json();
        return Json::dict({{"token", t->token},
                           {"sender", t->sender},
                           {"recipient", t->recipient},
                           {"value", t->value.to_decimal()},
                           {"tx_hash", t->tx_hash},
                           {"log_index", t->log_index},
                           {"block_number", t->block_number},
                           {"block_hash", t->block_hash}});
    });
}
