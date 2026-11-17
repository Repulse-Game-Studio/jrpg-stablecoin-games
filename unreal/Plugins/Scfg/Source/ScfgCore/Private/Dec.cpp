// No Python twin: Python's decimal.Decimal under the default context (see Dec.h). The arithmetic follows
// _pydecimal (Decimal.__add__, _normalize, _fix, quantize, _rescale, __str__); parse follows the C decimal's
// numeric_as_ascii and libmpdec's literal limits.
#include "Scfg/Dec.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace scfg {

namespace dec_detail {

// libmpdec's limits for a literal: its exponent, and its adjusted exponent.
constexpr int64_t MIN_LITERAL_EXP = -1999999999999999997;
constexpr int64_t MAX_LITERAL_ADJUSTED = 999999999999999999;
// Every exponent a Dec holds lies within +-EXP_BOUND, so exp + digits never overflows an int64_t.
constexpr int64_t EXP_BOUND = 4000000000000000000;
// integral_value refuses to build an integer with more zeros than this (a caller bug, not an input).
constexpr int64_t INTEGRAL_EXP_LIMIT = 100000;

Err conversion_syntax() { return Err::make("InvalidOperation", "[<class 'decimal.ConversionSyntax'>]"); }
Err invalid_operation() { return Err::make("InvalidOperation", "[<class 'decimal.InvalidOperation'>]"); }
Err overflow() { return Err::make("Overflow", "[<class 'decimal.Overflow'>]"); }

BigInt ten_pow(int64_t n) { return BigInt(10).pow(static_cast<uint64_t>(n)); }

// The coefficient's digits, through BigInt::to_decimal: Decimal has no digit limit, and Python's Decimal(int) is
// quadratic in the size of the int too (CPython 3.9: 0.25 s for 200 000 bits, 1 s for 400 000). Every coefficient
// the core builds is bounded where it is made: amount text (AMOUNT_TEXT_LIMIT), a JSON int (the reader's 4300
// digits), a float (at most 767 digits), a token value (a 32-byte log word, or a Solana amount read through
// int_from).
int64_t count_digits(const BigInt& magnitude) { return static_cast<int64_t>(magnitude.to_decimal().size()); }

// coef / 10^drop, rounded half to even (drop >= 1).
BigInt round_drop(const BigInt& coef, int64_t drop) {
    BigInt p = ten_pow(drop);
    Result<BigInt::DivMod> qr = coef.divmod(p);  // p > 0: cannot fail
    BigInt q = qr.value().quot;
    BigInt twice = qr.value().rem * 2;
    bool odd = q.mod(2).value() == BigInt(1);
    if (twice > p || (twice == p && odd)) q += 1;
    return q;
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ascii_ieq(std::string_view s, std::string_view lower) {
    if (s.size() != lower.size()) return false;
    for (size_t k = 0; k < s.size(); ++k) {
        if (ascii_lower(s[k]) != lower[k]) return false;
    }
    return true;
}

bool all_digits(std::string_view s) {
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// inf, infinity, nan, snan, a NaN with payload digits, ASCII case-insensitively (never the locale's casing).
bool is_special(std::string_view s) {
    if (ascii_ieq(s, "inf") || ascii_ieq(s, "infinity")) return true;
    if (s.size() >= 3 && ascii_ieq(s.substr(0, 3), "nan")) return all_digits(s.substr(3));
    if (s.size() >= 4 && ascii_ieq(s.substr(0, 4), "snan")) return all_digits(s.substr(4));
    return false;
}

// The C decimal's numeric_as_ascii(u, strip_ws=1, ignore_underscores=1): surrounding whitespace stripped, every
// underscore dropped, a non-ASCII space kept as ' ' (so it is refused below) and a decimal digit of any script made
// ASCII. nullopt for any other non-ASCII code point or a NUL, which no literal contains.
std::optional<std::string> as_ascii(std::string_view text) {
    std::string s = py::strip(text);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = py::utf8_decode(s, i);
        if (cp == '_') continue;
        if (cp > 0 && cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp >= 0x80 && py::is_space(cp)) {
            out.push_back(' ');
        } else {
            int d = py::decimal_value(cp);
            if (d < 0) return std::nullopt;
            out.push_back(static_cast<char>('0' + d));
        }
    }
    return out;
}

// _pydecimal's Decimal._fix under the default context, for the finite (negative, coef, exp).
Result<Dec> fix(BigInt coef, int64_t exp, bool negative) {
    if (coef.is_zero()) return Dec(BigInt(), std::clamp(exp, Dec::ETINY, Dec::EMAX), negative);
    int64_t digits = count_digits(coef);
    int64_t exp_min = digits + exp - Dec::PREC;
    if (exp_min > Dec::ETOP) return overflow();
    if (exp_min < Dec::ETINY) exp_min = Dec::ETINY;  // subnormal
    if (exp >= exp_min) return Dec(std::move(coef), exp, negative);
    int64_t keep = digits + exp - exp_min;  // < digits
    // keep < 0: the value is below half of the last place and rounds to zero (Python's sticky '1').
    BigInt c = keep < 0 ? BigInt() : round_drop(coef, digits - keep);
    if (count_digits(c) > Dec::PREC) {  // the rounding carried into a 29th digit, which is a 0
        c = c.floordiv(10).value();
        exp_min += 1;
    }
    if (exp_min > Dec::ETOP) return overflow();
    return Dec(std::move(c), exp_min, negative);
}

// -1, 0 or 1 by value; zeros of either sign are equal.
int compare(const Dec& a, const Dec& b) {
    int sa = a.is_zero() ? 0 : (a.negative() ? -1 : 1);
    int sb = b.is_zero() ? 0 : (b.negative() ? -1 : 1);
    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    int64_t adj_a = a.adjusted();
    int64_t adj_b = b.adjusted();
    int mag;
    if (adj_a != adj_b) {
        mag = adj_a > adj_b ? 1 : -1;
    } else {
        // The same leading-digit position, so the exponents differ by less than the digit counts.
        int64_t e = std::min(a.exp(), b.exp());
        BigInt x = a.coef() * ten_pow(a.exp() - e);
        BigInt y = b.coef() * ten_pow(b.exp() - e);
        mag = x == y ? 0 : (x > y ? 1 : -1);
    }
    return mag * sa;
}

}  // namespace dec_detail

Dec::Dec() = default;

Dec::Dec(BigInt coef, int64_t exp, bool negative) : coef_(std::move(coef)), exp_(exp), negative_(negative) {
    if (coef_.sign() < 0) detail::bad_result_access("Dec(coef, exp, negative) with a negative coefficient", nullptr);
    if (exp_ > dec_detail::EXP_BOUND || exp_ < -dec_detail::EXP_BOUND) {
        detail::bad_result_access("Dec(coef, exp, negative) with an exponent beyond +-4E+18", nullptr);
    }
    ndigits_ = dec_detail::count_digits(coef_);
}

Dec Dec::from_int(const BigInt& n) { return Dec(n.abs(), 0, n.sign() < 0); }

std::optional<Dec> Dec::from_double(double x) {
    if (std::isnan(x) || std::isinf(x)) return std::nullopt;
    uint64_t bits = std::bit_cast<uint64_t>(x);
    bool negative = (bits >> 63) != 0;
    int64_t biased = static_cast<int64_t>((bits >> 52) & 0x7ff);
    uint64_t mant = bits & ((uint64_t(1) << 52) - 1);
    int64_t e2 = biased == 0 ? -1074 : biased - 1075;
    if (biased != 0) mant |= uint64_t(1) << 52;
    if (mant == 0) return Dec(BigInt(), 0, negative);
    while ((mant & 1) == 0 && e2 < 0) {  // lowest terms: an odd numerator over 2^k
        mant >>= 1;
        ++e2;
    }
    if (e2 >= 0) return Dec(BigInt::from_uint64(mant) << static_cast<size_t>(e2), 0, negative);
    // mant * 2^e2 == mant * 5^-e2 * 10^e2
    return Dec(BigInt::from_uint64(mant) * BigInt(5).pow(static_cast<uint64_t>(-e2)), e2, negative);
}

Result<std::optional<Dec>> Dec::parse(std::string_view text) {
    std::optional<std::string> ascii = dec_detail::as_ascii(text);
    if (!ascii) return dec_detail::conversion_syntax();
    const std::string& s = *ascii;
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    if (dec_detail::is_special(std::string_view(s).substr(i))) return std::optional<Dec>();

    size_t int_start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    std::string digits = s.substr(int_start, i - int_start);
    size_t frac_len = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        size_t frac_start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        frac_len = i - frac_start;
        digits += s.substr(frac_start, frac_len);
    }
    if (digits.empty()) return dec_detail::conversion_syntax();

