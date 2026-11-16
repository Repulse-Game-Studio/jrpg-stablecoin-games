// No Python twin: tests of Dec beyond the vectors, ported from unity/core-tests/DecTests.cs (itself from
// rpgmaker/src/core/decimal.test.ts) plus the int64 exponent the C# port clamped. Every expected string is
// CPython 3.12's str(Decimal(...)) under the default context, checked with python3.
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"

#include <cmath>
#include <limits>
#include <string>

using scfg::BigInt;
using scfg::Dec;
using scfg::Json;
using scfg::Result;

// Decimal(s), which must be finite (an invalid or non-finite literal aborts the test binary's check with 0).
static Dec D(scfg_test::Ctx& t, std::string_view s) {
    Result<std::optional<Dec>> r = Dec::parse(s);
    if (!r.ok()) {
        t.fail(std::string(s) + " raised " + r.err().str());
        return Dec();
    }
    if (!r.value().has_value()) {
        t.fail(std::string(s) + " is not finite");
        return Dec();
    }
    return *r.value();
}

static std::string add_str(scfg_test::Ctx& t, std::string_view a, std::string_view b) {
    Result<Dec> r = D(t, a) + D(t, b);
    return r.ok() ? r.value().str() : "raised " + r.err().kind;
}

SCFG_TEST(DecTests, StrMatchesPython) {
    const char* cases[][2] = {
        {"4.99", "4.99"}, {"1E+1", "1E+1"}, {"0.0000001", "1E-7"}, {"1_000", "1000"}, {"-0", "-0"},
        {"0E-7", "0E-7"}, {"0E+3", "0E+3"}, {"123E5", "1.23E+7"}, {"0.000001", "0.000001"}, {"-0.00", "-0.00"},
        {"007", "7"}, {"5.", "5"}, {"+.5", "0.5"}, {"1_0.5_5", "10.55"}, {" 5", "5"}, {" 5\n", "5"},
        {"\xEF\xBC\x95", "5"},                      // fullwidth digit five
        {"\xD9\xA1\xD9\xA2", "12"},                 // Arabic-Indic digits
        {"\xF0\x9D\x9F\x8E\xF0\x9D\x9F\x8F", "1"},  // mathematical bold 0 and 1 (astral)
        {"\xC2\xA0" "5\xE3\x80\x80", "5"},          // U+00A0 and U+3000 stripped
        {"1e5_0", "1E+50"}, {"1__0", "10"}, {"_1", "1"}, {"1e0000000000000000000000000000005", "1E+5"},
        {"4.990000", "4.990000"}, {"0.000000", "0.000000"}, {"0E-999999", "0E-999999"}, {"-1.5E-40", "-1.5E-40"},
        {"1.e1", "1E+1"}, {"0.00000123", "0.00000123"}, {"123.456E-10", "1.23456E-8"},
        {"-123456789012345678901234567890.5", "-123456789012345678901234567890.5"},
        // exponents far beyond an int: no clamp
        {"0E-1000000000000000000", "0E-1000000000000000000"}, {"1E+999999999999999999", "1E+999999999999999999"},
        {"12E+999999999999999998", "1.2E+999999999999999999"}, {"1E-1999999999999999997", "1E-1999999999999999997"},
        {"-0E+999999999999999999", "-0E+999999999999999999"},
    };
    for (auto& c : cases) SCFG_CHECK_EQ(t, D(t, c[0]).str(), c[1]);
}

SCFG_TEST(DecTests, NonFiniteIsNullopt) {
    for (const char* s : {"nan", "-NaN", "sNaN", "NaN12", "snan7", "inf", "+Inf", "-Infinity", "INFINITY", "in_f",
                          "NaN1111111111111111111111111111111111111111", " Infinity\n"}) {
        Result<std::optional<Dec>> r = Dec::parse(s);
        t.check(r.ok() && !r.value().has_value(), std::string(s) + " should be non-finite");
    }
}

SCFG_TEST(DecTests, MalformedIsInvalidOperation) {
    for (const char* s : {"", " ", ".", "1e", "1E+", "abc", "0x10", "infin", "1.2.3", "--1", "1 2", "e5", "NaN1x", "_",
                          "1,5", "\xC4\xB1nf",  // dotless i: no locale casing
                          "1E+1000000000000000000", "1E-2000000000000000000", "1E+100000000000000000000000",
                          "1E-1999999999999999998", "12E+999999999999999999", "0.1E-1999999999999999997",
                          "1E+99999999999999999999", "0E+1000000000000000000", "+-1", "1e+-1", "Infinityy", "_ 5",
                          "1e1.5", "1\xC2\xA0" "2", "\xEF\xBB\xBF" "5", "\xFF" "5"}) {
        Result<std::optional<Dec>> r = Dec::parse(s);
        if (!t.check(!r.ok(), std::string("accepted ") + s)) continue;
        t.check(r.err().is_a("InvalidOperation") && r.err().is_a("ArithmeticError") && !r.err().is_a("ValueError"),
                std::string(s) + " raised " + r.err().str());
    }
    // a NUL inside is no digit either (the C decimal refuses it)
    SCFG_CHECK(t, !Dec::parse(std::string_view("5\0", 2)).ok());
}

