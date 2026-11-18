// Copyright 2026 Repulse
// No Python twin: tests of scfg::solana::tx beyond the tier1/solana_tx.json vectors: the byte layouts of
// SetComputeUnitLimit, SetComputeUnitPrice and TransferChecked (which have no vectors), more than 255 accounts as
// Python's ValueError (the C# port's byte casts wrapped), an empty message as Python's IndexError (C#: ValueError),
// and instructions that own their data. Every expectation is scfg_core's own output (python3 with
// plugin/game/python-packages).
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Bytes.h"
#include "Scfg/Result.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"
#include "Scfg/SolanaTx.h"

#include <cstdint>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Result;
namespace base58 = scfg::solana::base58;
namespace tx = scfg::solana::tx;

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

static std::string hex(const Result<std::vector<uint8_t>>& r) {
    return r.ok() ? scfg::bytes::to_hex(r.value()) : outcome(r);
}

static std::string data_hex(const Result<tx::Instruction>& r) {
    return r.ok() ? scfg::bytes::to_hex(r.value().data) : outcome(r);
}

// base58 of the 32-byte big-endian n.
static std::string key(uint64_t n) {
    std::vector<uint8_t> b(32, 0);
    for (int i = 31; i >= 24; --i) {
        b[static_cast<size_t>(i)] = static_cast<uint8_t>(n & 0xFF);
        n >>= 8;
    }
    return base58::b58encode(b);
}

static std::string fill(uint8_t byte) { return base58::b58encode(std::vector<uint8_t>(32, byte)); }

SCFG_TEST(SolanaTxTests, CompactU16) {
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(0)), "00");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(127)), "7f");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(128)), "8001");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(16384)), "808001");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(65535)), "ffff03");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(65536)), "ValueError: compact-u16 out of range: 65536");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(-1)), "ValueError: compact-u16 out of range: -1");
    SCFG_CHECK_EQ(t, hex(tx::compact_u16(INT64_MIN)), "ValueError: compact-u16 out of range: -9223372036854775808");
}

SCFG_TEST(SolanaTxTests, SetComputeUnitLimitLayout) {
    Result<tx::Instruction> ix = tx::set_compute_unit_limit(BigInt(200000));
    SCFG_CHECK_EQ(t, data_hex(ix), "02400d0300");
    if (ix.ok()) {
        SCFG_CHECK_EQ(t, ix.value().program_id, tx::COMPUTE_BUDGET_PROGRAM_ID);
        SCFG_CHECK(t, ix.value().accounts.empty());
    }
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_limit(BigInt::from_uint64(0xFFFFFFFFULL))), "02ffffffff");
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_limit(BigInt(0))), "0200000000");
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_limit(BigInt::from_uint64(0x100000000ULL))),
                  "OverflowError: int too big to convert");
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_limit(BigInt(-1))), "OverflowError: can't convert negative int to unsigned");
}

SCFG_TEST(SolanaTxTests, SetComputeUnitPriceLayout) {
    Result<tx::Instruction> ix = tx::set_compute_unit_price(BigInt(1000));
    SCFG_CHECK_EQ(t, data_hex(ix), "03e803000000000000");
    if (ix.ok()) {
        SCFG_CHECK_EQ(t, ix.value().program_id, tx::COMPUTE_BUDGET_PROGRAM_ID);
        SCFG_CHECK(t, ix.value().accounts.empty());
    }
    // Above int64: Python's int, so the whole u64 range.
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_price(BigInt::from_uint64(UINT64_MAX))), "03ffffffffffffffff");
    SCFG_CHECK_EQ(t, data_hex(tx::set_compute_unit_price(BigInt(1) << 64)), "OverflowError: int too big to convert");
}

