// Twin of scfg_core/amounts.py.
#include "Scfg/Amounts.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <optional>
#include <utility>

namespace scfg::amounts {

namespace detail {

// _CENT = Decimal("0.01"): its exponent.
constexpr int64_t CENT_EXP = -2;

bool in_range(const Dec& d) {
    return d.is_zero() || (-AMOUNT_EXPONENT_LIMIT <= d.adjusted() && d.adjusted() <= AMOUNT_EXPONENT_LIMIT);
}

std::string limit_text() { return "amount text longer than 100 characters"; }

// The checks parse_amount makes once it holds a Decimal (nullopt: a non-finite one).
Result<Dec> checked(const std::optional<Dec>& value) {
    if (!value) return Err::value_error("amount must be finite");
    const Dec& d = *value;
    if (!in_range(d)) return Err::value_error("amount out of range");
    if (d <= Dec()) return Err::value_error("amount must be positive");
    if (d > MAX_AMOUNT) return Err::value_error("amount too large");
    SCFG_TRY(Dec q, d.quantize(CENT_EXP));
    if (q != d) return Err::value_error("amount must have at most 2 decimals");
    return q;
}

Result<int64_t> check_field(const char* name, const Json& value) {
    if (value.is_int64() && value.as_int64() >= 0 && value.as_int64() <= 99) return value.as_int64();
    return Err::value_error(std::string(name) + " must be an int in 0..99");
}

}  // namespace detail

Result<Dec> stored_amount(const Json& value) {
    if (value.is_list()) {  // Decimal() reads a (sign, digits, exponent) tuple
        return Err::type_error("a stored amount is a number, not a list");
    }
    if (value.is_str() && py::len(value.as_str()) > AMOUNT_TEXT_LIMIT) return Err::value_error(detail::limit_text());
    Result<std::optional<Dec>> d = Dec::from_json(value);
    if (!d.ok()) {
        if (d.err().is_a("InvalidOperation")) return Err::value_error("not a number: " + py::repr(value));
        return std::move(d).err();  // the TypeError of a value Decimal() does not take
    }
    if (!d.value() || !detail::in_range(*d.value())) return Err::value_error("amount out of range: " + py::repr(value));
    return *d.value();
}

Result<Dec> parse_amount(const Dec& value) { return detail::checked(value); }

Result<Dec> parse_amount(const Json& value) {
    switch (value.type()) {
        case Json::Type::Bool:
        case Json::Type::Float:
            return Err::type_error("amounts must be str, int or Decimal (never float)");
        case Json::Type::Int:
            return detail::checked(Dec::from_int(value.as_bigint()));
        case Json::Type::Str: {
            const std::string& s = value.as_str();
            if (py::len(s) > AMOUNT_TEXT_LIMIT) return Err::value_error(detail::limit_text());
            if (s.find('_') != std::string::npos) return Err::value_error("not a number: " + py::repr_str(s));
            Result<std::optional<Dec>> d = Dec::parse(py::strip(s));
            if (!d.ok()) return Err::value_error("not a number: " + py::repr_str(s));
            return detail::checked(d.value());
        }
        default:
            return Err::type_error("unsupported amount type: " + std::string(value.type_name()));
    }
}

Result<BigInt> encode(const Dec& price, const Json& code, const Json& nonce) {
    SCFG_TRY(Dec p, parse_amount(price));
    SCFG_TRY(int64_t c, detail::check_field("code", code));
    SCFG_TRY(int64_t n, detail::check_field("nonce", nonce));
    // p has exactly 2 decimals and at most 9 digits, so int(p * 100) is its cents, exactly.
    BigInt cents = *p.move_point(2).integral_value();
    return cents * 10000 + BigInt(c * 100 + n);
}

Result<BigInt> encode(const Json& price, const Json& code, const Json& nonce) {
    SCFG_TRY(Dec p, parse_amount(price));
    return encode(p, code, nonce);
}

Result<Decoded> decode(const BigInt& units) {
    if (units.sign() < 0) return Err::value_error("units must be >= 0");
    BigInt cents = units.floordiv(10000).value();
    // (Decimal(units // 10_000) / 100).quantize(_CENT): the division is exact while the cents fit in 28 digits; from
    // 10**28 cents it rounds to 28 digits and quantize then needs 29: InvalidOperation.
    if (cents >= BigInt(10).pow(Dec::PREC)) return Err::make("InvalidOperation", "[<class 'decimal.InvalidOperation'>]");
    Decoded d;
    d.price = Dec(std::move(cents), detail::CENT_EXP, false);
    SCFG_TRY(int64_t code, units.floordiv(100).value().mod(100).value().to_int64());
    SCFG_TRY(int64_t nonce, units.mod(100).value().to_int64());
    d.code = code;
    d.nonce = nonce;
    return d;
}

Dec units_to_decimal(const BigInt& units) { return Dec::from_int(units).move_point(-TOKEN_DECIMALS); }

Result<BigInt> decimal_to_units(const Dec& amount) {
    if (!detail::in_range(amount)) return Err::value_error("amount out of range: " + amount.str());
    std::optional<BigInt> q = amount.move_point(TOKEN_DECIMALS).integral_value();
    if (!q) return Err::value_error("more than 6 decimals");
    return *q;
}

// units_to_decimal's exponent is -6 and its adjusted exponent at least -6, so str() is already the plain form
// f"{d:.6f}" gives, sign and leading "0." included.
std::string format_units(const BigInt& units) { return units_to_decimal(units).str(); }

}  // namespace scfg::amounts
