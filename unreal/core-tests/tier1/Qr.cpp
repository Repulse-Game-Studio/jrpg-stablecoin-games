// No Python twin: the tier1/qr.json adapters (conformance/python/scfg_conformance/tier1.py's qr_matrix and qr_rects;
// unity/core-tests/Tier1.cs). A matrix travels as one "0"/"1" string per row; qr_rects as {"size", "rects"}.
#include "support/Tier1.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Paylinks.h"
#include "Scfg/Py.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace paylinks = scfg::paylinks;
namespace py = scfg::py;

namespace scfg_test::tier1::qr_adapters {

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

// An int as Python's arithmetic takes it (a bool is 0 or 1).
Result<int64_t> int_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (v.is_bool()) return int64_t{v.as_bool() ? 1 : 0};
    if (!v.is_int64()) return Err::value_error("vector input " + std::string(key) + " must be an int");
    return v.as_int64();
}

}  // namespace scfg_test::tier1::qr_adapters

SCFG_TIER1_TOPIC(qr) {
    namespace a = scfg_test::tier1::qr_adapters;
    fn("qr_matrix", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string text, a::str_field(in, "text"));
        SCFG_TRY(std::vector<std::vector<bool>> matrix, paylinks::qr_matrix(text));
        Json rows = Json::list();
        for (const std::vector<bool>& row : matrix) {
            std::string line;
            for (bool m : row) line += m ? '1' : '0';
            rows.as_list().push_back(Json(std::move(line)));
        }
        return rows;
    });
    fn("qr_rects", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::string text, a::str_field(in, "text"));
        SCFG_TRY(int64_t border, a::int_field(in, "border"));
        SCFG_TRY(paylinks::QrRects r, paylinks::qr_rects(text, border));
        Json rects = Json::list();
        for (const auto& [x, y, w] : r.runs) rects.as_list().push_back(Json::list({x, y, w}));
        return Json::dict({{"size", r.size}, {"rects", std::move(rects)}});
    });
}
