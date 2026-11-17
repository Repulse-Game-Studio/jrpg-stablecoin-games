// No Python twin: the tier1/amounts.json adapters (conformance/python/scfg_conformance/tier1.py's "# amounts"
// block; unity/core-tests/Tier1.cs's RegisterAmounts).
#include "support/Tier1.h"

#include "Scfg/Amounts.h"
#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>

using scfg::BigInt;
using scfg::Dec;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace amounts = scfg::amounts;
namespace py = scfg::py;

namespace scfg_test::tier1::amounts_adapters {

// int(i[key]): units arrive as decimal integer strings.
Result<BigInt> units(const Json& in) {
    SCFG_TRY(Json v, py::get(in, "units"));
    return py::py_int(v);
}

}  // namespace scfg_test::tier1::amounts_adapters

SCFG_TIER1_TOPIC(amounts) {
    namespace a = scfg_test::tier1::amounts_adapters;
    fn("parse_amount", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json kind, py::get(in, "type"));
        Json value;
        if (kind.is_str() && kind.as_str() == "float") {
            value = Json(4.99);  // a C++ double, as tier1.py's 4.99
        } else if (kind.is_str() && kind.as_str() == "bool") {
            SCFG_TRY(Json v, py::get(in, "value"));
            value = Json(py::truthy(v));
        } else {  // "str" or "int": the JSON value itself
            SCFG_TRY(value, py::get(in, "value"));
        }
        SCFG_TRY(Dec d, amounts::parse_amount(value));
        return Json(d.str());
    });
    fn("encode", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json price, py::get(in, "price"));
        SCFG_TRY(Json code, py::get(in, "code"));
        SCFG_TRY(Json nonce, py::get(in, "nonce"));
        SCFG_TRY(BigInt u, amounts::encode(price, code, nonce));
        return Json(u.to_decimal());
    });
    fn("decode", [](const Json& in) -> Result<Json> {
        SCFG_TRY(BigInt u, a::units(in));
        SCFG_TRY(amounts::Decoded d, amounts::decode(u));
        return Json::dict({{"price", d.price.str()}, {"code", d.code}, {"nonce", d.nonce}});
    });
    fn("units_to_decimal", [](const Json& in) -> Result<Json> {
        SCFG_TRY(BigInt u, a::units(in));
        return Json(amounts::units_to_decimal(u).str());
    });
    fn("decimal_to_units", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json amount, py::get(in, "amount"));
        SCFG_TRY(std::optional<Dec> d, Dec::from_json(amount));  // Decimal(i["amount"])
        // decimal_to_units refuses a non-finite Decimal as out of range; a Dec is always finite
        if (!d) {
            SCFG_TRY(std::string shown, py::repr(amount));
            return Err::value_error("amount out of range: " + shown);
        }
        SCFG_TRY(BigInt u, amounts::decimal_to_units(*d));
        return Json(u.to_decimal());
    });
    fn("stored_amount", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json raw, py::get(in, "value"));
        SCFG_TRY(Json value, scfg_test::tier1::decode_value(raw));
        SCFG_TRY(Dec d, amounts::stored_amount(value));
        return Json(d.str());
    });
    fn("format_units", [](const Json& in) -> Result<Json> {
        SCFG_TRY(BigInt u, a::units(in));
        return Json(amounts::format_units(u));
    });
}
