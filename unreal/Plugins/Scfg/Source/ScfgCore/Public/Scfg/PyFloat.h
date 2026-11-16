// No Python twin: Python's float repr() and float() of a literal, the same under every C locale.
//
// Both rest on the C library (snprintf's exact %e digits, strtod's correct rounding) and never on its locale: the
// text handed to strtod and read from snprintf carries no decimal point at all ("25e-1" for 2.5), so a locale's
// decimal comma cannot change a result.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace scfg::pyfloat {

// repr(x) (float_repr_style 'short'): the shortest digits that round-trip (the closest such when several), written
// positionally when the decimal exponent is in [-4, 16) with at least one fractional digit (1.0, 0.0001,
// 1000000000000000.0), else as d.ddde+XX with at least two exponent digits (1e+16, 1e-05); "inf", "-inf", "nan",
// "-0.0".
std::string repr(double x);

// float(s) for a float literal: an optional sign, then digits with an optional '.' and fraction (or '.' and
// digits), then an optional exponent (e or E, an optional sign, digits); or inf, infinity or nan in any case after
// an optional sign. Correctly rounded; out of range is inf or 0.0, as float() makes it. Anything else is nullopt,
// checked before conversion: no hex floats and no locale forms ("2,5"). Stricter than float(), which also strips
// whitespace and takes underscores and non-ASCII digits.
std::optional<double> parse(std::string_view s);

}  // namespace scfg::pyfloat
