// Copyright 2026 Repulse
// No Python twin: tests of scfg::solana::base58 and scfg::solana::ed25519 beyond tier1/base58.json and
// tier1/ed25519.json: Python's exact messages, decode_fixed's length bound counted in code points and its float
// arithmetic up to the OverflowError boundary, what normalize_pubkey strips and decode_fixed does not, and the curve
// constants. Every expectation is scfg_core's own output (python3 with plugin/game/python-packages on the path).
#include "support/Helpers.h"
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Bytes.h"
#include "Scfg/Json.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaEd25519.h"

#include <cstdint>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Json;
using scfg::Result;
namespace base58 = scfg::solana::base58;
namespace ed25519 = scfg::solana::ed25519;

static const char* const MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

// The hex of the bytes, or "<Kind>: <message>".
static std::string decoded(const Result<std::vector<uint8_t>>& r) {
    return r.ok() ? scfg::bytes::to_hex(r.value()) : r.err().kind + ": " + r.err().message;
}

static std::string shown(const Result<std::string>& r) { return r.ok() ? r.value() : r.err().kind + ": " + r.err().message; }

using scfg_test::repeat;

static BigInt big(std::string_view decimal) { return BigInt::from_decimal(decimal).value(); }

SCFG_TEST(Base58Tests, EncodeAndDecodeKeepLeadingZeros) {
    std::vector<uint8_t> data = {0, 0, 1};
    SCFG_CHECK_EQ(t, base58::b58encode(data), "112");
    SCFG_CHECK_EQ(t, base58::b58encode(std::vector<uint8_t>{}), "");
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("11112")), "0000000001");
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("1")), "00");
    std::vector<uint8_t> raw = base58::b58decode(MINT).value();
    SCFG_CHECK_EQ(t, raw.size(), 32u);
    SCFG_CHECK_EQ(t, base58::b58encode(raw), MINT);
}

SCFG_TEST(Base58Tests, DecodeErrorsAreValueErrorsWithPythonsRepr) {
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("")), "ValueError: not base58: ''");
    SCFG_CHECK_EQ(t, decoded(base58::b58decode(5)), "ValueError: not base58: 5");
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("0")), "ValueError: invalid base58 character '0'");
    // A lone surrogate (WTF-8) is one character, printed as Python prints it.
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("1\xED\xA0\x80")), "ValueError: invalid base58 character '\\ud800'");
    SCFG_CHECK_EQ(t, decoded(base58::b58decode("1\xC3\xA9")), "ValueError: invalid base58 character '\xC3\xA9'");
}

SCFG_TEST(Base58Tests, DecodeFixedCountsCodePoints) {
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(std::string(45, '1'), 32)),
                  "ValueError: too long for 32 bytes: 45 characters");
    // 45 e-acutes are 90 bytes and 45 characters; 44 pass the bound and fail as base58.
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(repeat("\xC3\xA9", 45), 32)),
                  "ValueError: too long for 32 bytes: 45 characters");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(repeat("\xC3\xA9", 44), 32)),
                  "ValueError: invalid base58 character '\xC3\xA9'");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(std::string(89, '1'), 64)),
                  "ValueError: too long for 64 bytes: 89 characters");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(std::string(32, '1'), 32)), std::string(64, '0'));
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("11", 32)), "ValueError: expected 32 bytes, got 2: '11'");
    // decode_fixed does not strip; normalize_pubkey does.
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(" " + std::string(32, '1'), 32)),
                  "ValueError: invalid base58 character ' '");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed(5, 32)), "ValueError: not base58: 5");
}