SCFG_TEST(SolanaTxTests, TransferCheckedLayout) {
    Result<tx::Instruction> ix = tx::transfer_checked(
        "So11111111111111111111111111111111111111112", "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
        "11111111111111111111111111111111", "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", BigInt(4990242), 6,
        {"ComputeBudget111111111111111111111111111111"});
    SCFG_CHECK_EQ(t, data_hex(ix), "0c22254c000000000006");
    if (!ix.ok()) return;
    const tx::Instruction& i = ix.value();
    SCFG_CHECK_EQ(t, i.program_id, scfg::solana::pda::TOKEN_PROGRAM_ID);
    SCFG_CHECK_EQ(t, i.accounts.size(), 5);
    if (i.accounts.size() != 5) return;
    const char* names[5] = {"So11111111111111111111111111111111111111112", "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
                            "11111111111111111111111111111111", "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
                            "ComputeBudget111111111111111111111111111111"};
    const bool signer[5] = {false, false, false, true, false};
    const bool writable[5] = {true, false, true, false, false};
    for (size_t k = 0; k < 5; ++k) {
        SCFG_CHECK_EQ(t, i.accounts[k].pubkey, names[k]);
        SCFG_CHECK_EQ(t, i.accounts[k].is_signer, signer[k]);
        SCFG_CHECK_EQ(t, i.accounts[k].is_writable, writable[k]);
    }
    // No extra keys: the four accounts only. The largest amount and decimals.
    Result<tx::Instruction> plain = tx::transfer_checked("a", "b", "c", "d", BigInt::from_uint64(UINT64_MAX), 255);
    SCFG_CHECK_EQ(t, data_hex(plain), "0cffffffffffffffffff");
    SCFG_CHECK(t, plain.ok() && plain.value().accounts.size() == 4);
    // The amount is converted first, then the decimals.
    SCFG_CHECK_EQ(t, data_hex(tx::transfer_checked("a", "b", "c", "d", BigInt(1) << 64, 256)),
                  "OverflowError: int too big to convert");
    SCFG_CHECK_EQ(t, data_hex(tx::transfer_checked("a", "b", "c", "d", BigInt(1), 256)),
                  "ValueError: bytes must be in range(0, 256)");
    SCFG_CHECK_EQ(t, data_hex(tx::transfer_checked("a", "b", "c", "d", BigInt(1), -1)),
                  "ValueError: bytes must be in range(0, 256)");
    SCFG_CHECK_EQ(t, data_hex(tx::transfer_checked("a", "b", "c", "d", BigInt(-1), 6)),
                  "OverflowError: can't convert negative int to unsigned");
}

SCFG_TEST(SolanaTxTests, InstructionsOwnTheirData) {
    std::vector<uint8_t> buffer = {1, 2};
    tx::Instruction ix{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {}, buffer};
    buffer[0] = 9;
    buffer.push_back(3);
    Result<std::vector<uint8_t>> m = tx::compile_message(fill(9), {ix}, fill(7));
    SCFG_CHECK_EQ(t, hex(m),
                  "0100010209090909090909090909090909090909090909090909090909090909090909090306466fe5211732ffecadba72c39b"
                  "e7bc8ce5bbc5f7126b2c439b3a400000000707070707070707070707070707070707070707070707070707070707070707"
                  "010100020102");
}

// One instruction of the compute-budget program over n accounts key(1)..key(n).
static std::vector<tx::Instruction> wide(size_t n, bool signer, bool writable) {
    tx::Instruction ix{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {}, {}};
    for (size_t k = 0; k < n; ++k) ix.accounts.push_back({key(k + 1), signer, writable});
    return {ix};
}

static std::string compiled(const std::vector<tx::Instruction>& ixs) {
    Result<std::vector<uint8_t>> m = tx::compile_message(fill(9), ixs, fill(7));
    if (!m.ok()) return outcome(m);
    return std::to_string(m.value().size()) + " " + scfg::bytes::to_hex(std::span(m.value()).first(3));
}

