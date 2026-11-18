// Copyright 2026 Repulse
// No Python twin: Python's int (sign and magnitude, 32-bit limbs, Knuth's algorithm D for division).
#include "Scfg/BigInt.h"

#include "Scfg/Py.h"

#include <algorithm>
#include <bit>

namespace scfg {
namespace bigint_detail {

using Limbs = std::vector<uint32_t>;
constexpr uint64_t LIMB_BASE = uint64_t(1) << 32;
constexpr uint32_t DEC_CHUNK = 1000000000;  // 10^9 < 2^32: one short multiply/divide per 9 decimal digits

void trim(Limbs& a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}

int cmp(const Limbs& a, const Limbs& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

Limbs add(const Limbs& a, const Limbs& b) {
    const Limbs& lo = a.size() < b.size() ? a : b;
    const Limbs& hi = a.size() < b.size() ? b : a;
    Limbs out(hi.size() + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < hi.size(); ++i) {
        uint64_t s = uint64_t(hi[i]) + carry + (i < lo.size() ? lo[i] : 0);
        out[i] = uint32_t(s);
        carry = s >> 32;
    }
    out[hi.size()] = uint32_t(carry);
    trim(out);
    return out;
}

// a - b for a >= b.
Limbs sub(const Limbs& a, const Limbs& b) {
    Limbs out(a.size());
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t d = uint64_t(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        out[i] = uint32_t(d);
        borrow = (d >> 32) != 0 ? 1 : 0;  // wrapped below zero
    }
    trim(out);
    return out;
}

Limbs mul(const Limbs& a, const Limbs& b) {
    if (a.empty() || b.empty()) return {};
    Limbs out(a.size() + b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            // < 2^64: (2^32-1)^2 + 2 * (2^32-1) == 2^64 - 1
            uint64_t t = uint64_t(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = uint32_t(t);
            carry = t >> 32;
        }
        out[i + b.size()] = uint32_t(carry);
    }
    trim(out);
    return out;
}

// a = a * k + add, in place.
void mul_add_small(Limbs& a, uint32_t k, uint32_t addend) {
    uint64_t carry = addend;
    for (uint32_t& limb : a) {
        uint64_t t = uint64_t(limb) * k + carry;
        limb = uint32_t(t);
        carry = t >> 32;
    }
    if (carry != 0) a.push_back(uint32_t(carry));
}

// a = a / d in place; returns a % d. d != 0.
uint32_t div_small(Limbs& a, uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = a.size(); i-- > 0;) {
        uint64_t cur = (rem << 32) | a[i];
        a[i] = uint32_t(cur / d);
        rem = cur % d;
    }
    trim(a);
    return uint32_t(rem);
}

Limbs shl(const Limbs& a, size_t n) {
    if (a.empty()) return {};
    size_t words = n / 32;
    unsigned bits = unsigned(n % 32);
    Limbs out(a.size() + words + 1);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t v = uint64_t(a[i]) << bits;
        out[i + words] |= uint32_t(v);
        out[i + words + 1] |= uint32_t(v >> 32);
    }
    trim(out);
    return out;
}

// a >> n; sets dropped when a one bit was shifted out.
Limbs shr(const Limbs& a, size_t n, bool& dropped) {
    size_t words = n / 32;
    unsigned bits = unsigned(n % 32);
    dropped = false;
    if (words >= a.size()) {
        dropped = !a.empty();
        return {};
    }
    for (size_t i = 0; i < words; ++i) dropped = dropped || a[i] != 0;
    if (bits != 0 && (a[words] & ((uint32_t(1) << bits) - 1)) != 0) dropped = true;
    Limbs out(a.size() - words);
    for (size_t i = 0; i < out.size(); ++i) {
        uint64_t v = a[i + words];
        if (i + words + 1 < a.size()) v |= uint64_t(a[i + words + 1]) << 32;
        out[i] = uint32_t(v >> bits);
    }
    trim(out);
    return out;
}

// Knuth, TAOCP vol. 2, 4.3.1, algorithm D (as Hacker's Delight's divmnu64): u / v and u % v for v != 0.
void divmod(const Limbs& u, const Limbs& v, Limbs& q, Limbs& r) {
    if (cmp(u, v) < 0) {
        q.clear();
        r = u;
        return;
    }
    if (v.size() == 1) {
        q = u;
        uint32_t rem = div_small(q, v[0]);
        r = rem != 0 ? Limbs{rem} : Limbs{};
        return;
    }
    const size_t m = u.size(), n = v.size();
    // D1: normalize so that v's top limb has its high bit set (then qhat is at most 2 too large).
    const unsigned s = unsigned(std::countl_zero(v[n - 1]));
    Limbs vn(n), un(m + 1);
    for (size_t i = n - 1; i > 0; --i) {
        vn[i] = uint32_t((uint64_t(v[i]) << s) | (uint64_t(v[i - 1]) >> (32 - s)));
    }
    vn[0] = uint32_t(uint64_t(v[0]) << s);
    un[m] = uint32_t(uint64_t(u[m - 1]) >> (32 - s));
    for (size_t i = m - 1; i > 0; --i) {
        un[i] = uint32_t((uint64_t(u[i]) << s) | (uint64_t(u[i - 1]) >> (32 - s)));
    }
    un[0] = uint32_t(uint64_t(u[0]) << s);

    q.assign(m - n + 1, 0);
    for (size_t j = m - n + 1; j-- > 0;) {
        // D3: estimate qhat from the top two limbs, then correct it with the third.
        uint64_t num = (uint64_t(un[j + n]) << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1];
        uint64_t rhat = num % vn[n - 1];
        while (qhat >= LIMB_BASE || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            --qhat;
            rhat += vn[n - 1];
            if (rhat >= LIMB_BASE) break;
        }
        // D4: multiply and subtract qhat * vn from un[j .. j+n].
        int64_t borrow = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = qhat * vn[i];
            int64_t t = int64_t(un[i + j]) - borrow - int64_t(p & 0xffffffffu);
            un[i + j] = uint32_t(t);
            borrow = int64_t(p >> 32) - (t >> 32);
        }
        int64_t t = int64_t(un[j + n]) - borrow;
        un[j + n] = uint32_t(t);
        q[j] = uint32_t(qhat);
        // D6: qhat was one too large (rare); add vn back.
        if (t < 0) {
            q[j] -= 1;
            uint64_t carry = 0;
            for (size_t i = 0; i < n; ++i) {
                uint64_t sum = uint64_t(un[i + j]) + vn[i] + carry;
                un[i + j] = uint32_t(sum);
                carry = sum >> 32;
            }
            un[j + n] = uint32_t(uint64_t(un[j + n]) + carry);
        }
    }
    trim(q);
    // D8: unnormalize the remainder.
    r.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        r[i] = uint32_t((uint64_t(un[i]) >> s) | (uint64_t(un[i + 1]) << (32 - s)));
    }
    trim(r);
}

