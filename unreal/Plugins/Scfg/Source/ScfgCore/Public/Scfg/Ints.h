// Twin of scfg_core/ints.py: Python's int() for text the core reads from storage, a page or an RPC, with CPython's
// limit on the number of decimal digits enforced explicitly (converting a very long digit string costs quadratic
// time); json_loads (an RPC answer's integer literals through int_from) and digit_run_value (a number in a node's
// error message, at most 18 significant digits, Ruling 6).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace scfg::ints {

// CPython's default sys.get_int_max_str_digits().
inline constexpr size_t INT_MAX_STR_DIGITS = 4300;
// A digit run read out of a node's error message (an EVM range limit, a Solana transaction version) is a number
// only up to this many significant digits (Ruling 6): every range or version a request can carry is below 10**18.
inline constexpr size_t DIGIT_RUN_LIMIT = 18;

// int(value) (py::py_int), refusing a str with more than INT_MAX_STR_DIGITS decimal characters as CPython does
// (ValueError, with ints.py's message) before any conversion.
Result<BigInt> int_from(const Json& value);

// int(run) for a run of Unicode decimal digits (what re's \d+ matches), or nullopt when it has more than
// DIGIT_RUN_LIMIT significant digits. Linear: leading zeros (of any script) are stripped first, so no long text is
// converted. A character that is not a decimal digit is int()'s ValueError.
Result<std::optional<int64_t>> digit_run_value(std::string_view run);

// json.loads(text), its integer literals read with int_from: one of more than INT_MAX_STR_DIGITS digits is a
// ValueError. Fractions and exponents are floats, as json makes them. This is json::loads.
Result<Json> json_loads(std::string_view text);

namespace detail {

// ints.py's ValueError for a str with `digits` decimal characters.
Err too_many_digits(size_t digits);

}  // namespace detail
}  // namespace scfg::ints