    int64_t e = 0;
    bool out_of_range = false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool e_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            e_negative = s[i] == '-';
            ++i;
        }
        size_t e_start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == e_start) return dec_detail::conversion_syntax();
        size_t sig = e_start;
        while (sig < i - 1 && s[sig] == '0') ++sig;  // leading zeros cost nothing
        if (i - sig > 19) {
            out_of_range = true;  // at least 10^19: beyond libmpdec's limits whatever the digits around it
        } else {
            uint64_t mag = 0;
            for (size_t k = sig; k < i; ++k) mag = mag * 10 + static_cast<uint64_t>(s[k] - '0');
            if (mag > static_cast<uint64_t>(dec_detail::EXP_BOUND)) {
                out_of_range = true;
            } else {
                e = e_negative ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
            }
        }
    }
    if (i != s.size()) return dec_detail::conversion_syntax();
    if (out_of_range) return dec_detail::invalid_operation();

    size_t lead = 0;
    while (lead < digits.size() && digits[lead] == '0') ++lead;
    int64_t coef_digits = lead == digits.size() ? 1 : static_cast<int64_t>(digits.size() - lead);
    int64_t exp = e - static_cast<int64_t>(frac_len);
    if (exp < dec_detail::MIN_LITERAL_EXP || exp + coef_digits - 1 > dec_detail::MAX_LITERAL_ADJUSTED) {
        return dec_detail::invalid_operation();
    }
    Result<BigInt> coef = BigInt::from_decimal(std::string_view(digits).substr(lead == digits.size() ? lead - 1 : lead));
    if (!coef.ok()) return dec_detail::conversion_syntax();
    return std::optional<Dec>(Dec(coef.take(), exp, negative));
}