SCFG_TEST(DecTests, LiteralExponentLimitsAreLibmpdecs) {
    for (const char* s : {"1E-1999999999999999997", "1E+999999999999999999", "12E+999999999999999998",
                          "0.1E-1999999999999999996", "0E+999999999999999999", "00E+999999999999999999"}) {
        Result<std::optional<Dec>> r = Dec::parse(s);
        t.check(r.ok() && r.value().has_value(), std::string("refused ") + s);
    }
}

SCFG_TEST(DecTests, ExponentsBeyondAnIntAreExactNotClamped) {
    SCFG_CHECK_EQ(t, D(t, "1E+9999999999999999").exp(), 9999999999999999);
    SCFG_CHECK_EQ(t, D(t, "1E-9999999999999999").exp(), -9999999999999999);
    SCFG_CHECK_EQ(t, D(t, "0E-1000000000000000000").exp(), -1000000000000000000);
    SCFG_CHECK_EQ(t, D(t, "12E+999999999999999998").adjusted(), 999999999999999999);
    SCFG_CHECK(t, D(t, "1E+9999999999999999") > D(t, "1E+999999"));
    SCFG_CHECK(t, D(t, "1E+9999999999999999") < D(t, "1E+99999999999999999"));  // ordered, not equal as clamped
    SCFG_CHECK(t, D(t, "1E-9999999999999999") < D(t, "1E-999999"));
    SCFG_CHECK(t, D(t, "1E-9999999999999999") > D(t, "0"));
    SCFG_CHECK(t, D(t, "-1E+9999999999999999") < D(t, "-1E+999999"));
}

SCFG_TEST(DecTests, FloatConversionIsExactLikePython) {
    SCFG_CHECK_EQ(t, Dec::from_double(0.1)->str(), "0.1000000000000000055511151231257827021181583404541015625");
    SCFG_CHECK_EQ(t, Dec::from_double(1.5)->str(), "1.5");
    SCFG_CHECK_EQ(t, Dec::from_double(-2.5)->str(), "-2.5");
    SCFG_CHECK_EQ(t, Dec::from_double(4)->str(), "4");
    SCFG_CHECK_EQ(t, Dec::from_double(-7)->str(), "-7");
    SCFG_CHECK_EQ(t, Dec::from_double(-0.0)->str(), "-0");
    SCFG_CHECK_EQ(t, Dec::from_double(1e22)->str(), "10000000000000000000000");
    SCFG_CHECK_EQ(t, Dec::from_double(5e-324)->adjusted(), -324);
    SCFG_CHECK(t, Dec::from_double(5e-324)->str().rfind("4.940656458412465441765687928682213723650598026143247644255856825006755072702087518652998363616359923797965646954457177309266567103559397963987747960107818781263007131903114045278458171678489821036887186360569987307230500063874091535649843873124733972731696151400317153853980741262385655911710266585566867681870395603106249319452715914924553293054565444011274801297099995419319894090804165633245247571478690147267801593552386115501348035264934720193790268107107491703332226844753335720832431936092382893458368060106011506169809753078342277318329247904982524730776375927247874656084778203734469699533647017972677717585125660551199131504891101451037862738167250955837389733598993664809941164205702637090279242767544565229087538682506419718265533447265625E-324", 0) == 0);
    SCFG_CHECK(t, !Dec::from_double(std::numeric_limits<double>::quiet_NaN()).has_value());
    SCFG_CHECK(t, !Dec::from_double(std::numeric_limits<double>::infinity()).has_value());
    SCFG_CHECK(t, !Dec::from_double(-std::numeric_limits<double>::infinity()).has_value());
}