SCFG_TEST(SolanaTxTests, MoreThan255AccountsIsAValueError) {
    // 256 keys (payer, 254 accounts, the program) is the most a legacy message indexes.
    SCFG_CHECK_EQ(t, compiled(wide(254, false, true)), "8488 010001");
    SCFG_CHECK_EQ(t, compiled(wide(255, false, true)), "ValueError: byte must be in range(0, 256)");  // program index
    SCFG_CHECK_EQ(t, compiled(wide(300, false, true)), "ValueError: byte must be in range(0, 256)");
    SCFG_CHECK_EQ(t, compiled(wide(254, false, false)), "8488 0100ff");
    SCFG_CHECK_EQ(t, compiled(wide(255, false, false)), "ValueError: byte must be in range(0, 256)");  // header count
    SCFG_CHECK_EQ(t, compiled(wide(256, false, false)), "ValueError: byte must be in range(0, 256)");
    SCFG_CHECK_EQ(t, compiled(wide(253, true, true)), "8455 fe0001");
    SCFG_CHECK_EQ(t, compiled(wide(254, true, true)), "8488 ff0001");
    SCFG_CHECK_EQ(t, compiled(wide(255, true, true)), "ValueError: byte must be in range(0, 256)");  // 1 + 255 signers
    // A program that is also a writable signer of its instruction comes early, so an account index overflows.
    std::string program = key(400);
    tx::Instruction ix{program, {{program, true, true}}, {}};
    for (size_t k = 0; k < 300; ++k) ix.accounts.push_back({key(k + 1), false, true});
    SCFG_CHECK_EQ(t, compiled({ix}), "ValueError: bytes must be in range(0, 256)");
    ix.accounts.resize(255);
    SCFG_CHECK_EQ(t, compiled({ix}), "8489 020000");
}

SCFG_TEST(SolanaTxTests, CountsAndDataBeyondCompactU16) {
    tx::Instruction big{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {}, std::vector<uint8_t>(65536, 0)};
    SCFG_CHECK_EQ(t, compiled({big}), "ValueError: compact-u16 out of range: 65536");
    big.data.resize(65535);
    SCFG_CHECK(t, tx::compile_message(fill(9), {big}, fill(7)).ok());
}

SCFG_TEST(SolanaTxTests, KeysAreDecodedInPythonsOrder) {
    // An account key fails before the payer, the payer before the blockhash.
    tx::Instruction ix{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {{"0bad", false, true}}, {}};
    SCFG_CHECK_EQ(t, outcome(tx::compile_message("bad0", {ix}, "bad1")), "ValueError: invalid base58 character '0'");
    SCFG_CHECK_EQ(t, outcome(tx::compile_message("I", {}, "0")), "ValueError: invalid base58 character 'I'");
    SCFG_CHECK_EQ(t, outcome(tx::compile_message(fill(9), {}, "l")), "ValueError: invalid base58 character 'l'");
    // A key used twice takes the OR of its flags; the payer stays first even when listed as read-only.
    tx::Instruction a{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {{key(5), false, true}, {fill(9), false, false}}, {}};
    tx::Instruction b{std::string(tx::COMPUTE_BUDGET_PROGRAM_ID), {{key(5), true, false}}, {}};
    Result<std::vector<uint8_t>> m = tx::compile_message(fill(9), {a, b}, fill(7));
    SCFG_CHECK(t, m.ok() && m.value().size() > 3 && m.value()[0] == 2 && m.value()[1] == 0 && m.value()[2] == 1);
}

SCFG_TEST(SolanaTxTests, UnsignedTransaction) {
    SCFG_CHECK_EQ(t, hex(tx::unsigned_transaction(std::vector<uint8_t>{})), "IndexError: index out of range");
    SCFG_CHECK_EQ(t, hex(tx::unsigned_transaction(std::vector<uint8_t>{0})), "0000");
    Result<std::vector<uint8_t>> two = tx::unsigned_transaction(std::vector<uint8_t>{2, 0, 0});
    SCFG_CHECK_EQ(t, hex(two), "02" + std::string(256, '0') + "020000");
    // 255 signers: a two-byte count.
    Result<std::vector<uint8_t>> many = tx::unsigned_transaction(std::vector<uint8_t>{255});
    SCFG_CHECK(t, many.ok() && many.value().size() == 2 + 64 * 255 + 1 && many.value()[0] == 0xff && many.value()[1] == 0x01);
}