Result<std::optional<Dec>> Dec::from_json(const Json& v) {
    switch (v.type()) {
        case Json::Type::Str:
            return parse(v.as_str());
        case Json::Type::Int:
            return std::optional<Dec>(from_int(v.as_bigint()));
        case Json::Type::Bool:
            return std::optional<Dec>(from_int(v.as_bool() ? 1 : 0));
        case Json::Type::Float:
            return from_double(v.as_float());
        default:
            return Err::type_error("conversion from " + std::string(v.type_name()) + " to Decimal is not supported");
    }
}

std::string Dec::str() const {
    std::string digits = coef_.to_decimal();  // bounded as count_digits says
    int64_t n = static_cast<int64_t>(digits.size());
    int64_t leftdigits = exp_ + n;
    int64_t dotplace = exp_ <= 0 && leftdigits > -6 ? leftdigits : 1;
    std::string out;
    if (negative_) out.push_back('-');
    if (dotplace <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-dotplace), '0');
        out += digits;
    } else if (dotplace >= n) {
        out += digits;
        out.append(static_cast<size_t>(dotplace - n), '0');
    } else {
        out.append(digits, 0, static_cast<size_t>(dotplace));
        out.push_back('.');
        out.append(digits, static_cast<size_t>(dotplace), std::string::npos);
    }
    int64_t e = leftdigits - dotplace;
    if (e != 0) {
        out.push_back('E');
        out.push_back(e > 0 ? '+' : '-');
        out += py::dec(e > 0 ? e : -e);
    }
    return out;
}

