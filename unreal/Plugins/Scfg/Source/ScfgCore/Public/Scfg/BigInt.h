// Copyright 2026 Repulse
// No Python twin: Python's int, a signed arbitrary-precision integer.
//
// Sign and magnitude, the magnitude in little-endian 32-bit limbs (uint64_t intermediates: no __int128, as MSVC
// has none). Division follows Python: floordiv() rounds towards negative infinity and mod() takes the divisor's
// sign (-7 // 2 == -4, -7 % 2 == 1, 7 % -2 == -1). There is deliberately no operator/ or operator%, so C++'s
// truncating division cannot be mistaken for Python's. A zero divisor is Python's ZeroDivisionError.
//
// Text: from_decimal/from_hex take ASCII digits only (Python's int() with Unicode digits, underscores, spaces
// and its 4300-digit limit is py::py_int, built on these).
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scfg {

class SCFGCORE_API BigInt {
public:
    struct DivMod;

    BigInt() = default;
    BigInt(int64_t v);  // implicit, as Python mixes int literals freely (x * 10, x == 0)
    static BigInt from_uint64(uint64_t v);
    // An optional '-' then one or more ASCII digits 0-9 (leading zeros fine); anything else is a ValueError.
    static Result<BigInt> from_decimal(std::string_view s);
    // An optional '-', an optional 0x or 0X, then one or more ASCII hex digits of either case; else a ValueError.
    static Result<BigInt> from_hex(std::string_view s);
    // int.from_bytes(b, "big") (unsigned).
    static BigInt from_bytes_be(std::span<const uint8_t> b);

    // The decimal digits of n ("-" first when negative), with no digit limit. Quadratic in the size of n: the core
    // formats an int through py::int_str (CPython's 4300-digit limit, decided from the bit length first), and calls
    // this directly only on a value that is bounded where it is made (each such call says why).
    std::string to_decimal() const;
    // format(n, "x"): lowercase, no 0x prefix, a '-' before a negative, "0" for zero.
    std::string to_hex() const;
    // n.to_bytes(length, "big"): zero-padded; a negative value or one that does not fit is an OverflowError.
    Result<std::vector<uint8_t>> to_bytes_be(size_t length) const;

    // -1, 0 or 1.
    int sign() const { return mag_.empty() ? 0 : (neg_ ? -1 : 1); }
    bool is_zero() const { return mag_.empty(); }
    // n.bit_length(): of the absolute value, 0 for zero.
    uint64_t bit_length() const;
    bool fits_int64() const;
    // An OverflowError when !fits_int64().
    Result<int64_t> to_int64() const;

    BigInt operator-() const;
    BigInt abs() const;
    // n ** e (e >= 0; Python's 0 ** 0 == 1).
    BigInt pow(uint64_t e) const;

    friend SCFGCORE_API BigInt operator+(const BigInt& a, const BigInt& b);
    friend SCFGCORE_API BigInt operator-(const BigInt& a, const BigInt& b);
    friend SCFGCORE_API BigInt operator*(const BigInt& a, const BigInt& b);
    BigInt& operator+=(const BigInt& o) { return *this = *this + o; }
    BigInt& operator-=(const BigInt& o) { return *this = *this - o; }
    BigInt& operator*=(const BigInt& o) { return *this = *this * o; }

    // Python's << and >> (>> floors: -5 >> 1 == -3). A count is never negative here (size_t).
    friend SCFGCORE_API BigInt operator<<(const BigInt& a, size_t n);
    friend SCFGCORE_API BigInt operator>>(const BigInt& a, size_t n);

    // Python's //, % and divmod(); a zero divisor is a ZeroDivisionError.
    Result<BigInt> floordiv(const BigInt& d) const;
    Result<BigInt> mod(const BigInt& d) const;
    Result<DivMod> divmod(const BigInt& d) const;
    // pow(n, e, m), the result with m's sign as Python's. A zero m is a ValueError (Python's "pow() 3rd argument
    // cannot be 0"); so is a negative e (Python's modular inverse, which the core does not need).
    Result<BigInt> pow_mod(const BigInt& e, const BigInt& m) const;

    friend bool operator==(const BigInt& a, const BigInt& b) = default;
    friend SCFGCORE_API std::strong_ordering operator<=>(const BigInt& a, const BigInt& b);

private:
    // Normalized: no high zero limbs, and zero is never negative.
    bool neg_ = false;
    std::vector<uint32_t> mag_;

    BigInt(bool neg, std::vector<uint32_t> mag);
};

struct BigInt::DivMod {
    BigInt quot;
    BigInt rem;
};

}  // namespace scfg
