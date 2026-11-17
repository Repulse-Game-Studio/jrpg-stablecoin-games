// Twin of scfg_core/amounts.py (and of unity/.../Core/Amounts.cs, rpgmaker/src/core/amounts.ts).
// Money handling: parsing developer/player amounts and the price|code|nonce codec.
//
// On-chain USDC/USDT amounts are integers in base units (6 decimals). The last four decimals of every payment carry
// an item code (2 digits) and a random nonce (2 digits), so a transaction can be traced back to what it bought:
//
//     4.99 | 37 | 42   ->   4.993742 USDC   (units = 4_993_742)
//
// Values are loosely typed where Python's are: a Json is whatever Python value it holds (a Json float is a Python
// float, never an amount), and a Dec is a Decimal. Token units are a BigInt, which, like a Python int, may be
// negative or huge (units_to_decimal and format_units accept those; decode does the range check).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace scfg::amounts {

inline constexpr int64_t TOKEN_DECIMALS = 6;
// Decimal("1000000"). A function-local static, as MIN_TIP() and the registries: nothing in the core runs at static
// initialisation, which an Unreal module loaded on demand would run at an unknown time.
SCFGCORE_API const Dec& MAX_AMOUNT();
// The magnitude bound of every amount the core reads: a nonzero amount's adjusted exponent (the power of ten of its
// leading digit, Decimal.adjusted()) must lie in [-40, 40]. That is far beyond any token amount (the smallest unit is
// 1E-6; a uint256 of base units read through Arc's 18-decimal emitter is floored to 6 decimals and never stored past
// 10**32 units), and it keeps near-Emax values (1E+999999) from overflowing or building million-digit integers.
inline constexpr int64_t AMOUNT_EXPONENT_LIMIT = 40;
// The longest amount text the core parses (parse_amount, stored_amount), in code points: far beyond any amount (a
// stored one is at most about 40 characters), and it keeps digit parsing cheap in every core.
inline constexpr size_t AMOUNT_TEXT_LIMIT = 100;

// An amount read back from storage (a receipt's amount, a session's price): Decimal(value) when it is a finite
// number within the amount range (AMOUNT_EXPONENT_LIMIT) and, as text, at most AMOUNT_TEXT_LIMIT characters, else a
// ValueError (a TypeError for a list, or for a value Decimal() does not take: None, a dict). A str takes digit-group
// underscores and any exponent libmpdec accepts, as Decimal(str) does.
SCFGCORE_API Result<Dec> stored_amount(const Json& value);

// Validates a price/tip amount and returns it quantized to cents.
//
// Accepts str, int or Decimal. Floats (and bools) are refused with a TypeError: binary floating point and money do
// not mix. A str is a plain decimal number: Decimal's digit-group underscores ("1_000") are refused like any other
// malformed amount. Anything out of (0, MAX_AMOUNT()] or with more than 2 decimals is a ValueError.
SCFGCORE_API Result<Dec> parse_amount(const Json& value);
SCFGCORE_API Result<Dec> parse_amount(const Dec& value);

// price (2 decimals) + code (0 = tip, 1..99 = item) + nonce -> base units. code and nonce must be ints in 0..99
// (a bool is not one), else a ValueError.
SCFGCORE_API Result<BigInt> encode(const Json& price, const Json& code, const Json& nonce);
SCFGCORE_API Result<BigInt> encode(const Dec& price, const Json& code, const Json& nonce);

struct Decoded {
    Dec price;
    int64_t code = 0;
    int64_t nonce = 0;
};

// Inverse of encode: base units -> (price, code, nonce). A ValueError for negative units. From 10**32 units the
// price needs more than Decimal's 28 digits and this is an InvalidOperation (an ArithmeticError, not a ValueError),
// as Python's decimal raises; callers that read transfers off the chain skip such a value.
SCFGCORE_API Result<Decoded> decode(const BigInt& units);

// Base units -> token amount, exactly (4993742 -> 4.993742), however many digits: the value is built from its
// digits instead of through Decimal's 28-digit arithmetic.
SCFGCORE_API Dec units_to_decimal(const BigInt& units);

// Token amount -> base units, exactly (no rounding to Decimal's 28 digits). A ValueError for more than 6 decimals,
// or for an amount out of range (AMOUNT_EXPONENT_LIMIT). A Dec is always finite: a caller holding a non-finite
// Decimal (Dec::parse's nullopt) raises that ValueError itself.
SCFGCORE_API Result<BigInt> decimal_to_units(const Dec& amount);

// Human display of a token amount with all 6 decimals: 4993742 -> "4.993742" (f"{d:.6f}").
SCFGCORE_API std::string format_units(const BigInt& units);

}  // namespace scfg::amounts
