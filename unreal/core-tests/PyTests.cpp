// No Python twin: tests of scfg::py (the CPython 3.9 built-ins the core relies on) and scfg::pyfloat (float repr()
// and parsing). Expected values are CPython's (python3 -c ...); 3.9's wording where it differs from 3.12's.
#include "support/Test.h"

#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Json;
using scfg_test::Ctx;
namespace py = scfg::py;
namespace pyfloat = scfg::pyfloat;

static std::string U(uint32_t cp) { return py::utf8_encode(cp); }
static std::string repeat(std::string_view s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}
// The decimal text of a py_int result, or "<Kind: message>".
static std::string I(const scfg::Result<BigInt>& r) { return r.ok() ? r.value().to_decimal() : "<" + r.err().str() + ">"; }
static std::string kind(const scfg::Result<BigInt>& r) { return r.ok() ? "<ok>" : r.err().kind; }

// Python's str.isspace() set (what str.strip() removes).
static const uint32_t kPySpace[] = {0x09,   0x0a,   0x0b,   0x0c,   0x0d,   0x1c,   0x1d,   0x1e,   0x1f,   0x20,
                                    0x85,   0xa0,   0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006,
                                    0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000};

SCFG_TEST(PyTests, StripMatchesStrStrip) {
    SCFG_CHECK_EQ(t, py::strip("\x1c" + U(0x85) + " x" + U(0x3000)), "x");
    SCFG_CHECK_EQ(t, py::strip(U(0xfeff) + "x"), U(0xfeff) + "x");  // the BOM is not whitespace in Python
    SCFG_CHECK_EQ(t, py::strip("\x1c  x " + U(0x3000) + "\n"), "x");
    SCFG_CHECK_EQ(t, py::strip(U(0x200b) + "x" + U(0x180e)), U(0x200b) + "x" + U(0x180e));
    SCFG_CHECK_EQ(t, py::strip("  "), "");
    SCFG_CHECK_EQ(t, py::strip(""), "");
    SCFG_CHECK_EQ(t, py::strip("a b"), "a b");
    for (uint32_t cp : kPySpace) {
        SCFG_CHECK(t, py::is_space(cp));
        t.check(py::strip(U(cp) + "5" + U(cp)) == "5", "strip of U+" + std::to_string(cp));
    }
    for (uint32_t cp : {0x0u, 0x08u, 0x0eu, 0x1bu, 0x21u, 0x7fu, 0x84u, 0x180eu, 0x200bu, 0xfeffu, 0x2060u, 0x10ffffu})
        t.check(!py::is_space(cp), "is_space(" + std::to_string(cp) + ")");
}

SCFG_TEST(PyTests, DecimalDigitsArePython39s) {
    SCFG_CHECK(t, py::is_decimal_digit(0x0663));
    SCFG_CHECK(t, py::is_decimal_digit(0x1E950));
    // U+1FBF0 (Unicode 13) is a digit in CPython 3.9; U+16AC0 (Tangsa, Unicode 14) is not.
    SCFG_CHECK(t, py::is_decimal_digit(0x1FBF0));
    SCFG_CHECK(t, !py::is_decimal_digit(0x16AC0));
    for (uint32_t zero : {0x10d40u, 0x116d0u, 0x16d70u, 0x16ac0u, 0x11f50u, 0x1e4f0u, 0x1ccf0u})
        t.check(!py::is_decimal_digit(zero + 5), "a Unicode 14+ digit at " + std::to_string(zero + 5));
    SCFG_CHECK(t, py::is_decimal_digit('0') && py::is_decimal_digit('9') && !py::is_decimal_digit('a'));
    SCFG_CHECK(t, !py::is_decimal_digit(0xb2));  // superscript two: isdigit() but not isdecimal()
    SCFG_CHECK_EQ(t, py::decimal_value(0x1d7ff), 9);
    SCFG_CHECK_EQ(t, py::decimal_value(0xff10), 0);
    SCFG_CHECK_EQ(t, py::decimal_value('x'), -1);
    SCFG_CHECK_EQ(t, py::decimal_value(0x0669), 9);
    SCFG_CHECK_EQ(t, py::decimal_value(0x066a), -1);  // just past the Arabic-Indic run
    // 650 decimal digits in Unicode 13.0.0 (sum(1 for c in range(0x110000) if chr(c).isdecimal()) on 3.9)
    size_t n = 0;
    for (uint32_t cp = 0; cp < 0x110000; ++cp) n += py::is_decimal_digit(cp) ? 1 : 0;
    SCFG_CHECK_EQ(t, n, 650);
}

