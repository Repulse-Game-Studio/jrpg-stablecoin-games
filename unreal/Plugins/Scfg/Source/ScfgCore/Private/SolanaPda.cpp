// Copyright 2026 Repulse
// Twin of scfg_core/solana/pda.py (see SolanaPda.h).
#include "Scfg/SolanaPda.h"

#include "Scfg/Err.h"
#include "Scfg/Sha256.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaEd25519.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace scfg::solana::pda::detail {

inline constexpr std::string_view MARKER = "ProgramDerivedAddress";

}  // namespace scfg::solana::pda::detail

namespace scfg::solana::pda {

Result<std::optional<Address>> create_program_address(const Seeds& seeds, std::span<const uint8_t> program_id) {
    const bool too_long = std::any_of(seeds.begin(), seeds.end(), [](const auto& s) { return s.size() > MAX_SEED_LEN; });
    if (seeds.size() > MAX_SEEDS || too_long) return Err::value_error("too many seeds or a seed longer than 32 bytes");
    std::vector<uint8_t> buf;
    for (const std::vector<uint8_t>& s : seeds) buf.insert(buf.end(), s.begin(), s.end());
    buf.insert(buf.end(), program_id.begin(), program_id.end());
    buf.insert(buf.end(), detail::MARKER.begin(), detail::MARKER.end());
    Address digest = sha256::digest(buf);
    if (ed25519::is_on_curve(digest)) return std::optional<Address>();
    return std::optional<Address>(digest);
}

Result<ProgramAddress> find_program_address(const Seeds& seeds, std::span<const uint8_t> program_id) {
    Seeds with = seeds;  // list(seeds) + [bytes([bump])]: the caller's list is not touched
    with.emplace_back(1, uint8_t{0});
    for (int bump = 255; bump >= 0; --bump) {
        with.back()[0] = static_cast<uint8_t>(bump);
        SCFG_TRY(std::optional<Address> address, create_program_address(with, program_id));
        if (address) return ProgramAddress{*address, bump};
    }
    return Err::value_error("no viable bump seed");
}

Result<std::string> associated_token_address(const Json& owner, const Json& mint) {
    Seeds seeds(3);
    SCFG_TRY(seeds[0], base58::decode_fixed(owner, 32));
    SCFG_TRY(seeds[1], base58::decode_fixed(Json(TOKEN_PROGRAM_ID), 32));
    SCFG_TRY(seeds[2], base58::decode_fixed(mint, 32));
    SCFG_TRY(std::vector<uint8_t> program, base58::decode_fixed(Json(ASSOCIATED_TOKEN_PROGRAM_ID), 32));
    SCFG_TRY(ProgramAddress found, find_program_address(seeds, program));
    return base58::b58encode(found.address);
}

}  // namespace scfg::solana::pda