SCFG_TEST(Base58Tests, DecodeFixedLengthIsAnIntAsPythonComputesIt) {
    // Python raises ValueError for a length that is not an int (bool included).
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", true)), "ValueError: not a byte count: True");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", 32.0)), "ValueError: not a byte count: 32.0");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", "32")), "ValueError: not a byte count: '32'");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", Json())), "ValueError: not a byte count: None");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("", 0)), "ValueError: not base58: ''");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", 0)), "ValueError: too long for 0 bytes: 1 characters");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("", -1)), "ValueError: too long for -1 bytes: 0 characters");
    BigInt e300 = BigInt(10).pow(300);
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", Json(e300))),
                  "ValueError: expected " + e300.to_decimal() + " bytes, got 1: '1'");
    // The largest length whose float arithmetic does not overflow, and the next one (bisected in Python).
    BigInt last = big(
        "324190371338249677671543549870082794825190139253527938400140927846424292606366884928759009835446666598706670"
        "431734921453013305963749565541560096049539127620969153052827474675223914100053441250453933203477233502000389"
        "14708126174528680046474302248640751823280608725218570677514463865959505410955891125329592319");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", Json(last))),
                  "ValueError: expected " + last.to_decimal() + " bytes, got 1: '1'");
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", Json(last + BigInt(1)))),
                  "ValueError: not a byte count: " + (last + BigInt(1)).to_decimal());
    SCFG_CHECK_EQ(t, decoded(base58::decode_fixed("1", Json(-(last + BigInt(1))))),
                  "ValueError: not a byte count: -" + (last + BigInt(1)).to_decimal());
    SCFG_CHECK(t, base58::max_encoded_length(BigInt(32)) == 44.0);
    SCFG_CHECK(t, base58::max_encoded_length(BigInt(64)) == 88.0);
}

SCFG_TEST(Base58Tests, NormalizeStripsAndChecksTheType) {
    SCFG_CHECK_EQ(t, shown(base58::normalize_pubkey(std::string(" \xE3\x80\x80") + MINT + "\n")), MINT);
    SCFG_CHECK_EQ(t, shown(base58::normalize_pubkey(5)), "ValueError: not an address: 5");
    SCFG_CHECK_EQ(t, shown(base58::normalize_pubkey(std::string(43, '1'))),
                  "ValueError: expected 32 bytes, got 43: '" + std::string(43, '1') + "'");
    SCFG_CHECK_EQ(t, shown(base58::normalize_signature(Json())), "ValueError: not a signature: None");
    SCFG_CHECK_EQ(t, shown(base58::normalize_signature(std::string(64, '1'))), std::string(64, '1'));
}

SCFG_TEST(Ed25519Tests, ConstantsAreCurve25519s) {
    SCFG_CHECK_EQ(t, ed25519::P().to_hex(), "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed");
    SCFG_CHECK_EQ(t, ed25519::D().to_hex(), "52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3");
}

SCFG_TEST(Ed25519Tests, DecompressionAsPython) {
    auto on = [](std::string_view hex) { return ed25519::is_on_curve(scfg::bytes::from_hex(hex).value()); };
    SCFG_CHECK(t, on(std::string(64, '0')));                     // y = 0
    SCFG_CHECK(t, on("01" + std::string(62, '0')));              // y = 1
    SCFG_CHECK(t, !on("02" + std::string(62, '0')));             // y = 2
    SCFG_CHECK(t, !on("02" + std::string(60, '0') + "80"));      // the sign bit is masked
    SCFG_CHECK(t, on(std::string(64, 'f')));                     // non-canonical y >= p is reduced
    SCFG_CHECK(t, on("f2" + std::string(60, 'f') + "7f"));       // p + 5
    SCFG_CHECK(t, on("ec" + std::string(60, 'f') + "7f"));       // p - 1
    SCFG_CHECK(t, ed25519::is_on_curve(base58::b58decode("7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU").value()));
    SCFG_CHECK(t, !ed25519::is_on_curve(base58::b58decode("C4PRXFV6Gf5mytVZb6RoeLsG8CjcFWzR2EJ3dvwPTUJH").value()));
    SCFG_CHECK(t, !ed25519::is_on_curve(std::vector<uint8_t>(31, 0)));
    SCFG_CHECK(t, !ed25519::is_on_curve(std::vector<uint8_t>(33, 0)));
}
