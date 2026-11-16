// No Python twin: Python's float repr() and float() of a literal (see PyFloat.h). The one core file that calls the
// C library's number formatting and parsing (ruling R3); it keeps them locale-proof by never handing strtod, or
// reading back from snprintf, a decimal point: snprintf's output is read up to its digits and exponent whatever the
// point between them is, and strtod only ever sees "<digits>e<exponent>".
#include "Scfg/PyFloat.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace scfg::pyfloat::detail {

bool digit(char c) { return c >= '0' && c <= '9'; }

// strtod of "[-]<digits>e<exp>": no decimal point, so no locale can change it.
double from_digits(std::string_view sign, std::string_view digits, int64_t exp) {
    std::string text(sign);
    text += digits;
    text += 'e';
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, exp);
    text.append(buf, res.ptr);
    return std::strtod(text.c_str(), nullptr);
}

// m * 10**q as strtod reads it, for m > 0.
double value_of(uint64_t m, int q) {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, m);
    return from_digits("", std::string_view(buf, static_cast<size_t>(res.ptr - buf)), q);
}

// The shortest decimal m * 10**q (m without trailing zeros) that strtod reads back as a > 0, the closest such when
// several. snprintf's %.{p-1}e is the correctly rounded p-digit decimal; when it does not round-trip, its neighbour
// on the other side of a still can (the gap below a power of two is half the gap above), so both neighbours are
// tried before p grows. At p = 17 the correctly rounded decimal always round-trips.
void shortest(double a, uint64_t& m, int& q) {
    for (int p = 1; p <= 17; ++p) {
        char buf[64];
        int n = std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        if (n <= 0 || n >= static_cast<int>(sizeof buf)) continue;
        // "d<point>ddd e<sign>dd": the digits, whatever the locale's point is, then the exponent.
        uint64_t digits = 0;
        int i = 0;
        for (; i < n && buf[i] != 'e' && buf[i] != 'E'; ++i) {
            if (digit(buf[i])) digits = digits * 10 + static_cast<uint64_t>(buf[i] - '0');
        }
        int e = 0;
        bool neg = false;
        if (i < n) ++i;
        if (i < n && (buf[i] == '+' || buf[i] == '-')) neg = buf[i++] == '-';
        for (; i < n && digit(buf[i]); ++i) e = e * 10 + (buf[i] - '0');
        int exp10 = (neg ? -e : e) - (p - 1);
        for (uint64_t c : {digits, digits + 1, digits - 1}) {
            if (c == 0 || value_of(c, exp10) != a) continue;
            m = c;
            q = exp10;
            while (m % 10 == 0) {
                m /= 10;
                ++q;
            }
            return;
        }
    }
    m = 0;  // unreachable for a finite a > 0
    q = 0;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace scfg::pyfloat::detail

namespace scfg::pyfloat {

std::string repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    if (x == 0.0) return std::signbit(x) ? "-0.0" : "0.0";
    uint64_t m = 0;
    int q = 0;
    detail::shortest(std::fabs(x), m, q);
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, m);
    std::string digits(buf, res.ptr);
    int len = static_cast<int>(digits.size());
    int decpt = len + q;  // x = 0.<digits> * 10**decpt
    int exp = decpt - 1;  // x = d.ddd * 10**exp
    std::string out = x < 0 ? "-" : "";
    if (exp < -4 || exp >= 16) {
        out += digits[0];
        if (len > 1) out += "." + digits.substr(1);
        out += exp < 0 ? "e-" : "e+";
        int a = exp < 0 ? -exp : exp;
        if (a < 10) out += '0';
        res = std::to_chars(buf, buf + sizeof buf, a);
        out.append(buf, res.ptr);
    } else if (decpt <= 0) {
        out += "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
    } else if (decpt >= len) {
        out += digits + std::string(static_cast<size_t>(decpt - len), '0') + ".0";
    } else {
        out += digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
    }
    return out;
}

std::optional<double> parse(std::string_view s) {
    size_t i = 0;
    std::string_view sign;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        if (s[i] == '-') sign = "-";
        ++i;
    }
    std::string word = detail::lower_ascii(s.substr(i));
    if (word == "inf" || word == "infinity") {
        double inf = std::numeric_limits<double>::infinity();
        return sign.empty() ? inf : -inf;
    }
    if (word == "nan") return std::copysign(std::numeric_limits<double>::quiet_NaN(), sign.empty() ? 1.0 : -1.0);

    size_t int_start = i;
    while (i < s.size() && detail::digit(s[i])) ++i;
    std::string_view int_part = s.substr(int_start, i - int_start);
    std::string_view frac_part;
    if (i < s.size() && s[i] == '.') {
        size_t f = ++i;
        while (i < s.size() && detail::digit(s[i])) ++i;
        frac_part = s.substr(f, i - f);
    }
    if (int_part.empty() && frac_part.empty()) return std::nullopt;
    // The exponent saturates far beyond any double's range (the mantissa's own length is added below).
    const int64_t cap = int64_t{1} << 40;
    int64_t exp = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
        size_t e = i;
        while (i < s.size() && detail::digit(s[i])) {
            if (exp < cap) exp = exp * 10 + (s[i] - '0');
            ++i;
        }
        if (i == e) return std::nullopt;
        if (neg) exp = -exp;
    }
    if (i != s.size()) return std::nullopt;

    // value = <int_part frac_part> * 10**(exp - len(frac_part)), its digits without leading zeros.
    std::string digits(int_part);
    digits += frac_part;
    size_t lead = digits.find_first_not_of('0');
    if (lead == std::string::npos) return sign.empty() ? 0.0 : -0.0;
    digits.erase(0, lead);
    int64_t scale = exp - static_cast<int64_t>(frac_part.size());
    // digits * 10**scale lies in [10**(n - 1 + scale), 10**(n + scale)), n = len(digits): far outside a double's
    // range it is inf or zero without asking strtod about a huge exponent.
    int64_t magnitude = static_cast<int64_t>(digits.size()) + scale;
    if (magnitude > 400) return sign.empty() ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    if (magnitude < -400) return sign.empty() ? 0.0 : -0.0;
    return detail::from_digits(sign, digits, scale);
}

}  // namespace scfg::pyfloat