std::string bad_literal(std::string_view s, int base) {
    return std::string("invalid literal for int() with base ") + (base == 10 ? "10" : "16") + ": '" + std::string(s) +
           "'";
}

}  // namespace bigint_detail

BigInt::BigInt(bool neg, std::vector<uint32_t> mag) : neg_(neg), mag_(std::move(mag)) {
    bigint_detail::trim(mag_);
    if (mag_.empty()) neg_ = false;
}

BigInt::BigInt(int64_t v) {
    // The magnitude of INT64_MIN is 2^63, which no int64_t holds: negate in uint64_t.
    uint64_t m = v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
    *this = from_uint64(m);
    neg_ = v < 0;
}

BigInt BigInt::from_uint64(uint64_t v) { return BigInt(false, {uint32_t(v), uint32_t(v >> 32)}); }

Result<BigInt> BigInt::from_decimal(std::string_view s) {
    std::string_view digits = s;
    bool neg = !digits.empty() && digits[0] == '-';
    if (neg) digits.remove_prefix(1);
    if (digits.empty()) return Err::value_error(bigint_detail::bad_literal(s, 10));
    for (char c : digits) {
        if (c < '0' || c > '9') return Err::value_error(bigint_detail::bad_literal(s, 10));
    }
    bigint_detail::Limbs mag;
    size_t width = digits.size() % 9 == 0 ? 9 : digits.size() % 9;
    for (size_t pos = 0; pos < digits.size(); pos += width, width = 9) {
        uint32_t chunk = 0, mult = 1;
        for (size_t i = pos; i < pos + width; ++i) {
            chunk = chunk * 10 + uint32_t(digits[i] - '0');
            mult *= 10;
        }
        bigint_detail::mul_add_small(mag, mult, chunk);
    }
    return BigInt(neg, std::move(mag));
}

