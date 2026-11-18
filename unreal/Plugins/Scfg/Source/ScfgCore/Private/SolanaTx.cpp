// Copyright 2026 Repulse
// Twin of scfg_core/solana/tx.py (see SolanaTx.h).
#include "Scfg/SolanaTx.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>

namespace scfg::solana::tx::detail {

using Key = std::array<uint8_t, 32>;

Result<Key> decode_key(std::string_view text) {
    SCFG_TRY(std::vector<uint8_t> raw, base58::decode_fixed(Json(text), 32));
    Key k{};
    std::copy(raw.begin(), raw.end(), k.begin());
    return k;
}

// bytearray.append(v) / bytes([v]): a byte, or the ValueError Python raises for one out of range.
Status append_byte(std::vector<uint8_t>& out, size_t v, std::string_view what) {
    if (v > 255) return Err::value_error(std::string(what) + " must be in range(0, 256)");
    out.push_back(static_cast<uint8_t>(v));
    return ok();
}

Status append_compact(std::vector<uint8_t>& out, size_t n) {
    SCFG_TRY(std::vector<uint8_t> enc, compact_u16(static_cast<int64_t>(n)));
    out.insert(out.end(), enc.begin(), enc.end());
    return ok();
}

// int.to_bytes(size, "little"), with its OverflowError.
Result<std::vector<uint8_t>> uint_le(const BigInt& v, size_t size) {
    SCFG_TRY(std::vector<uint8_t> be, v.to_bytes_be(size));
    return std::vector<uint8_t>(be.rbegin(), be.rend());
}

Instruction compute_budget(uint8_t tag, std::vector<uint8_t> le) {
    std::vector<uint8_t> data{tag};
    data.insert(data.end(), le.begin(), le.end());
    return Instruction{std::string(COMPUTE_BUDGET_PROGRAM_ID), {}, std::move(data)};
}

}  // namespace scfg::solana::tx::detail

