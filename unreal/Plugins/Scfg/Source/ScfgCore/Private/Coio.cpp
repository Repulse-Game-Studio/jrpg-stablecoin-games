// Twin of scfg_core/coio.py (response_from_envelope) and of rpc.UrllibTransport's answer rules (parse_rpc_body).
#include "Scfg/Coio.h"

#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Ints.h"
#include "Scfg/Py.h"

#include <limits>
#include <optional>
#include <string>

namespace scfg::coio::detail {

// The status check both readers share. readable: the body parsed (as body).
Result<Json> response_for(const BigInt& status, bool readable, Json body) {
    if (status < 200 || status >= 300) {
        if (readable && body.is_dict() && py::truthy(body.as_dict().get("error"))) return body;
        std::optional<int> code;
        if (status >= std::numeric_limits<int>::min() && status <= std::numeric_limits<int>::max())
            code = static_cast<int>(status.to_int64().value());
        return Err::transport_error("HTTP " + status.to_decimal(), code);
    }
    if (!readable) return Err::transport_error(std::string(INVALID_JSON), std::nullopt);
    return body;
}

}  // namespace scfg::coio::detail

namespace scfg::coio {

Result<Json> parse_rpc_body(int status, std::string_view body) {
    if (body.size() > MAX_RESPONSE_BYTES) return Err::transport_error(std::string(RESPONSE_TOO_LARGE), std::nullopt);
    // Python: json_loads(raw.decode("utf-8")); a 2xx catches ValueError (UnicodeDecodeError is one), the error
    // path catches every Exception. json::loads reports both as ValueError.
    Result<Json> parsed = json::loads(body);
    bool readable = parsed.ok();
    return detail::response_for(BigInt(status), readable, readable ? parsed.take() : Json());
}

Result<Json> response_from_envelope(const Json::Dict& envelope) {
    if (const Json* error = envelope.find("error")) return Err::transport_error(py::str(*error), std::nullopt);
    Json raw_status = envelope.get("status");
    SCFG_TRY(BigInt status, ints::int_from(py::truthy(raw_status) ? raw_status : Json(0)));
    Json raw_body = envelope.get("body");
    Json text = py::truthy(raw_body) ? raw_body : Json("");
    if (!text.is_str()) {
        return Err::type_error("the JSON object must be str, bytes or bytearray, not " + std::string(text.type_name()));
    }
    // json_loads(text) with `except ValueError`: every error json::loads_text gives is one.
    Result<Json> parsed = json::loads_text(text.as_str());
    bool readable = parsed.ok();
    return detail::response_for(status, readable, readable ? parsed.take() : Json());
}

}  // namespace scfg::coio