Result<BigInt> BigInt::from_hex(std::string_view s) {
    std::string_view digits = s;
    bool neg = !digits.empty() && digits[0] == '-';
    if (neg) digits.remove_prefix(1);
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.remove_prefix(2);
    if (digits.empty()) return Err::value_error(bigint_detail::bad_literal(s, 16));
    bigint_detail::Limbs mag((digits.size() + 7) / 8);
    for (size_t i = 0; i < digits.size(); ++i) {
        int d = py::hex_value(digits[digits.size() - 1 - i]);  // i-th digit from the right
        if (d < 0) return Err::value_error(bigint_detail::bad_literal(s, 16));
        mag[i / 8] |= uint32_t(d) << (4 * (i % 8));
    }
    return BigInt(neg, std::move(mag));
}

BigInt BigInt::from_bytes_be(std::span<const uint8_t> b) {
    bigint_detail::Limbs mag((b.size() + 3) / 4);
    for (size_t i = 0; i < b.size(); ++i) {
        mag[i / 4] |= uint32_t(b[b.size() - 1 - i]) << (8 * (i % 4));  // i-th byte from the right
    }
    return BigInt(false, std::move(mag));
}

std::string BigInt::to_decimal() const {
    if (mag_.empty()) return "0";
    bigint_detail::Limbs work = mag_;
    std::vector<uint32_t> chunks;  // base 10^9, least significant first
    while (!work.empty()) chunks.push_back(bigint_detail::div_small(work, bigint_detail::DEC_CHUNK));
    std::string out = neg_ ? "-" : "";
    for (size_t i = chunks.size(); i-- > 0;) {
        char buf[9];
        uint32_t c = chunks[i];
        for (int k = 8; k >= 0; --k, c /= 10) buf[k] = char('0' + c % 10);
        size_t start = 0;
        if (i == chunks.size() - 1) {  // the most significant chunk is not zero-padded
            while (start < 8 && buf[start] == '0') ++start;
        }
        out.append(buf + start, buf + 9);
    }
    return out;
}

std::string BigInt::to_hex() const {
    if (mag_.empty()) return "0";
    std::string out = neg_ ? "-" : "";
    bool started = false;
    for (size_t i = mag_.size(); i-- > 0;) {
        for (int k = 7; k >= 0; --k) {
            unsigned d = (mag_[i] >> (4 * k)) & 0xf;
            if (d == 0 && !started) continue;
            started = true;
            out += py::HEX_DIGITS[d];
        }
    }
    return out;
}

Result<std::vector<uint8_t>> BigInt::to_bytes_be(size_t length) const {
    if (neg_) return Err::overflow_error("can't convert negative int to unsigned");
    if (bit_length() > uint64_t(length) * 8) return Err::overflow_error("int too big to convert");
    std::vector<uint8_t> out(length, 0);
    for (size_t i = 0; i < length && i / 4 < mag_.size(); ++i) {
        out[length - 1 - i] = uint8_t(mag_[i / 4] >> (8 * (i % 4)));
    }
    return out;
}

uint64_t BigInt::bit_length() const {
    if (mag_.empty()) return 0;
    return uint64_t(mag_.size() - 1) * 32 + uint64_t(32 - std::countl_zero(mag_.back()));
}

bool BigInt::fits_int64() const {
    if (bit_length() <= 63) return true;
    // -2^63 is the one 64-bit magnitude that fits.
    return neg_ && mag_.size() == 2 && mag_[1] == 0x80000000u && mag_[0] == 0;
}

Result<int64_t> BigInt::to_int64() const {
    if (!fits_int64()) return Err::overflow_error("Python int too large to convert to C int64_t");
    uint64_t m = 0;
    for (size_t i = mag_.size(); i-- > 0;) m = (m << 32) | mag_[i];
    // Two's complement negation in uint64_t, then the (C++20-defined) modular conversion.
    return int64_t(neg_ ? uint64_t(0) - m : m);
}

