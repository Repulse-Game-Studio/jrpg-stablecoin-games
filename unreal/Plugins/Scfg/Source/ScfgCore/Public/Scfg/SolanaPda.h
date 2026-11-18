// Copyright 2026 Repulse
// Twin of scfg_core/solana/pda.py (and of unity/.../Core/Solana/Pda.cs, rpgmaker/src/core/solana/pda.ts).
// Program-derived addresses and associated token accounts (classic SPL Token program).
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::solana::pda {

inline constexpr std::string_view TOKEN_PROGRAM_ID = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
inline constexpr std::string_view ASSOCIATED_TOKEN_PROGRAM_ID = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL";
inline constexpr size_t MAX_SEED_LEN = 32;
inline constexpr size_t MAX_SEEDS = 16;

using Seeds = std::vector<std::vector<uint8_t>>;
using Address = std::array<uint8_t, 32>;

// sha256(b"".join(seeds) + program_id + b"ProgramDerivedAddress"), or nullopt when that hash is a point on the curve
// (a valid public key, so not a program address). More than MAX_SEEDS seeds or a seed longer than MAX_SEED_LEN
// bytes is ValueError("too many seeds or a seed longer than 32 bytes").
SCFGCORE_API Result<std::optional<Address>> create_program_address(const Seeds& seeds,
                                                                   std::span<const uint8_t> program_id);

// Python's (address, bump) tuple.
struct ProgramAddress {
    Address address{};
    int bump = 0;
};

// The first bump seed from 255 down to 0 whose seeds + [bytes([bump])] give a program address. The caller's seeds are
// copied, never changed. The create_program_address ValueError (15 seeds is the most that leave room for the bump),
// or ValueError("no viable bump seed").
SCFGCORE_API Result<ProgramAddress> find_program_address(const Seeds& seeds, std::span<const uint8_t> program_id);

// The base58 associated token account of owner for mint, as wallets create it: the program address of [owner,
// TOKEN_PROGRAM_ID, mint] under ASSOCIATED_TOKEN_PROGRAM_ID. owner and mint go through decode_fixed(x, 32), so text
// that is not a 32-byte base58 str is its ValueError.
SCFGCORE_API Result<std::string> associated_token_address(const Json& owner, const Json& mint);

}  // namespace scfg::solana::pda
