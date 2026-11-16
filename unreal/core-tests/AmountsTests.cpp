// No Python twin: tests of scfg::amounts beyond tier1/amounts.json (the Decimal-typed overloads, the error classes
// and messages, the decode precision boundary), ported from unity/core-tests/DecTests.cs. Every expectation is what
// python3 gives for plugin/game/python-packages/scfg_core/amounts.py.
#include "support/Test.h"

#include "Scfg/Amounts.h"
#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"

#include <limits>
#include <string>

using scfg::BigInt;
using scfg::Dec;
using scfg::Json;
using scfg::Result;
namespace amounts = scfg::amounts;

static Dec D(std::string_view s) { return *Dec::parse(s).value(); }

// str(result), or "<Kind>: <message>" for an Err.
template <class T>
static std::string shown(const Result<T>& r) {
    if (!r.ok()) return r.err().kind + ": " + r.err().message;
    if constexpr (std::is_same_v<T, Dec>) {
        return r.value().str();
    } else {
        return r.value().to_decimal();
    }
}

SCFG_TEST(AmountsTests, Constants) {
    SCFG_CHECK_EQ(t, amounts::TOKEN_DECIMALS, 6);
    SCFG_CHECK_EQ(t, amounts::MAX_AMOUNT.str(), "1000000");
    SCFG_CHECK_EQ(t, amounts::AMOUNT_EXPONENT_LIMIT, 40);
    SCFG_CHECK_EQ(t, amounts::AMOUNT_TEXT_LIMIT, 100);
}

SCFG_TEST(AmountsTests, ParseAmountTakesADecAndRefusesOtherTypes) {
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(D("4.9"))), "4.90");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(3)), "3.00");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(Json(BigInt(3)))), "3.00");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(" 4.99 ")), "4.99");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(Json())), "TypeError: unsupported amount type: NoneType");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(Json::dict())), "TypeError: unsupported amount type: dict");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(5.0)), "TypeError: amounts must be str, int or Decimal (never float)");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(true)), "TypeError: amounts must be str, int or Decimal (never float)");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(D("1E+41"))), "ValueError: amount out of range");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount("1E-41")), "ValueError: amount out of range");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount("NaN")), "ValueError: amount must be finite");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(D("-0"))), "ValueError: amount must be positive");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(D("0.001"))), "ValueError: amount must have at most 2 decimals");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount("1_0")), "ValueError: not a number: '1_0'");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount("abc")), "ValueError: not a number: 'abc'");
    SCFG_CHECK_EQ(t, shown(amounts::parse_amount(std::string(101, 'x'))),
                  "ValueError: amount text longer than 100 characters");
}

SCFG_TEST(AmountsTests, StoredAmountReadsWhatDecimalTakes) {
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(Json())), "TypeError: conversion from NoneType to Decimal is not supported");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(Json::dict())), "TypeError: conversion from dict to Decimal is not supported");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(Json::list({1}))), "TypeError: a stored amount is a number, not a list");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount("1E+41")), "ValueError: amount out of range: '1E+41'");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(std::numeric_limits<double>::infinity())),
                  "ValueError: amount out of range: inf");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(Json(BigInt(10).pow(50)))),
                  "ValueError: amount out of range: 100000000000000000000000000000000000000000000000000");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount("abc")), "ValueError: not a number: 'abc'");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount("1_000")), "1000");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(true)), "1");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(1.5)), "1.5");
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(std::string(101, 'x'))),
                  "ValueError: amount text longer than 100 characters");
    // the text limit counts code points, as Python's len(): 60 astral digits are 240 bytes
    std::string astral;
    for (int k = 0; k < 60; ++k) astral += "\xF0\x9D\x9F\x8E";  // MATHEMATICAL BOLD DIGIT ZERO
    SCFG_CHECK_EQ(t, shown(amounts::stored_amount(astral)), "0");
}

