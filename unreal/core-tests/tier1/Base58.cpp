// No Python twin: the tier1/base58.json adapters (conformance/python/scfg_conformance/tier1.py's base58 block;
// unity/core-tests/Tier1.cs). Text and length are passed through untouched, as tier1.py does; bytes travel as hex.
#include "support/Tier1.h"

#include "Scfg/Bytes.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaBase58.h"

#include <cstdint>
#include <string>
#include <vector>

using scfg::Json;
using scfg::Result;
namespace base58 = scfg::solana::base58;
namespace py = scfg::py;

namespace scfg_test::tier1::base58_adapters {

// bytes.fromhex(i[key]).
Result<std::vector<uint8_t>> hex_bytes(const Json& in, std::string_view key) {
    SCFG_TRY(Json v, py::get(in, key));
    if (!v.is_str()) return scfg::Err::type_error("fromhex() argument must be str, not " + std::string(v.type_name()));
    return scfg::bytes::from_hex(v.as_str());
}

}  // namespace scfg_test::tier1::base58_adapters

SCFG_TIER1_TOPIC(base58) {
    namespace a = scfg_test::tier1::base58_adapters;
    fn("b58encode", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::vector<uint8_t> data, a::hex_bytes(in, "data_hex"));
        return Json(base58::b58encode(data));
    });
    fn("b58decode", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(std::vector<uint8_t> raw, base58::b58decode(text));
        return Json(scfg::bytes::to_hex(raw));
    });
    fn("decode_fixed", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(Json length, py::get(in, "length"));
        SCFG_TRY(std::vector<uint8_t> raw, base58::decode_fixed(text, length));
        return Json(scfg::bytes::to_hex(raw));
    });
    fn("normalize_pubkey", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(std::string out, base58::normalize_pubkey(text));
        return Json(out);
    });
    fn("normalize_signature", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(std::string out, base58::normalize_signature(text));
        return Json(out);
    });
}
