// No Python twin: the tier1/ed25519.json adapter (conformance/python/scfg_conformance/tier1.py's is_on_curve;
// unity/core-tests/Tier1.cs).
#include "support/Tier1.h"

#include "Scfg/Bytes.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaEd25519.h"

#include <cstdint>
#include <string>
#include <vector>

using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(ed25519) {
    fn("is_on_curve", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json hex, py::get(in, "point_hex"));
        if (!hex.is_str()) return scfg::Err::type_error("fromhex() argument must be str");
        SCFG_TRY(std::vector<uint8_t> point, scfg::bytes::from_hex(hex.as_str()));
        return Json(scfg::solana::ed25519::is_on_curve(point));
    });
}