SCFG_TEST(AmountsTests, EncodeChecksItsFields) {
    SCFG_CHECK_EQ(t, shown(amounts::encode(D("4.99"), 37, 42)), "4993742");
    SCFG_CHECK_EQ(t, shown(amounts::encode("1000000", 99, 99)), "1000000009999");
    for (const Json& code : {Json(1.0), Json(), Json(100), Json(-1), Json(true), Json(BigInt(2).pow(70))}) {
        SCFG_CHECK_EQ(t, shown(amounts::encode("1", code, 0)), "ValueError: code must be an int in 0..99");
    }
    SCFG_CHECK_EQ(t, shown(amounts::encode("1", 0, "1")), "ValueError: nonce must be an int in 0..99");
    // the price is checked first
    SCFG_CHECK_EQ(t, shown(amounts::encode(4.99, 100, 0)), "TypeError: amounts must be str, int or Decimal (never float)");
}

SCFG_TEST(AmountsTests, DecodeBoundaryMatchesPythonDecimalPrecision) {
    BigInt ten32 = BigInt(10).pow(32);
    Result<amounts::Decoded> top = amounts::decode(ten32 - 1);
    SCFG_CHECK(t, top.ok() && top.value().price.str() == "99999999999999999999999999.99" && top.value().code == 99 &&
                      top.value().nonce == 99);
    Result<amounts::Decoded> mid = amounts::decode(BigInt(10).pow(31));
    SCFG_CHECK(t, mid.ok() && mid.value().price.str() == "10000000000000000000000000.00");
    Result<amounts::Decoded> zero = amounts::decode(0);
    SCFG_CHECK(t, zero.ok() && zero.value().price.str() == "0.00" && zero.value().code == 0 && zero.value().nonce == 0);
    for (const BigInt& big : {ten32, ten32 + 12345, BigInt(10).pow(40)}) {
        Result<amounts::Decoded> r = amounts::decode(big);
        t.check(!r.ok() && r.err().is_a("InvalidOperation") && !r.err().is_a("ValueError"),
                big.to_decimal() + " decoded or raised the wrong class");
    }
    Result<amounts::Decoded> neg = amounts::decode(-1);
    SCFG_CHECK(t, !neg.ok() && neg.err().kind == "ValueError" && neg.err().message == "units must be >= 0");
}

SCFG_TEST(AmountsTests, DecimalToUnitsIsExactWithinTheRange) {
    SCFG_CHECK_EQ(t, shown(amounts::decimal_to_units(D("1E+41"))), "ValueError: amount out of range: 1E+41");
    SCFG_CHECK_EQ(t, shown(amounts::decimal_to_units(D("0.0000001"))), "ValueError: more than 6 decimals");
    SCFG_CHECK_EQ(t, shown(amounts::decimal_to_units(D("-0"))), "0");
    SCFG_CHECK_EQ(t, shown(amounts::decimal_to_units(D("0E-1000000000000000000"))), "0");
    SCFG_CHECK_EQ(t, shown(amounts::decimal_to_units(D("1.2345678901234567890123456789012345E+30"))),
                  "1234567890123456789012345678901234500");
}

SCFG_TEST(AmountsTests, UnitsToDecimalAndFormatUnitsAgree) {
    SCFG_CHECK_EQ(t, amounts::units_to_decimal(4990000).str(), "4.990000");
    SCFG_CHECK_EQ(t, amounts::units_to_decimal(0).str(), "0.000000");
    SCFG_CHECK_EQ(t, amounts::units_to_decimal(-5000000).str(), "-5.000000");
    for (int64_t u : {int64_t(0), int64_t(1), int64_t(-5), int64_t(999999), int64_t(4993742), INT64_MAX, INT64_MIN}) {
        SCFG_CHECK_EQ(t, amounts::format_units(u), amounts::units_to_decimal(u).str());
    }
    SCFG_CHECK_EQ(t, amounts::format_units(-5), "-0.000005");
    SCFG_CHECK_EQ(t, amounts::format_units(BigInt(10).pow(30) + 1), "1000000000000000000000000.000001");
}
