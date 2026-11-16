// No Python twin: tests of scfg::ints (twin of scfg_core/ints.py). Expected values are ints.py's own outputs
// (python3 -c "from scfg_core import ints; print(ints.digit_run_value(...))").
#include "support/Test.h"

#include "Scfg/Ints.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>
#include <string>

using scfg::BigInt;
using scfg::Json;
using scfg_test::Ctx;
namespace ints = scfg::ints;

static std::string repeat(std::string_view s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}
// digit_run_value(run) as text: the number, "None", or "<Kind: message>".
static std::string run_value(std::string_view run) {
    scfg::Result<std::optional<int64_t>> r = ints::digit_run_value(run);
    if (!r.ok()) return "<" + r.err().str() + ">";
    return r.value() ? std::to_string(*r.value()) : "None";
}

SCFG_TEST(IntsTests, Limits) {
    SCFG_CHECK_EQ(t, ints::INT_MAX_STR_DIGITS, 4300);
    SCFG_CHECK_EQ(t, ints::DIGIT_RUN_LIMIT, 18);
}

SCFG_TEST(IntsTests, DigitRunValue) {
    struct Case {
        std::string run, want;
    };
    const Case cases[] = {
        {"111111111111111111", "111111111111111111"},  // 18 significant digits
        {"999999999999999999", "999999999999999999"},
        {"1111111111111111111", "None"},  // 19
        {"0100000000000000000", "100000000000000000"},  // 19 characters, 18 significant
        {"000000000000000000", "0"},
        {"", "0"},
        {"7", "7"},
        {"\xD9\xA1\xD9\xA2", "12"},  // Arabic-Indic 12
    };
    for (const Case& c : cases) SCFG_CHECK_EQ(t, run_value(c.run), c.want);
}

SCFG_TEST(IntsTests, DigitRunLeadingZerosAreFree) {
    SCFG_CHECK_EQ(t, run_value(std::string(100, '0') + std::string(18, '1')), "111111111111111111");
    SCFG_CHECK_EQ(t, run_value(std::string(100, '0') + std::string(19, '1')), "None");
    SCFG_CHECK_EQ(t, run_value(std::string(100, '0')), "0");
    // zeros of any script: Arabic-Indic, a mix of scripts, mathematical bold digits
    SCFG_CHECK_EQ(t, run_value(repeat("\xD9\xA0", 30) + "\xD9\xA5"), "5");
    SCFG_CHECK_EQ(t, run_value(repeat("0\xD9\xA0", 20) + "7"), "7");
    SCFG_CHECK_EQ(t, run_value(repeat(scfg::py::utf8_encode(0x1D7CE), 30) + scfg::py::utf8_encode(0x1D7D7)), "9");
    SCFG_CHECK_EQ(t, run_value(std::string(1000000, '9')), "None");
    // a character that is not a digit is int(c)'s ValueError
    SCFG_CHECK_EQ(t, run_value(std::string(19, '0') + "x"), "<ValueError: invalid literal for int() with base 10: 'x'>");
}

SCFG_TEST(IntsTests, IntFromRefusesOverlongDigitStrings) {
    scfg::Result<BigInt> spaced = ints::int_from(Json(std::string(5000, ' ') + "7"));
    SCFG_CHECK(t, spaced.ok() && spaced.value() == 7);
    scfg::Result<BigInt> neg = ints::int_from(Json("-" + std::string(4300, '9')));
    SCFG_CHECK(t, neg.ok() && neg.value() == 1 - BigInt(10).pow(4300));
    scfg::Result<BigInt> over = ints::int_from(Json(std::string(4301, '9')));
    SCFG_CHECK(t, !over.ok());
    if (!over.ok()) {
        SCFG_CHECK_EQ(t, over.err().kind, "ValueError");
        SCFG_CHECK_EQ(t, over.err().message,
                      "Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits; "
                      "use sys.set_int_max_str_digits() to increase the limit");
    }
    SCFG_CHECK(t, !ints::int_from(Json("-" + std::string(4301, '9'))).ok());
    // ints.py counts every decimal character first, so a bad character after 5000 digits is the limit's message
    scfg::Result<BigInt> tail = ints::int_from(Json(std::string(5000, '9') + "x"));
    SCFG_CHECK(t, !tail.ok() && tail.err().message.rfind("Exceeds the limit (4300 digits) for integer string conversion: "
                                                        "value has 5000 digits", 0) == 0);
    scfg::Result<BigInt> letters = ints::int_from(Json(std::string(5000, 'x')));
    SCFG_CHECK(t, !letters.ok() && letters.err().message.rfind("invalid literal for int() with base 10: 'xxx", 0) == 0);
    SCFG_CHECK(t, ints::int_from(Json(12)).ok() && ints::int_from(Json(12)).value() == 12);
    SCFG_CHECK(t, ints::int_from(Json(true)).ok() && ints::int_from(Json(true)).value() == 1);
    SCFG_CHECK(t, ints::int_from(Json(2.5)).ok() && ints::int_from(Json(2.5)).value() == 2);
    SCFG_CHECK(t, !ints::int_from(Json()).ok() && ints::int_from(Json()).err().kind == "TypeError");
}

SCFG_TEST(IntsTests, JsonLoadsReadsIntegersWithIntFrom) {
    scfg::Result<Json> big = ints::json_loads("1" + std::string(4299, '0'));
    SCFG_CHECK(t, big.ok() && big.value().is_int() && big.value().as_bigint() == BigInt(10).pow(4299));
    scfg::Result<Json> f = ints::json_loads("[1.5e3]");
    SCFG_CHECK(t, f.ok() && scfg::json::dumps(f.value()) == "[1500.0]");
    scfg::Result<Json> over = ints::json_loads("[" + std::string(4301, '1') + "]");
    SCFG_CHECK(t, !over.ok() && over.err().message.rfind("Exceeds the limit (4300 digits) for integer string conversion: "
                                                        "value has 4301 digits", 0) == 0);
}