SCFG_TEST(PyTests, LenCountsCodePoints) {
    SCFG_CHECK_EQ(t, py::len(repeat("\xF0\x9F\x98\x80", 60)), 60);
    SCFG_CHECK_EQ(t, py::len(""), 0);
    SCFG_CHECK_EQ(t, py::len("a\xED\xA0\x80" "b"), 3);  // a lone surrogate (WTF-8) is one code point
    SCFG_CHECK_EQ(t, py::len("\xED\xB8\x80\xED\xA0\xBD"), 2);  // reversed halves are not a pair
    SCFG_CHECK_EQ(t, py::len("\xC3\xA9t\xC3\xA9"), 3);
}

SCFG_TEST(PyTests, Utf8Helpers) {
    SCFG_CHECK_EQ(t, U(0x41), "A");
    SCFG_CHECK_EQ(t, U(0xe9), "\xC3\xA9");
    SCFG_CHECK_EQ(t, U(0x20ac), "\xE2\x82\xAC");
    SCFG_CHECK_EQ(t, U(0x1F600), "\xF0\x9F\x98\x80");
    SCFG_CHECK_EQ(t, U(0xd800), "\xED\xA0\x80");  // WTF-8
    std::vector<uint32_t> cps = py::code_points("A\xC3\xA9\xED\xA0\x80\xF0\x9F\x98\x80");
    SCFG_CHECK(t, cps == std::vector<uint32_t>({0x41, 0xe9, 0xd800, 0x1F600}));
    size_t i = 0;
    SCFG_CHECK_EQ(t, py::utf8_decode("\xC3\xA9x", i), 0xe9);
    SCFG_CHECK_EQ(t, i, 2);
    i = 0;
    SCFG_CHECK_EQ(t, py::utf8_decode("\xFF", i), py::REPLACEMENT);  // an invalid byte is U+FFFD, one byte wide
    SCFG_CHECK_EQ(t, i, 1);
    SCFG_CHECK(t, py::utf8_check("ok \xC3\xA9 \xED\xA0\x80").ok());
    SCFG_CHECK(t, !py::utf8_check("\xC3").ok());
    // s.encode("utf-8"): a lone surrogate is a ValueError (UnicodeEncodeError)
    scfg::Result<std::vector<uint8_t>> b = py::utf8_bytes("\xE2\x82\xAC");
    SCFG_CHECK(t, b.ok() && b.value() == std::vector<uint8_t>({0xe2, 0x82, 0xac}));
    scfg::Result<std::vector<uint8_t>> s = py::utf8_bytes("a\xED\xA0\x80");
    SCFG_CHECK(t, !s.ok() && s.err().kind == "ValueError");
    if (!s.ok())
        SCFG_CHECK_EQ(t, s.err().message, "'utf-8' codec can't encode character '\\ud800' in position 1: surrogates not allowed");
}

SCFG_TEST(PyTests, CompareCodePoints) {
    SCFG_CHECK(t, py::compare_code_points("a", "b") < 0);
    SCFG_CHECK(t, py::compare_code_points("b", "a") > 0);
    SCFG_CHECK(t, py::compare_code_points("ab", "ab") == 0);
    SCFG_CHECK(t, py::compare_code_points("a", "ab") < 0);
    SCFG_CHECK(t, py::compare_code_points("", "a") < 0);
    SCFG_CHECK(t, py::compare_code_points("\xEF\xBD\x9E", "\xF0\x9F\x98\x80") < 0);  // U+FF5E < U+1F600
    SCFG_CHECK(t, py::compare_code_points("\x7f", "\xC2\x80") < 0);
    SCFG_CHECK(t, py::compare_code_points("\xED\x9F\xBF", "\xED\xA0\x80") < 0);  // U+D7FF < lone U+D800
    SCFG_CHECK(t, py::compare_code_points("\xED\xBF\xBF", "\xEE\x80\x80") < 0);  // lone U+DFFF < U+E000
}

