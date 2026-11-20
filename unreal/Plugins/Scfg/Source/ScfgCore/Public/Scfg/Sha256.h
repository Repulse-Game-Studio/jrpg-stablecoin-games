// Copyright 2026 Repulse
// No Python twin: hashlib.sha256(data).digest() (FIPS 180-4), for the core's Solana addresses.
#pragma once

#include "Scfg/Export.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace scfg::sha256 {

// MSVC warns (C4686) when a dllimport function returns a class template specialization that is not instantiated yet
// at its declaration; sizeof instantiates it here.
static_assert(sizeof(std::array<uint8_t, 32>) == 32);

// The 32-byte SHA-256 digest of data.
SCFGCORE_API std::array<uint8_t, 32> digest(std::span<const uint8_t> data);
// The same for the bytes of a string.
SCFGCORE_API std::array<uint8_t, 32> digest(std::string_view data);

}  // namespace scfg::sha256
