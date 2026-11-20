// Copyright 2026 Repulse
// Twin of scfg_core/solana/tx.py (and of unity/.../Core/Solana/Tx.cs, rpgmaker/src/core/solana/tx.ts).
// Legacy Solana transactions, serialized without any library.
//
// Message layout: header (3 bytes) | compact-u16 #keys | keys (32 bytes each) | recent blockhash (32) |
// compact-u16 #instructions | per instruction: program index, compact-u16 #accounts, account indexes,
// compact-u16 #data, data. Account order (as the Solana SDK compiles it): fee payer, then writable signers,
// readonly signers, writable non-signers, readonly non-signers, each group sorted by raw key bytes. Program ids are
// readonly non-signers.
//
// Instructions own their accounts and data (Python's frozen dataclasses hold immutable tuples and bytes): nothing
// here keeps a view of a caller's buffer.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::solana::tx {

inline constexpr std::string_view COMPUTE_BUDGET_PROGRAM_ID = "ComputeBudget111111111111111111111111111111";

struct AccountMeta {
    std::string pubkey;  // base58
    bool is_signer = false;
    bool is_writable = false;
};

struct Instruction {
    std::string program_id;  // base58
    std::vector<AccountMeta> accounts;
    std::vector<uint8_t> data;
};

// The compact-u16 encoding of n (7 bits per byte, low first, the high bit set on every byte but the last), or
// ValueError("compact-u16 out of range: <n>") outside 0..0xFFFF.
SCFGCORE_API Result<std::vector<uint8_t>> compact_u16(int64_t n);

// The legacy message paying from payer. Every key goes through base58 decode_fixed(key, 32) (its ValueError), in
// Python's order: each instruction's accounts then its program id, then the payer, then the blockhash. A signer or
// writable flag of a key is the OR over its uses; the payer is always the first, writable signer. As in Python, a
// message that cannot be written is a ValueError: a header count or a program or account index above 255 ("byte must
// be in range(0, 256)" / "bytes must be in range(0, 256)": more than 255 accounts), or a count or a data length
// above 0xFFFF (compact_u16's).
SCFGCORE_API Result<std::vector<uint8_t>> compile_message(std::string_view payer,
                                                          const std::vector<Instruction>& instructions,
                                                          std::string_view recent_blockhash);

// Empty signature slots + message, ready for a wallet to sign: compact_u16(message[0]) then 64 zero bytes per
// required signature, then the message. An empty message is Python's IndexError("index out of range").
SCFGCORE_API Result<std::vector<uint8_t>> unsigned_transaction(std::span<const uint8_t> message);

// ComputeBudget SetComputeUnitLimit: data 02 | units as 4 bytes little-endian. A negative or too large value is
// int.to_bytes' OverflowError ("can't convert negative int to unsigned" / "int too big to convert").
SCFGCORE_API Result<Instruction> set_compute_unit_limit(const BigInt& units);

// ComputeBudget SetComputeUnitPrice: data 03 | micro_lamports as 8 bytes little-endian (OverflowError as above).
SCFGCORE_API Result<Instruction> set_compute_unit_price(const BigInt& micro_lamports);

// SPL Token TransferChecked: accounts source (writable), mint, destination (writable), owner (signer), then each extra
// key read-only (e.g. a Solana Pay reference, so the payment can be found with getSignaturesForAddress(extra key));
// data 0C | amount as 8 bytes little-endian | decimals. An amount outside u64 is the OverflowError above, then a
// decimals outside 0..255 is ValueError("bytes must be in range(0, 256)").
SCFGCORE_API Result<Instruction> transfer_checked(std::string_view source, std::string_view mint,
                                                  std::string_view destination, std::string_view owner,
                                                  const BigInt& amount, int64_t decimals,
                                                  const std::vector<std::string>& extra = {});

}  // namespace scfg::solana::tx