SCFG_TEST(PyTests, IntMatchesPythonInt) {
    SCFG_CHECK_EQ(t, I(py::py_int(U(0x0663))), "3");
    SCFG_CHECK_EQ(t, I(py::py_int(" 7 ")), "7");
    SCFG_CHECK_EQ(t, I(py::py_int("1_000")), "1000");
    SCFG_CHECK_EQ(t, kind(py::py_int("1__0")), "ValueError");
    SCFG_CHECK_EQ(t, I(py::py_int("-5")), "-5");
    SCFG_CHECK_EQ(t, I(py::py_int("+5")), "5");
    SCFG_CHECK_EQ(t, I(py::py_int("-0")), "0");
    SCFG_CHECK_EQ(t, I(py::py_int("007")), "7");
    SCFG_CHECK_EQ(t, I(py::py_int(U(0xff11) + U(0xff12))), "12");
    SCFG_CHECK_EQ(t, I(py::py_int(U(0x11d57))), "7");
    SCFG_CHECK_EQ(t, I(py::py_int("1" + std::string(30, '0'))), "1" + std::string(30, '0'));
    SCFG_CHECK_EQ(t, I(py::py_int(std::string("9223372036854775808"))), "9223372036854775808");
    for (std::string bad : {"", "abc", "1.5", "_1", "1_", "--1", "0x10", "1e3", "- 1", "   ", "1 2", "+", "-_1",
                            "\xEF\xBB\xBF" "5", "5\xC2\xB2", "\x7f" "5", "\xED\xA0\x80"}) {
        t.check(kind(py::py_int(bad)) == "ValueError", "py_int accepted " + bad);
    }
    SCFG_CHECK_EQ(t, I(py::py_int("x'")), "<ValueError: invalid literal for int() with base 10: \"x'\">");
    // CPython's %.200R: the repr is cut to 200 characters
    std::string msg = I(py::py_int("a" + std::string(300, 'b')));
    SCFG_CHECK_EQ(t, msg, "<ValueError: invalid literal for int() with base 10: 'a" + std::string(198, 'b') + ">");
}

SCFG_TEST(PyTests, IntWhitespaceIsNotStripsWhitespace) {
    // int() skips non-ASCII whitespace but not the ASCII separators \x1c-\x1f that strip() removes
    for (uint32_t cp : kPySpace) {
        bool rejects = cp >= 0x1c && cp <= 0x1f;
        for (const std::string& s : {U(cp) + "5", "5" + U(cp)}) {
            scfg::Result<BigInt> r = py::py_int(s);
            t.check(rejects ? !r.ok() : (r.ok() && r.value() == 5), "py_int around U+" + std::to_string(cp));
        }
    }
    SCFG_CHECK_EQ(t, I(py::py_int(U(0x85) + U(0xa0) + U(0x3000) + " 12\t\n")), "12");
    SCFG_CHECK_EQ(t, I(py::py_int(std::string(100000, ' ') + "7" + std::string(100000, ' '))), "7");
}

SCFG_TEST(PyTests, IntOfOtherValues) {
    SCFG_CHECK_EQ(t, I(py::py_int(Json(5))), "5");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(-1.9))), "-1");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(1.9))), "1");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(1e20))), "100000000000000000000");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(-1e20))), "-100000000000000000000");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(9.223372036854776e18))), "9223372036854775808");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(1.7976931348623157e308))).size(), 309);
    SCFG_CHECK_EQ(t, I(py::py_int(Json(-0.0))), "0");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(true))), "1");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(false))), "0");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(BigInt(2).pow(70)))), BigInt(2).pow(70).to_decimal());
    SCFG_CHECK_EQ(t, I(py::py_int(Json(" 42 "))), "42");
    SCFG_CHECK_EQ(t, I(py::py_int(Json())),
                  "<TypeError: int() argument must be a string, a bytes-like object or a number, not 'NoneType'>");
    SCFG_CHECK_EQ(t, kind(py::py_int(Json::list())), "TypeError");
    SCFG_CHECK_EQ(t, kind(py::py_int(Json::dict())), "TypeError");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(std::nan("")))), "<ValueError: cannot convert float NaN to integer>");
    SCFG_CHECK_EQ(t, I(py::py_int(Json(-std::numeric_limits<double>::infinity()))),
                  "<OverflowError: cannot convert float infinity to integer>");
}

