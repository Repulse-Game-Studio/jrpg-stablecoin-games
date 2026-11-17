// Twin of scfg_core/solana/base58.py (and of unity/.../Core/Solana/Base58.cs, rpgmaker/src/core/solana/base58.ts).
// Base58 (Bitcoin alphabet), as used for Solana addresses and signatures.
//
// Text is loosely typed, as in Python: a Json that is not a str is the ValueError Python raises for it. Lengths are
// counted in code points (len()), so 44 emoji are 44 characters, not 176 bytes.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::solana::base58 {

inline constexpr std::string_view ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// The base58 text of data: one '1' per leading zero byte, then the big-endian number's digits.
SCFGCORE_API std::string b58encode(std::span<const uint8_t> data);

// The bytes of base58 text. Text that is not a non-empty str is ValueError("not base58: <repr>"), and the first
// character outside the alphabet is ValueError("invalid base58 character <repr>"). Quadratic in the text's length,
// as Python's is: decode_fixed bounds the length first.
SCFGCORE_API Result<std::vector<uint8_t>> b58decode(const Json& text);

// The longest base58 text of `length` bytes, math.ceil(length * math.log(256) / math.log(58)): 44 characters for
// 32, 88 for 64. Computed in doubles as Python computes it; nullopt where Python's float arithmetic overflows (an
// OverflowError there: a length beyond about 3.2e307).
SCFGCORE_API std::optional<double> max_encoded_length(const BigInt& length);

// `length` bytes from base58 `text`. Text longer (in code points) than any encoding of `length` bytes is refused
// before it is decoded. Bad input is a ValueError, a `length` that is not an int (a bool, a float, a str, ...)
// included, as Python's ("not a byte count: <repr>"). The text is not stripped.
SCFGCORE_API Result<std::vector<uint8_t>> decode_fixed(const Json& text, const Json& length);

// The canonical base58 form of a 32-byte public key: text.strip(), decoded as exactly 32 bytes and encoded again.
// A non-str is ValueError("not an address: <repr>").
SCFGCORE_API Result<std::string> normalize_pubkey(const Json& text);

// The same for a 64-byte signature; a non-str is ValueError("not a signature: <repr>").
SCFGCORE_API Result<std::string> normalize_signature(const Json& text);

}  // namespace scfg::solana::base58
