// No Python twin: a finite Python decimal.Decimal under the default context (28 digits, round half even, Emax
// 999999, Emin -999999, Overflow and InvalidOperation trapped), limited to what the payment core does with one:
// Decimal(str | int | float), str(), +, comparisons, quantize, adjusted() and the exact digit shifts of
// amounts.py. Mirrors unity/.../Core/Dec.cs and rpgmaker/src/core/decimal.ts.
//
// The exponent is an int64_t and never clamped (Dec.cs clamped it at +-2^30): every exponent a literal can carry
// fits, since libmpdec refuses a literal whose exponent is below -1999999999999999997 or whose adjusted exponent is
// above 999999999999999999, so "0E-1000000000000000000" prints as Python prints it.
//
// The decimal signals are Errs of their own kinds (Err.cpp's table): InvalidOperation and Overflow are
// ArithmeticErrors, not ValueErrors, as in Python. Messages are the C decimal's ("[<class 'decimal.Overflow'>]").
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scfg {

// An immutable finite decimal (-1)^negative * coef * 10^exp, the coefficient a magnitude kept apart from the sign
// so that -0 exists. Equality and ordering are by value (1.0 == 1.00, -0 == 0), as Python's.
class SCFGCORE_API Dec {
public:
    // The default context.
    static constexpr int64_t PREC = 28;
    static constexpr int64_t EMAX = 999999;
    static constexpr int64_t EMIN = -999999;
    static constexpr int64_t ETINY = EMIN - PREC + 1;  // -1000026
    static constexpr int64_t ETOP = EMAX - PREC + 1;   // 999972

    // Decimal(0).
    Dec();
    // Decimal((negative, digits of coef, exp)). coef must be >= 0 (a negative one aborts: it is a magnitude).
    Dec(BigInt coef, int64_t exp, bool negative);

    // Decimal(n) for an int: exact.
    static Dec from_int(const BigInt& n);
    // Decimal(x) for a float: exact, no rounding (0.1 has 55 fraction digits), -0.0 is -0. nullopt for a NaN or an
    // infinity, which Python turns into a non-finite Decimal.
    static std::optional<Dec> from_double(double x);
    // Decimal(text): surrounding whitespace (str.strip()'s set) is ignored, every underscore is dropped and a
    // decimal digit of any script counts as its ASCII digit, as CPython's C decimal reads a str. nullopt for a NaN
    // (any payload) or an infinity. A malformed literal, or an exponent beyond libmpdec's limits, is an
    // InvalidOperation (an ArithmeticError: amounts.py turns it into its own ValueError).
    static Result<std::optional<Dec>> parse(std::string_view text);
    // Decimal(v) for a JSON value: a str as parse, an int exact, a bool 0 or 1, a float as from_double. Anything else
    // is a TypeError ("conversion from NoneType to Decimal is not supported"); a list too, where Python would read a
    // (sign, digits, exponent) tuple (the core never builds a Decimal from one).
    static Result<std::optional<Dec>> from_json(const Json& v);

    // The magnitude of the coefficient (>= 0).
    const BigInt& coef() const { return coef_; }
    int64_t exp() const { return exp_; }
    // The sign bit (true for -0 too).
    bool negative() const { return negative_; }
    bool is_zero() const { return coef_.is_zero(); }
    // The coefficient's number of decimal digits (1 for zero, as len(as_tuple().digits)).
    int64_t digit_count() const { return ndigits_; }
    // d.adjusted(): the exponent of the leading digit, exp + digits - 1 (a zero's is its exp).
    int64_t adjusted() const { return exp_ + ndigits_ - 1; }

    // str(d): plain notation when exp <= 0 and the adjusted exponent is at least -6 ("4.990000", "0.000000"),
    // otherwise scientific ("1E+1", "1E-7", "0E+3", "1.2E+999999999999999999").
    std::string str() const;

    // Decimal((sign, digits, exponent + n)): the same digits with the point moved, exact (no rounding to the
    // context, unlike Python's scaleb): amounts.py's units_to_decimal and decimal_to_units. The exponent must stay
    // within int64_t (it aborts otherwise; a parsed exponent is within +-2*10^18).
    Dec move_point(int64_t n) const;

    // d.quantize(Decimal(1).scaleb(exp)) under the default context: rounded half even to the exponent exp,
    // keeping the sign. InvalidOperation when exp is outside [ETINY, EMAX], when the value's adjusted exponent is
    // above EMAX, or when the result needs more than PREC digits.
    Result<Dec> quantize(int64_t exp) const;

    // int(d) when d is integral (Python's d == d.to_integral_value(), then int(d)), else nullopt. A zero is 0 whatever
    // its exponent. The caller bounds the magnitude first (amounts::decimal_to_units checks AMOUNT_EXPONENT_LIMIT): an
    // exponent above 100000 aborts rather than build a number of that many digits.
    std::optional<BigInt> integral_value() const;

    // a + b under the default context, exactly as _pydecimal's __add__: operands whose exponents are far apart cost
    // nothing, the sum is rounded half even to 28 digits (as a subnormal at ETINY), a zero's exponent is clamped
    // into [ETINY, EMAX], and a result above EMAX is an Overflow (the trapped decimal.Overflow).
    friend SCFGCORE_API Result<Dec> operator+(const Dec& a, const Dec& b);

    friend SCFGCORE_API bool operator==(const Dec& a, const Dec& b);
    friend SCFGCORE_API std::weak_ordering operator<=>(const Dec& a, const Dec& b);

private:
    BigInt coef_;
    int64_t exp_ = 0;
    bool negative_ = false;
    int64_t ndigits_ = 1;
};

}  // namespace scfg