SCFG_TEST(PyTests, IntDigitLimit) {
    SCFG_CHECK(t, py::py_int(std::string(4300, '9')).ok() && py::py_int(std::string(4300, '9')).value() == BigInt(10).pow(4300) - 1);
    SCFG_CHECK_EQ(t, I(py::py_int(" " + repeat("1_", 2000) + "1 ")), std::string(2001, '1'));
    // CPython 3.9.25 counts the digits of the leading run and refuses more than 4300 before it looks further
    SCFG_CHECK_EQ(t, I(py::py_int(std::string(4301, '9'))),
                  "<ValueError: Exceeds the limit (4300) for integer string conversion: value has 4301 digits; "
                  "use sys.set_int_max_str_digits() to increase the limit>");
    SCFG_CHECK(t, I(py::py_int(std::string(5000, '9') + "x")).find("Exceeds the limit (4300)") != std::string::npos);
    SCFG_CHECK(t, I(py::py_int("x" + std::string(5000, '9'))).find("invalid literal") != std::string::npos);
    SCFG_CHECK(t, I(py::py_int("1__" + std::string(5000, '9'))).find("invalid literal") != std::string::npos);
    for (const std::string& text : {std::string(4301, '9'), "-" + std::string(4301, '9'), repeat("1_", 4300) + "1",
                                    std::string(100000, '9'), repeat(U(0x0661), 100000)}) {
        auto start = std::chrono::steady_clock::now();
        t.check(kind(py::py_int(text)) == "ValueError", "py_int accepted " + std::to_string(text.size()) + " bytes");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        t.check(ms < 500, "py_int took " + std::to_string(ms) + " ms");
    }
}

SCFG_TEST(PyTests, TypeNameAndTruthy) {
    for (const Json& f : {Json(), Json(false), Json(0), Json(0.0), Json(-0.0), Json(""), Json::list(), Json::dict()})
        t.check(!py::truthy(f), "truthy(" + py::repr(f) + ")");
    for (const Json& v : {Json(true), Json(1), Json(-1), Json(0.5), Json(std::nan("")), Json(BigInt(2).pow(70)),
                          Json("0"), Json::list({0}), Json::dict({{"a", 0}})})
        t.check(py::truthy(v), "falsy(" + py::repr(v) + ")");
}

SCFG_TEST(PyTests, EqIsPythonEquals) {
    // Python's ==: True == 1 == 1.0, and a NaN is not equal to itself
    SCFG_CHECK(t, py::eq(Json(true), Json(1)));
    SCFG_CHECK(t, py::eq(Json(1), Json(1.0)));
    SCFG_CHECK(t, py::eq(Json(false), Json(0.0)));
    SCFG_CHECK(t, !py::eq(Json(std::nan("")), Json(std::nan(""))));
    SCFG_CHECK(t, py::eq(Json(BigInt(2).pow(70)), Json(std::ldexp(1.0, 70))));
    SCFG_CHECK(t, !py::eq(Json(int64_t{9007199254740993}), Json(9007199254740992.0)));
    SCFG_CHECK(t, !py::eq(Json(1), Json("1")));
    SCFG_CHECK(t, py::eq(Json(), Json()));
    SCFG_CHECK(t, !py::eq(Json(), Json(0)));
    SCFG_CHECK(t, py::eq(Json::dict({{"a", 1}, {"b", Json::list({true})}}), Json::dict({{"b", Json::list({1.0})}, {"a", 1}})));
    SCFG_CHECK(t, !py::eq(Json::dict({{"a", 1}}), Json::dict({{"a", 1}, {"b", 2}})));
    SCFG_CHECK(t, !py::eq(Json::list({1, 2}), Json::list({2, 1})));
    SCFG_CHECK(t, !py::eq(Json::list(), Json::dict()));
    SCFG_CHECK(t, py::eq(Json("\xC3\xA9"), Json("\xC3\xA9")));
}

SCFG_TEST(PyTests, GetIsDictSubscript) {
    Json d = Json::dict({{"a", 1}, {"n", nullptr}});
    scfg::Result<Json> a = py::get(d, "a");
    SCFG_CHECK(t, a.ok() && a.value().as_int64() == 1);
    SCFG_CHECK(t, py::get(d, "n").ok() && py::get(d, "n").value().is_null());
    scfg::Result<Json> missing = py::get(d, "b");
    SCFG_CHECK(t, !missing.ok() && missing.err().kind == "KeyError" && missing.err().message == "'b'");
    SCFG_CHECK_EQ(t, py::key_error("it's").message, "\"it's\"");
    struct Case {
        Json v;
        const char* msg;
    };
    const Case cases[] = {
        {Json(), "'NoneType' object is not subscriptable"},
        {Json::list(), "list indices must be integers or slices, not str"},
        {Json("s"), "string indices must be integers"},
        {Json(5), "'int' object is not subscriptable"},
        {Json(1.5), "'float' object is not subscriptable"},
        {Json(true), "'bool' object is not subscriptable"},
    };
    for (const Case& c : cases) {
        scfg::Result<Json> r = py::get(c.v, "a");
        if (t.check(!r.ok() && r.err().kind == "TypeError", std::string("get on ") + c.msg)) SCFG_CHECK_EQ(t, r.err().message, c.msg);
    }
}

