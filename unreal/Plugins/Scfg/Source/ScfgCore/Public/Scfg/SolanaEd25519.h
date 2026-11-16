// Twin of scfg_core/solana/ed25519.py (and of unity/.../Core/Solana/Ed25519.cs, rpgmaker/src/core/solana/ed25519.ts).
// Is a 32-byte value a valid ed25519 public key (a point on the curve)?
//
// Solana uses this to tell wallet addresses (on the curve) from program-derived addresses such as token accounts
// (deliberately off the curve). This mirrors curve25519-dalek's CompressedEdwardsY::decompress, which Solana uses: y
// is read little-endian with the sign bit masked and reduced mod p (non-canonical encodings accepted), and the point
// exists iff (y^2 - 1) / (d*y^2 + 1) is a square mod p.
#pragma once

#include "Scfg/BigInt.h"

#include <cstdint>
#include <span>

namespace scfg::solana::ed25519 {

// p = 2**255 - 19.
const BigInt& P();
// d = -121665 / 121666 mod p (computed as Python computes it, on first use).
const BigInt& D();

// False for anything but 32 bytes.
bool is_on_curve(std::span<const uint8_t> point);

}  // namespace scfg::solana::ed25519
