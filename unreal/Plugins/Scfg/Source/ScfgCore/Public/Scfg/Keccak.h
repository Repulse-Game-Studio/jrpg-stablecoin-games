// Copyright 2026 Repulse
// Twin of scfg_core/keccak.py and scfg_core/_xkcp.py (and of unity/.../Core/Keccak.cs, rpgmaker/src/core/keccak.ts).
// Keccak-256 as used by Ethereum: the original Keccak padding (suffix 0x01), not NIST SHA3-256 (0x06). The
// permutation and the sponge follow the Keccak team's public-domain reference implementation (XKCP's
// CompactFIPS202, vendored in _xkcp.py), on 64-bit lanes instead of Python integers.
#pragma once

#include "Scfg/Export.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace scfg::keccak {

// keccak256(data): the 32-byte digest (Keccak-f[1600], rate 1088 bits, capacity 512, suffix 0x01).
SCFGCORE_API std::array<uint8_t, 32> keccak256(std::span<const uint8_t> data);
// The same for the bytes of a string (Python's keccak256(text.encode()) when text is ASCII, as every caller's is).
SCFGCORE_API std::array<uint8_t, 32> keccak256(std::string_view data);

}  // namespace scfg::keccak