SCFG_TEST(PyTests, QuoteMatchesUrllib) {
    auto q = [&](std::string_view s, std::string_view safe) {
        scfg::Result<std::string> r = py::quote(s, safe);
        return r.ok() ? r.value() : "<" + r.err().str() + ">";
    };
    SCFG_CHECK_EQ(t, q("a b/c~\xC3\xA9\xF0\x9F\x98\x80?&=", "/"), "a%20b/c~%C3%A9%F0%9F%98%80%3F%26%3D");
    SCFG_CHECK_EQ(t, py::quote("a b/c").value(), "a%20b/c");  // safe defaults to "/"
    SCFG_CHECK_EQ(t, q("a b~_.-/\xC3\xA9", ""), "a%20b~_.-%2F%C3%A9");
    SCFG_CHECK_EQ(t, q("\xE6\x97\xA5\xE6\x9C\xAC", ""), "%E6%97%A5%E6%9C%AC");
    SCFG_CHECK_EQ(t, q("!*'()[]#%&?=+\t\n", ""), "%21%2A%27%28%29%5B%5D%23%25%26%3F%3D%2B%09%0A");
    SCFG_CHECK_EQ(t, q("a:b@c", ":@\xC3\xA9"), "a:b@c");
    SCFG_CHECK_EQ(t, q("\xC3\xA9", "\xC3\xA9"), "%C3%A9");  // a non-ASCII safe character is ignored
    SCFG_CHECK(t, !py::quote("\xED\xA0\x80").ok() && py::quote("\xED\xA0\x80").err().kind == "ValueError");
}

SCFG_TEST(PyTests, ReprMatchesPython) {
    struct Case {
        std::string s, want;
    };
    const std::vector<Case> cases = {
        {std::string("x\0", 2), "'x\\x00'"},
        {"a'b", "\"a'b\""},
        {"a\"b", "'a\"b'"},
        {"a'\"b", "'a\\'\"b'"},
        {U(0x85) + U(0xa0) + U(0xad) + U(0x2028) + U(0xfeff) + U(0x1F600) + U(0xd800),
         "'\\x85\\xa0\\xad\\u2028\\ufeff" + U(0x1F600) + "\\ud800'"},
        {"tab\there\\", "'tab\\there\\\\'"},
        {"\xC3\xA9\n\r", "'\xC3\xA9\\n\\r'"},
        {"\x7f", "'\\x7f'"},
        {U(0xe0001) + U(0xf0000), "'\\U000e0001\\U000f0000'"},
    };
    for (const Case& c : cases) SCFG_CHECK_EQ(t, py::repr_str(c.s), c.want);
    Json list = Json::list({1, "a", nullptr, true, 1.5, Json::dict({{"k", Json::list({2})}})});
    SCFG_CHECK_EQ(t, py::repr(list), "[1, 'a', None, True, 1.5, {'k': [2]}]");
    SCFG_CHECK_EQ(t, py::repr(Json(BigInt(-2).pow(71))), "-2361183241434822606848");
    SCFG_CHECK_EQ(t, py::repr(Json(std::nan(""))), "nan");
    SCFG_CHECK_EQ(t, py::repr(Json(1e16)), "1e+16");
    SCFG_CHECK_EQ(t, py::repr(Json::dict()), "{}");
}

SCFG_TEST(PyTests, LowerForAsciiSearch) {
    SCFG_CHECK_EQ(t, py::lower_ascii("ABC xyz \xC3\x89"), "abc xyz \xC3\x89");  // only A-Z
    SCFG_CHECK_EQ(t, py::lower_ascii("I"), "i");
    // Python's lower() as far as an ASCII search can tell: Kelvin is k, U+0130 is "i" + U+0307
    SCFG_CHECK_EQ(t, py::lower_for_ascii("LIMIT " + U(0x212a) + U(0x130) + "\xC3\x89"),
                  "limit k" + std::string("i") + U(0x307) + "\xC3\x89");
    SCFG_CHECK(t, py::lower_for_ascii("L" + U(0x130) + "MIT").find("limit") == std::string::npos);
}