BigInt BigInt::operator-() const { return BigInt(!neg_, mag_); }

BigInt BigInt::abs() const { return BigInt(false, mag_); }

BigInt BigInt::pow(uint64_t e) const {
    BigInt result(1), base = *this;
    while (e != 0) {
        if (e & 1) result = result * base;
        e >>= 1;
        if (e != 0) base = base * base;
    }
    return result;
}

BigInt operator+(const BigInt& a, const BigInt& b) {
    if (a.neg_ == b.neg_) return BigInt(a.neg_, bigint_detail::add(a.mag_, b.mag_));
    int c = bigint_detail::cmp(a.mag_, b.mag_);
    if (c == 0) return BigInt();
    // Different signs: the larger magnitude wins the sign.
    if (c > 0) return BigInt(a.neg_, bigint_detail::sub(a.mag_, b.mag_));
    return BigInt(b.neg_, bigint_detail::sub(b.mag_, a.mag_));
}

BigInt operator-(const BigInt& a, const BigInt& b) { return a + (-b); }

BigInt operator*(const BigInt& a, const BigInt& b) {
    return BigInt(a.neg_ != b.neg_, bigint_detail::mul(a.mag_, b.mag_));
}

BigInt operator<<(const BigInt& a, size_t n) { return BigInt(a.neg_, bigint_detail::shl(a.mag_, n)); }

BigInt operator>>(const BigInt& a, size_t n) {
    bool dropped = false;
    bigint_detail::Limbs mag = bigint_detail::shr(a.mag_, n, dropped);
    if (!a.neg_) return BigInt(false, std::move(mag));
    // Floor for a negative: -((|a| >> n) + 1) when a one bit was shifted out.
    if (dropped) mag = bigint_detail::add(mag, {1});
    return BigInt(true, std::move(mag));
}

Result<BigInt::DivMod> BigInt::divmod(const BigInt& d) const {
    if (d.is_zero()) return Err::make("ZeroDivisionError", "integer division or modulo by zero");
    bigint_detail::Limbs q, r;
    bigint_detail::divmod(mag_, d.mag_, q, r);
    // Truncated: the quotient's sign is the product of the signs, the remainder has the dividend's.
    DivMod out{BigInt(neg_ != d.neg_, std::move(q)), BigInt(neg_, std::move(r))};
    // Floor: a non-zero remainder whose sign differs from the divisor's moves one divisor over.
    if (!out.rem.is_zero() && out.rem.neg_ != d.neg_) {
        out.quot = out.quot - 1;
        out.rem = out.rem + d;
    }
    return out;
}

Result<BigInt> BigInt::floordiv(const BigInt& d) const {
    SCFG_TRY(DivMod qr, divmod(d));
    return std::move(qr.quot);
}

Result<BigInt> BigInt::mod(const BigInt& d) const {
    SCFG_TRY(DivMod qr, divmod(d));
    return std::move(qr.rem);
}

Result<BigInt> BigInt::pow_mod(const BigInt& e, const BigInt& m) const {
    if (m.is_zero()) return Err::value_error("pow() 3rd argument cannot be 0");
    if (e.neg_) return Err::value_error("BigInt::pow_mod: negative exponent (modular inverse) is not supported");
    // Square and multiply, reducing with Python's % throughout, so the result has m's sign as Python's.
    SCFG_TRY(BigInt result, BigInt(1).mod(m));
    SCFG_TRY(BigInt base, mod(m));
    for (uint64_t i = e.bit_length(); i-- > 0;) {
        SCFG_TRY(result, (result * result).mod(m));
        if ((e.mag_[i / 32] >> (i % 32)) & 1) {
            SCFG_TRY(result, (result * base).mod(m));
        }
    }
    return result;
}

std::strong_ordering operator<=>(const BigInt& a, const BigInt& b) {
    if (a.neg_ != b.neg_) return a.neg_ ? std::strong_ordering::less : std::strong_ordering::greater;
    int c = bigint_detail::cmp(a.mag_, b.mag_);
    if (a.neg_) c = -c;
    return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
}

}  // namespace scfg
