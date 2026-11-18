// Copyright 2026 Repulse
// No Python twin: tests of scfg::solana::pda beyond the tier1/pda.json vectors: the seed limits (16 seeds leave no
// room for a bump), that find_program_address leaves the caller's seeds alone (the C# port reused one array), and
// associated_token_address's errors. Every expectation is scfg_core's own output (python3 with
// plugin/game/python-packages).
#include "support/Test.h"

#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using scfg::Json;
using scfg::Result;
namespace base58 = scfg::solana::base58;
namespace pda = scfg::solana::pda;

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

static std::vector<uint8_t> raw(std::string_view text) { return base58::decode_fixed(Json(text), 32).value(); }

SCFG_TEST(PdaTests, Constants) {
    SCFG_CHECK_EQ(t, pda::MAX_SEEDS, 16);
    SCFG_CHECK_EQ(t, pda::MAX_SEED_LEN, 32);
    SCFG_CHECK_EQ(t, raw(pda::TOKEN_PROGRAM_ID).size(), 32);
    SCFG_CHECK_EQ(t, raw(pda::ASSOCIATED_TOKEN_PROGRAM_ID).size(), 32);
}

SCFG_TEST(PdaTests, SeedLimits) {
    const char* msg = "ValueError: too many seeds or a seed longer than 32 bytes";
    std::vector<uint8_t> none;
    pda::Seeds sixteen(16, std::vector<uint8_t>{'a'});
    SCFG_CHECK(t, pda::create_program_address(sixteen, none).ok());
    SCFG_CHECK_EQ(t, outcome(pda::create_program_address(pda::Seeds(17, std::vector<uint8_t>{'a'}), none)), msg);
    SCFG_CHECK_EQ(t, outcome(pda::create_program_address({std::vector<uint8_t>(33, 'a')}, none)), msg);
    SCFG_CHECK(t, pda::create_program_address({std::vector<uint8_t>(32, 'a')}, none).ok());
    // The bump is a 17th seed.
    SCFG_CHECK_EQ(t, outcome(pda::find_program_address(sixteen, std::vector<uint8_t>(32, 0))), msg);
    SCFG_CHECK(t, pda::find_program_address(pda::Seeds(15, std::vector<uint8_t>{'a'}), std::vector<uint8_t>(32, 0)).ok());
}

SCFG_TEST(PdaTests, FindProgramAddressCopiesTheSeeds) {
    pda::Seeds seeds = {{'s', 'e', 'e', 'd'}};
    Result<pda::ProgramAddress> r = pda::find_program_address(seeds, raw(pda::ASSOCIATED_TOKEN_PROGRAM_ID));
    SCFG_CHECK_EQ(t, outcome(r), "ok");
    if (!r.ok()) return;
    SCFG_CHECK_EQ(t, base58::b58encode(r.value().address), "ZZhuXCdZCkF51KK4C2xNe3AhYtCzmhvQjFmF9ZFMpoU");
    SCFG_CHECK_EQ(t, r.value().bump, 255);
    SCFG_CHECK(t, seeds.size() == 1 && seeds[0].size() == 4);
    // The address is create_program_address of the seeds and the bump, which is off the curve.
    pda::Seeds with = seeds;
    with.push_back({static_cast<uint8_t>(r.value().bump)});
    Result<std::optional<pda::Address>> again = pda::create_program_address(with, raw(pda::ASSOCIATED_TOKEN_PROGRAM_ID));
    SCFG_CHECK(t, again.ok() && again.value() && *again.value() == r.value().address);
}

SCFG_TEST(PdaTests, AssociatedTokenAddress) {
    Result<std::string> ata = pda::associated_token_address("9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM",
                                                            "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v");
    SCFG_CHECK(t, ata.ok() && ata.value() == "FGETo8T8wMcN2wCjav8VK6eh3dLk63evNDPxzLSJra8B");
    SCFG_CHECK_EQ(t, outcome(pda::associated_token_address(5, "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v")),
                  "ValueError: not base58: 5");
    SCFG_CHECK_EQ(t, outcome(pda::associated_token_address("9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM", "")).substr(0, 11),
                  "ValueError:");
}