SCFG_TEST(PyTests, Format1MatchesStrFormat) {
    auto f = [](std::string_view tmpl, std::string_view arg) {
        scfg::Result<std::string> r = py::format1(tmpl, arg);
        return r.ok() ? r.value() : "<" + r.err().kind + ">";
    };
    std::string v = "\xC3\xA9'";
    SCFG_CHECK_EQ(t, f("{}x", v), v + "x");
    SCFG_CHECK_EQ(t, f("{0}", v), v);
    SCFG_CHECK_EQ(t, f("{00}", v), v);
    SCFG_CHECK_EQ(t, f("{!r}", v), "\"" + v + "\"");
    SCFG_CHECK_EQ(t, f("{!s}", v), v);
    SCFG_CHECK_EQ(t, f("{!a}", v), "\"\\xe9'\"");
    SCFG_CHECK_EQ(t, f("{0!r:}", "v"), "'v'");
    SCFG_CHECK_EQ(t, f("{{}}", v), "{}");
    SCFG_CHECK_EQ(t, f("a{:}b", v), "a" + v + "b");
    SCFG_CHECK_EQ(t, f("no fields", v), "no fields");
    SCFG_CHECK_EQ(t, f("{1}", v), "<IndexError>");
    SCFG_CHECK_EQ(t, f("{}{}", v), "<IndexError>");
    SCFG_CHECK_EQ(t, f("{name}", v), "<KeyError>");
    SCFG_CHECK_EQ(t, f("{-1}", v), "<KeyError>");
    SCFG_CHECK_EQ(t, f("{", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("}", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("{abc", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("{0}{}", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("{}{0}", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("{!x}", v), "<ValueError>");
    SCFG_CHECK_EQ(t, f("{:>5}", v), "<ValueError>");  // stricter than Python: no format specs
    SCFG_CHECK_EQ(t, f("{0.a}", v), "<ValueError>");  // stricter than Python: no attribute access
    scfg::Result<std::string> k = py::format1("{name}", "v");
    SCFG_CHECK(t, !k.ok() && k.err().message == "'name'");
    scfg::Result<std::string> i = py::format1("{1}", "v");
    SCFG_CHECK(t, !i.ok() && i.err().message == "Replacement index 1 out of range for positional args tuple");
}

// ---- pyfloat ----

SCFG_TEST(PyFloatTests, ReprIsPythons) {
    struct Case {
        double x;
        const char* want;
    };
    const Case cases[] = {
        {1.5, "1.5"},
        {0.1, "0.1"},
        {1e16, "1e+16"},
        {1e15, "1000000000000000.0"},
        {9999999999999998.0, "9999999999999998.0"},
        {1e-4, "0.0001"},
        {1e-5, "1e-05"},
        {0.0, "0.0"},
        {-0.0, "-0.0"},
        {1.0, "1.0"},
        {100.0, "100.0"},
        {-123.456, "-123.456"},
        {0.30000000000000004, "0.30000000000000004"},
        {5e-324, "5e-324"},
        {2.2250738585072014e-308, "2.2250738585072014e-308"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {1e100, "1e+100"},
        {1.5e-300, "1.5e-300"},
        {123456789012345678.0, "1.2345678901234568e+17"},
        {std::numeric_limits<double>::infinity(), "inf"},
        {-std::numeric_limits<double>::infinity(), "-inf"},
        {std::nan(""), "nan"},
        {-std::nan(""), "nan"},
        // powers of two where the correctly rounded 16 digits do not round-trip but their upper neighbour does
        // (python3 -c "print(repr(2.0**-1017), repr(2.0**-1007))"): the shortest search must try neighbours
        {std::ldexp(1.0, -1017), "7.120236347223045e-307"},
        {std::ldexp(1.0, -1007), "7.291122019556398e-304"},
        {std::ldexp(1.0, -957), "8.209073602596753e-289"},
    };
    for (const Case& c : cases) SCFG_CHECK_EQ(t, pyfloat::repr(c.x), c.want);
}

// The shortest digits as std::to_chars finds them (the test's oracle: exact and locale-free), in Python's layout.
static std::string oracle_repr(double x) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string s(buf, res.ptr);  // "-d.ddde+XX"
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    std::string digits = s.substr(0, 1) + (e > 2 ? s.substr(2, e - 2) : "");
    int exp = std::stoi(s.substr(e + 1));
    std::string out = neg ? "-" : "";
    if (exp < -4 || exp >= 16) {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        std::string ex = std::to_string(exp < 0 ? -exp : exp);
        out += std::string("e") + (exp < 0 ? "-" : "+") + (ex.size() < 2 ? "0" : "") + ex;
    } else if (exp < 0) {
        out += "0." + std::string(static_cast<size_t>(-exp - 1), '0') + digits;
    } else if (static_cast<size_t>(exp) + 1 >= digits.size()) {
        out += digits + std::string(static_cast<size_t>(exp) + 1 - digits.size(), '0') + ".0";
    } else {
        out += digits.substr(0, static_cast<size_t>(exp) + 1) + "." + digits.substr(static_cast<size_t>(exp) + 1);
    }
    return out;
}

SCFG_TEST(PyFloatTests, ReprMatchesShortestRoundTripEverywhere) {
    // Every power of two (46 of them need the neighbour search) and 20000 random bit patterns (a fixed xorshift
    // seed), against std::to_chars's shortest representation; each also parses back to itself.
    std::vector<double> xs;
    for (int k = -1074; k <= 1023; ++k) xs.push_back(std::ldexp(1.0, k));
    uint64_t s = 0x9E3779B97F4A7C15u;
    while (xs.size() < 2098 + 20000) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        double x;
        std::memcpy(&x, &s, sizeof x);
        if (std::isfinite(x)) xs.push_back(x);
    }
    size_t bad = 0;
    for (double x : xs) {
        std::string got = pyfloat::repr(x), want = oracle_repr(x);
        std::optional<double> back = pyfloat::parse(got);
        if (got != want || !back || std::memcmp(&*back, &x, sizeof x) != 0) {
            if (++bad <= 5) t.fail("repr " + got + " oracle " + want);
        }
    }
    SCFG_CHECK_EQ(t, bad, 0);
}

SCFG_TEST(PyFloatTests, ParseChecksTheGrammarFirst) {
    struct Case {
        const char* s;
        double want;
    };
    const Case ok[] = {
        {"2.5", 2.5}, {"-2.5", -2.5}, {"+2.5", 2.5}, {"1e16", 1e16}, {"1E+16", 1e16}, {"1e-05", 1e-5},
        {".5", 0.5}, {"5.", 5.0}, {"0", 0.0}, {"00.100", 0.1}, {"1e400", std::numeric_limits<double>::infinity()},
        {"-1e400", -std::numeric_limits<double>::infinity()}, {"1e-400", 0.0}, {"inf", std::numeric_limits<double>::infinity()},
        {"-Infinity", -std::numeric_limits<double>::infinity()}, {"INF", std::numeric_limits<double>::infinity()},
        {"0.30000000000000004", 0.30000000000000004}, {"5e-324", 5e-324},
        {"123456789012345678901234567890", 1.2345678901234568e29},
        {"1e99999999999999999999999", std::numeric_limits<double>::infinity()},
        {"0.000000000000000000000000000000000000001e39", 1.0},
    };
    for (const Case& c : ok) {
        std::optional<double> v = pyfloat::parse(c.s);
        t.check(v.has_value() && *v == c.want, std::string("parse(") + c.s + ")");
    }
    SCFG_CHECK(t, std::isnan(pyfloat::parse("nan").value_or(0.0)));
    SCFG_CHECK(t, std::isnan(pyfloat::parse("-NaN").value_or(0.0)));
    SCFG_CHECK(t, std::signbit(pyfloat::parse("-0.0").value_or(1.0)));
    SCFG_CHECK(t, std::signbit(pyfloat::parse("-1e-400").value_or(1.0)));
    for (const char* bad : {"", " 1", "1 ", "0x1p3", "0x10", "1,5", "1.5.5", "e5", "1e", "1e+", ".", "-", "+-1", "1_0",
                            "infinit", "nanx", "in", "1.5f", "\xD9\xA1", "1e5.5", "--1", "inf inity", "nan(1)"}) {
        t.check(!pyfloat::parse(bad).has_value(), std::string("parse accepted ") + bad);
    }
}
