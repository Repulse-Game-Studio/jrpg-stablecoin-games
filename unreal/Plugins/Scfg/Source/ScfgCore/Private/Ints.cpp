// Twin of scfg_core/ints.py: int() with the digit limit, json_loads and digit_run_value (see Ints.h).
#include "Scfg/Ints.h"

#include "Scfg/Py.h"

#include <string>

namespace scfg::ints::detail {

Err too_many_digits(size_t digits) {
    return Err::value_error("Exceeds the limit (4300 digits) for integer string conversion: value has " +
                            py::dec(digits) + " digits; use sys.set_int_max_str_digits() to increase the limit");
}

}  // namespace scfg::ints::detail

namespace scfg::ints {

Result<BigInt> int_from(const Json& value) {
    if (value.is_str() && py::len(value.as_str()) > INT_MAX_STR_DIGITS) {
        const std::string& s = value.as_str();
        size_t digits = 0;
        for (size_t i = 0; i < s.size();) digits += py::is_decimal_digit(py::utf8_decode(s, i)) ? 1 : 0;
        if (digits > INT_MAX_STR_DIGITS) return detail::too_many_digits(digits);
    }
    return py::py_int(value);
}

Result<std::optional<int64_t>> digit_run_value(std::string_view run) {
    if (py::len(run) > DIGIT_RUN_LIMIT) {
        // Every decimal digit is its script's zero plus its value (Nd digits come in runs of ten): strip the leading
        // ones whose value is 0. Python builds that set with int(c) for every character, so a non-digit is int()'s
        // ValueError.
        size_t start = run.size();
        for (size_t i = 0; i < run.size();) {
            size_t at = i;
            int d = py::decimal_value(py::utf8_decode(run, i));
            if (d < 0) {
                SCFG_TRY_VOID(py::py_int(run.substr(at, i - at)));
            }
            if (d != 0 && start == run.size()) start = at;
        }
        run = run.substr(start);
        if (py::len(run) > DIGIT_RUN_LIMIT) return std::optional<int64_t>();
    }
    if (run.empty()) return std::optional<int64_t>(0);
    SCFG_TRY(BigInt v, py::py_int(run));
    SCFG_TRY(int64_t n, v.to_int64());
    return std::optional<int64_t>(n);
}

Result<Json> json_loads(std::string_view text) { return json::loads(text); }

}  // namespace scfg::ints
