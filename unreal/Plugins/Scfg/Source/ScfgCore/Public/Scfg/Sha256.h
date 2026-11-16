// No Python twin: hashlib.sha256(data).digest() (FIPS 180-4), for the core's Solana addresses.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace scfg::sha256 {

// The 32-byte SHA-256 digest of data.
std::array<uint8_t, 32> digest(std::span<const uint8_t> data);
// The same for the bytes of a string.
std::array<uint8_t, 32> digest(std::string_view data);

}  // namespace scfg::sha256