namespace scfg::solana::tx {

Result<std::vector<uint8_t>> compact_u16(int64_t n) {
    if (n < 0 || n > 0xFFFF) {
        return Err::value_error("compact-u16 out of range: " + py::dec(n));
    }
    std::vector<uint8_t> out;
    uint64_t v = static_cast<uint64_t>(n);
    for (;;) {
        const uint8_t byte = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        if (v != 0) {
            out.push_back(static_cast<uint8_t>(byte | 0x80));
        } else {
            out.push_back(byte);
            return out;
        }
    }
}

Result<std::vector<uint8_t>> compile_message(std::string_view payer, const std::vector<Instruction>& instructions,
                                             std::string_view recent_blockhash) {
    using detail::Key;
    struct Flags {
        bool signer = false;
        bool writable = false;
    };
    std::map<Key, Flags> flags;  // ordered by raw key bytes, as Python's sorted()
    // The decoded keys of each instruction (its accounts, then its program id), for the second pass.
    std::vector<std::vector<Key>> decoded;
    decoded.reserve(instructions.size());
    for (const Instruction& ix : instructions) {
        std::vector<Key>& keys = decoded.emplace_back();
        keys.reserve(ix.accounts.size() + 1);
        for (const AccountMeta& meta : ix.accounts) {
            SCFG_TRY(Key k, detail::decode_key(meta.pubkey));
            Flags& f = flags[k];
            f.signer = f.signer || meta.is_signer;
            f.writable = f.writable || meta.is_writable;
            keys.push_back(k);
        }
        SCFG_TRY(Key program, detail::decode_key(ix.program_id));
        flags[program];  // a readonly non-signer unless it is also listed otherwise
        keys.push_back(program);
    }
    SCFG_TRY(Key payer_raw, detail::decode_key(payer));
    flags.erase(payer_raw);

    std::vector<Key> ordered{payer_raw};
    size_t counts[4] = {0, 0, 0, 0};  // writable signers, readonly signers, writable non-signers, readonly non-signers
    const std::pair<bool, bool> groups[4] = {{true, true}, {true, false}, {false, true}, {false, false}};
    for (size_t g = 0; g < 4; ++g) {
        for (const auto& [k, f] : flags) {
            if (f.signer == groups[g].first && f.writable == groups[g].second) {
                ordered.push_back(k);
                ++counts[g];
            }
        }
    }
    std::map<Key, size_t> index;
    for (size_t i = 0; i < ordered.size(); ++i) index.emplace(ordered[i], i);

    std::vector<uint8_t> out;
    SCFG_TRY_VOID(detail::append_byte(out, 1 + counts[0] + counts[1], "byte"));
    SCFG_TRY_VOID(detail::append_byte(out, counts[1], "byte"));
    SCFG_TRY_VOID(detail::append_byte(out, counts[3], "byte"));
    SCFG_TRY_VOID(detail::append_compact(out, ordered.size()));
    for (const Key& k : ordered) out.insert(out.end(), k.begin(), k.end());
    SCFG_TRY(Key blockhash, detail::decode_key(recent_blockhash));
    out.insert(out.end(), blockhash.begin(), blockhash.end());
    SCFG_TRY_VOID(detail::append_compact(out, instructions.size()));
    for (size_t n = 0; n < instructions.size(); ++n) {
        const Instruction& ix = instructions[n];
        const std::vector<Key>& keys = decoded[n];
        SCFG_TRY_VOID(detail::append_byte(out, index.find(keys.back())->second, "byte"));
        SCFG_TRY_VOID(detail::append_compact(out, ix.accounts.size()));
        for (size_t a = 0; a < ix.accounts.size(); ++a) {
            SCFG_TRY_VOID(detail::append_byte(out, index.find(keys[a])->second, "bytes"));
        }
        SCFG_TRY_VOID(detail::append_compact(out, ix.data.size()));
        out.insert(out.end(), ix.data.begin(), ix.data.end());
    }
    return out;
}

Result<std::vector<uint8_t>> unsigned_transaction(std::span<const uint8_t> message) {
    if (message.empty()) return Err::index_error("index out of range");
    const uint8_t signers = message[0];
    SCFG_TRY(std::vector<uint8_t> out, compact_u16(signers));
    out.insert(out.end(), size_t{64} * signers, uint8_t{0});
    out.insert(out.end(), message.begin(), message.end());
    return out;
}

Result<Instruction> set_compute_unit_limit(const BigInt& units) {
    SCFG_TRY(std::vector<uint8_t> le, detail::uint_le(units, 4));
    return detail::compute_budget(2, std::move(le));
}

Result<Instruction> set_compute_unit_price(const BigInt& micro_lamports) {
    SCFG_TRY(std::vector<uint8_t> le, detail::uint_le(micro_lamports, 8));
    return detail::compute_budget(3, std::move(le));
}

Result<Instruction> transfer_checked(std::string_view source, std::string_view mint, std::string_view destination,
                                     std::string_view owner, const BigInt& amount, int64_t decimals,
                                     const std::vector<std::string>& extra) {
    std::vector<AccountMeta> accounts = {{std::string(source), false, true},
                                         {std::string(mint), false, false},
                                         {std::string(destination), false, true},
                                         {std::string(owner), true, false}};
    for (const std::string& k : extra) accounts.push_back({k, false, false});
    std::vector<uint8_t> data{12};
    SCFG_TRY(std::vector<uint8_t> le, detail::uint_le(amount, 8));
    data.insert(data.end(), le.begin(), le.end());
    if (decimals < 0 || decimals > 255) return Err::value_error("bytes must be in range(0, 256)");
    data.push_back(static_cast<uint8_t>(decimals));
    return Instruction{std::string(pda::TOKEN_PROGRAM_ID), std::move(accounts), std::move(data)};
}

}  // namespace scfg::solana::tx
