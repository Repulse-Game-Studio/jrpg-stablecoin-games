// No Python twin: the tier1/keccak.json adapter (conformance/python/scfg_conformance/tier1.py's _keccak256;
// unity/core-tests/Tier1.cs's RegisterKeccak).
#include "support/Tier1.h"

#include "Scfg/Bytes.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Keccak.h"
#include "Scfg/Py.h"

#include <cstdint>
#include <string>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(keccak) {
    // keccak256(bytes.fromhex(i["data_hex"])).hex(); the vectors write plain hex (no spaces, which fromhex skips)
    fn("keccak256", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json data_hex, py::get(in, "data_hex"));
        if (!data_hex.is_str()) {
            return Err::type_error("fromhex() argument must be str, not " + std::string(data_hex.type_name()));
        }
        SCFG_TRY(std::vector<uint8_t> data, scfg::bytes::from_hex(data_hex.as_str()));
        return Json(scfg::bytes::to_hex(scfg::keccak::keccak256(data)));
    });
}