SCFG_TEST(DecTests, FromJsonIsDecimalOfAJsonValue) {
    auto str_of = [&t](const Json& v) -> std::string {
        Result<std::optional<Dec>> r = Dec::from_json(v);
        if (!r.ok()) return "raised " + r.err().kind;
        return r.value().has_value() ? r.value()->str() : "non-finite";
    };
    SCFG_CHECK_EQ(t, str_of(Json(" 4.990000 ")), "4.990000");
    SCFG_CHECK_EQ(t, str_of(Json(-3)), "-3");
    SCFG_CHECK_EQ(t, str_of(Json(BigInt::from_decimal("123456789012345678901234567890").value())), "123456789012345678901234567890");
    SCFG_CHECK_EQ(t, str_of(Json(true)), "1");
    SCFG_CHECK_EQ(t, str_of(Json(false)), "0");
    SCFG_CHECK_EQ(t, str_of(Json(1.5)), "1.5");
    SCFG_CHECK_EQ(t, str_of(Json(std::numeric_limits<double>::infinity())), "non-finite");
    SCFG_CHECK_EQ(t, str_of(Json("NaN")), "non-finite");
    SCFG_CHECK_EQ(t, str_of(Json("abc")), "raised InvalidOperation");
    SCFG_CHECK_EQ(t, str_of(Json()), "raised TypeError");
    SCFG_CHECK_EQ(t, str_of(Json::dict()), "raised TypeError");
    SCFG_CHECK_EQ(t, str_of(Json::list({0, Json::list({2, 5}), -1})), "raised TypeError");
    Result<std::optional<Dec>> none = Dec::from_json(Json());
    SCFG_CHECK_EQ(t, none.err().message, "conversion from NoneType to Decimal is not supported");
}

SCFG_TEST(DecTests, QuantizeRoundsHalfEvenAndKeepsSign) {
    struct Case {
        const char* text;
        int64_t exp;
        const char* want;
    };
    for (const Case& c : {Case{"-0.001", -2, "-0.00"}, Case{"0.005", -2, "0.00"}, Case{"0.015", -2, "0.02"},
                          Case{"0.025", -2, "0.02"}, Case{"1E+1", -2, "10.00"}, Case{"0.999", -2, "1.00"},
                          Case{"1E-30", -2, "0.00"}, Case{"0E+30", -2, "0.00"}, Case{"0.5", 0, "0"}, Case{"1.5", 0, "2"},
                          Case{"2.5", 0, "2"}, Case{"15", 1, "2E+1"}, Case{"25", 1, "2E+1"},
                          Case{"999999999999999999999999999.5", 0, "1000000000000000000000000000"},
                          Case{"-0", -2, "-0.00"}, Case{"4.990", -2, "4.99"}}) {
        Result<Dec> q = D(t, c.text).quantize(c.exp);
        SCFG_CHECK_EQ(t, q.ok() ? q.value().str() : "raised " + q.err().str(), c.want);
    }
}

SCFG_TEST(DecTests, QuantizeBeyondThePrecisionIsInvalidOperation) {
    for (const char* s : {"1E+30", "999999999999999999999999999", "9999999999999999999999999999.5", "1E+999999999999999999"}) {
        Result<Dec> q = D(t, s).quantize(-2);
        if (!t.check(!q.ok(), std::string("quantized ") + s)) continue;
        t.check(q.err().is_a("InvalidOperation") && !q.err().is_a("ValueError"), q.err().str());
    }
}

SCFG_TEST(DecTests, ComparesByValue) {
    SCFG_CHECK(t, D(t, "1.0") == D(t, "1.00"));
    SCFG_CHECK(t, D(t, "-0") == D(t, "0"));
    SCFG_CHECK(t, D(t, "0E+5") == D(t, "-0.00"));
    SCFG_CHECK(t, D(t, "-1") < D(t, "0"));
    SCFG_CHECK(t, D(t, "1E+1") > D(t, "9.99"));
    SCFG_CHECK(t, D(t, "1E-7") > D(t, "0"));
    SCFG_CHECK(t, D(t, "1E+30") > D(t, "1E+29"));
    SCFG_CHECK(t, D(t, "-2") > D(t, "-10"));
    SCFG_CHECK(t, D(t, "4.99") > D(t, "4.980000"));
    SCFG_CHECK(t, D(t, "1") != D(t, "1.01"));
    SCFG_CHECK(t, D(t, "1") < D(t, "1.01") && D(t, "1.01") >= D(t, "1.010") && D(t, "-5") <= D(t, "-5"));
    SCFG_CHECK(t, D(t, "0.00").is_zero() && D(t, "-0E+5").is_zero() && !D(t, "1E-9").is_zero());
    SCFG_CHECK(t, D(t, "-0").negative() && !D(t, "0").negative());
    SCFG_CHECK(t, Dec() == D(t, "0") && Dec().str() == "0");
}