Dec Dec::move_point(int64_t n) const {
    if ((n > 0 && exp_ > dec_detail::EXP_BOUND - n) || (n < 0 && exp_ < -dec_detail::EXP_BOUND - n)) {
        detail::bad_result_access("Dec::move_point beyond the exponent bound", nullptr);
    }
    return Dec(coef_, exp_ + n, negative_);
}

Result<Dec> Dec::quantize(int64_t exp) const {
    if (exp < ETINY || exp > EMAX) return dec_detail::invalid_operation();  // target exponent out of bounds
    if (is_zero()) return Dec(BigInt(), exp, negative_);
    if (adjusted() > EMAX) return dec_detail::invalid_operation();
    if (adjusted() - exp + 1 > PREC) return dec_detail::invalid_operation();  // too many digits
    BigInt c;
    int64_t shift = exp_ - exp;
    if (shift >= 0) {
        c = coef_ * dec_detail::ten_pow(shift);  // at most PREC digits, by the check above
    } else if (-shift > ndigits_) {
        c = BigInt();  // below half of the last place: rounds to zero
    } else {
        c = dec_detail::round_drop(coef_, -shift);
    }
    Dec result(std::move(c), exp, negative_);
    if (result.adjusted() > EMAX || result.digit_count() > PREC) return dec_detail::invalid_operation();
    return result;
}

std::optional<BigInt> Dec::integral_value() const {
    if (is_zero()) return BigInt();
    BigInt mag;
    if (exp_ >= 0) {
        if (exp_ > dec_detail::INTEGRAL_EXP_LIMIT) {
            detail::bad_result_access("Dec::integral_value of a value beyond 1E+100000 (bound it first)", nullptr);
        }
        mag = coef_ * dec_detail::ten_pow(exp_);
    } else {
        if (-exp_ > ndigits_) return std::nullopt;  // below 1 and not zero
        Result<BigInt::DivMod> qr = coef_.divmod(dec_detail::ten_pow(-exp_));
        if (!qr.value().rem.is_zero()) return std::nullopt;
        mag = qr.value().quot;
    }
    return negative_ ? -mag : mag;
}

Result<Dec> operator+(const Dec& a, const Dec& b) {
    int64_t exp = std::min(a.exp_, b.exp_);
    if (a.is_zero() && b.is_zero()) return dec_detail::fix(BigInt(), exp, a.negative_ && b.negative_);
    if (a.is_zero() || b.is_zero()) {
        const Dec& kept = a.is_zero() ? b : a;  // the zero operand only decides the exponent
        int64_t target = std::max(exp, kept.exp_ - Dec::PREC - 1);  // <= kept.exp_: padding only, at most 29 digits
        return dec_detail::fix(kept.coef_ * dec_detail::ten_pow(kept.exp_ - target), target, kept.negative_);
    }
    // _normalize: an operand entirely below the other's rounding range becomes a sticky digit.
    const Dec& hi = a.exp_ < b.exp_ ? b : a;
    const Dec& lo = a.exp_ < b.exp_ ? a : b;
    int64_t sticky_exp = hi.exp_ + std::min<int64_t>(-1, hi.ndigits_ - Dec::PREC - 2);
    BigInt lo_coef = lo.coef_;
    int64_t work = lo.exp_;
    if (lo.ndigits_ + lo.exp_ - 1 < sticky_exp) {
        lo_coef = BigInt(1);
        work = sticky_exp;
    }
    BigInt x = hi.coef_ * dec_detail::ten_pow(hi.exp_ - work);
    if (hi.negative_) x = -x;
    BigInt y = lo.negative_ ? -lo_coef : lo_coef;
    BigInt sum = x + y;
    if (sum.is_zero()) return dec_detail::fix(BigInt(), exp, false);  // ROUND_HALF_EVEN: +0
    return dec_detail::fix(sum.abs(), work, sum.sign() < 0);
}

bool operator==(const Dec& a, const Dec& b) { return dec_detail::compare(a, b) == 0; }

std::weak_ordering operator<=>(const Dec& a, const Dec& b) {
    int c = dec_detail::compare(a, b);
    return c < 0 ? std::weak_ordering::less : (c > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent);
}

}  // namespace scfg