SCFG_TEST(DecTests, IntegralValue) {
    auto iv = [&t](std::string_view s) -> std::string {
        std::optional<BigInt> v = D(t, s).integral_value();
        return v ? v->to_decimal() : "none";
    };
    SCFG_CHECK_EQ(t, iv("5.000"), "5");
    SCFG_CHECK_EQ(t, iv("1E+3"), "1000");
    SCFG_CHECK_EQ(t, iv("-12.0"), "-12");
    SCFG_CHECK_EQ(t, iv("5.5"), "none");
    SCFG_CHECK_EQ(t, iv("0.0000001"), "none");
    SCFG_CHECK_EQ(t, iv("-0"), "0");
    SCFG_CHECK_EQ(t, iv("0E+999999"), "0");
    SCFG_CHECK_EQ(t, iv("0E-1000000000000000000"), "0");
    SCFG_CHECK_EQ(t, iv("1E-1000000000000000000"), "none");
}

SCFG_TEST(DecTests, MovePointKeepsTheDigits) {
    SCFG_CHECK_EQ(t, D(t, "4993742").move_point(-6).str(), "4.993742");
    SCFG_CHECK_EQ(t, D(t, "0").move_point(-6).str(), "0.000000");
    SCFG_CHECK_EQ(t, D(t, "-5").move_point(-6).str(), "-0.000005");
    SCFG_CHECK_EQ(t, D(t, "4.99").move_point(6).str(), "4.99E+6");  // Decimal((0, (4, 9, 9), 4))
    SCFG_CHECK_EQ(t, D(t, "1.2345678901234567890123456789012345E+30").move_point(6).str(),
                  "1.2345678901234567890123456789012345E+36");  // exact: no rounding to 28 digits
}

// Decimal(a) + Decimal(b) under the default context, as CPython prints it.
SCFG_TEST(DecTests, AddMatchesPythonContextAddition) {
    const char* cases[][3] = {
        {"1.5", "0", "1.5"}, {"1.000000", "0.5", "1.500000"}, {"0.00", "0", "0.00"}, {"-0", "-0", "-0"},
        {"1", "-1", "0"}, {"0", "-1E-1000030", "-0E-1000026"}, {"-0", "1E-1000030", "0E-1000026"},
        {"0", "-0E-5", "0.00000"}, {"1.0", "-1.0", "0.0"}, {"0E+5", "0E+3", "0E+3"},
        {"0", "12345678901234567890123456789", "1.234567890123456789012345679E+28"}, {"0", "0E+1000005", "0"},
        {"9.999999999999999999999999999E+999999", "0.0000000000000000000000000004E+999999",
         "9.999999999999999999999999999E+999999"},
        {"0", "5E-1000027", "0E-1000026"}, {"0", "5.1E-1000027", "1E-1000026"}, {"0", "15E-1000027", "2E-1000026"},
        {"0", "25E-1000027", "2E-1000026"}, {"1E-1000026", "-1E-1000026", "0E-1000026"},
        {"1E+200000", "1", "1.000000000000000000000000000E+200000"},
        {"1", "1E+200000", "1.000000000000000000000000000E+200000"}, {"4.25", "0.000001", "4.250001"},
        {"-7", "2.5", "-4.5"}, {"10000000000000000000000000000", "1", "1.000000000000000000000000000E+28"},
        {"10000000000000000000000000000", "5", "1.000000000000000000000000000E+28"},
        {"10000000000000000000000000000", "15", "1.000000000000000000000000002E+28"},
        {"9999999999999999999999999999", "5", "1.000000000000000000000000000E+28"},
        {"0E-100", "1", "1.000000000000000000000000000"}, {"4.990000", "0.010000", "5.000000"},
        // operands whose exponents are far apart cost nothing
        {"1E-1000000000000000000", "1", "1.000000000000000000000000000"}, {"0", "0E-1000000000000000000", "0E-1000026"},
        {"-0", "-0E+999999999999999999", "-0"}, {"1E+40", "-1E-40", "1.000000000000000000000000000E+40"},
    };
    for (auto& c : cases) SCFG_CHECK_EQ(t, add_str(t, c[0], c[1]), c[2]);
}

SCFG_TEST(DecTests, AddAboveEmaxIsOverflow) {
    const char* cases[][2] = {{"1E+999999", "9E+999999"},
                              {"9.999999999999999999999999999E+999999", "0.0000000000000000000000000005E+999999"},
                              {"0", "1E+1000000"}, {"1E+999999999999999999", "1"}, {"1", "-1E+999999999999999999"}};
    for (auto& c : cases) {
        Result<Dec> r = D(t, c[0]) + D(t, c[1]);
        if (!t.check(!r.ok(), std::string(c[0]) + " + " + c[1] + " did not overflow")) continue;
        t.check(r.err().is_a("Overflow") && r.err().is_a("ArithmeticError") && !r.err().is_a("ValueError"), r.err().str());
    }
}
