// No Python twin: tests of scfg::Json (Python's json.loads / json.dumps and its dict and list), deep_copy and
// the test-side json_eq. Expected texts are CPython's own output (python3 -c "import json; ...").
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Vectors.h"

#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Json;
using scfg_test::Ctx;
namespace json = scfg::json;

// loads(s), which the test expects to succeed (a failure is recorded and the value is None).
static Json L(Ctx& t, std::string_view s) {
    scfg::Result<Json> r = json::loads(s);
    if (!t.check(r.ok(), "loads failed on " + std::string(s) + ": " + (r.ok() ? "" : r.err().str()))) return Json();
    return r.take();
}

// The ValueError message of loads(s), or "<ok>" / "<Kind: ...>" when it is not a ValueError.
static std::string loads_error(std::string_view s) {
    scfg::Result<Json> r = json::loads(s);
    if (r.ok()) return "<ok>";
    if (r.err().kind != "ValueError") return "<" + r.err().str() + ">";
    return r.err().message;
}

static std::string D(const Json& v) { return json::dumps(v); }

SCFG_TEST(JsonTests, IntegersStayIntegers) {
    Json big = L(t, "12345678901234567890123");
    SCFG_CHECK(t, big.is_int() && !big.is_int64());
    SCFG_CHECK_EQ(t, big.as_bigint().to_decimal(), "12345678901234567890123");
    SCFG_CHECK_EQ(t, big.type_name(), "int");
    Json one = L(t, "1");
    SCFG_CHECK(t, one.is_int64() && one.as_int64() == 1);
    Json onef = L(t, "1.0");
    SCFG_CHECK(t, onef.is_float() && onef.as_float() == 1.0);
    SCFG_CHECK_EQ(t, onef.type_name(), "float");
    Json min = L(t, "-9223372036854775808");
    SCFG_CHECK(t, min.is_int64() && min.as_int64() == INT64_MIN);
    SCFG_CHECK(t, !L(t, "9223372036854775808").is_int64());
    SCFG_CHECK(t, L(t, "-9223372036854775809").is_int() && !L(t, "-9223372036854775809").is_int64());
    // 19 and more digits that still fit are normalised to int64
    SCFG_CHECK(t, L(t, "9223372036854775807").is_int64());
    SCFG_CHECK(t, L(t, "1e2").is_float() && L(t, "1e2").as_float() == 100.0);
    SCFG_CHECK(t, L(t, "-0").is_int64() && L(t, "-0").as_int64() == 0);
    SCFG_CHECK(t, L(t, "1e400").as_float() == std::numeric_limits<double>::infinity());  // float('1e400') is inf
    SCFG_CHECK(t, L(t, "-1E-400").as_float() == 0.0 && std::signbit(L(t, "-1E-400").as_float()));
    SCFG_CHECK(t, L(t, "2.5E+3").as_float() == 2500.0);
}

SCFG_TEST(JsonTests, IntegerDigitLimit) {
    SCFG_CHECK_EQ(t, loads_error(std::string(4301, '9')),
                  "Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits; "
                  "use sys.set_int_max_str_digits() to increase the limit");
    SCFG_CHECK(t, loads_error("[-" + std::string(4301, '9') + "]").rfind("Exceeds the limit (4300 digits)", 0) == 0);
    Json ok = L(t, std::string(4300, '9'));
    SCFG_CHECK(t, ok.is_int() && ok.as_bigint() == BigInt(10).pow(4300) - 1);
    SCFG_CHECK(t, L(t, "-" + std::string(4300, '9')).as_bigint() == 1 - BigInt(10).pow(4300));
    // a fraction or an exponent makes it a float, which has no digit limit
    SCFG_CHECK(t, L(t, std::string(4301, '9') + ".0").is_float());
    SCFG_CHECK(t, L(t, std::string(4301, '9') + "e-4301").is_float());
}

SCFG_TEST(JsonTests, RoundTripsPythonLayout) {
    std::string src = "{\"b\":1,\"a\":[true,null,\"\xC3\xA9\"]}";
    SCFG_CHECK_EQ(t, D(L(t, src)), "{\"b\": 1, \"a\": [true, null, \"\\u00e9\"]}");
    SCFG_CHECK_EQ(t, json::dumps(L(t, src), {.ensure_ascii = false}), "{\"b\": 1, \"a\": [true, null, \"\xC3\xA9\"]}");
    SCFG_CHECK_EQ(t, json::dumps(L(t, "{\"b\":1,\"a\":[],\"c\":{},\"d\":[1,{\"x\":2}]}"), {.indent = 2}),
                  "{\n  \"b\": 1,\n  \"a\": [],\n  \"c\": {},\n  \"d\": [\n    1,\n    {\n      \"x\": 2\n    }\n  ]\n}");
    SCFG_CHECK_EQ(t, json::dumps(L(t, "{\"b\":1,\"a\":[]}"), {.indent = 0}), "{\n\"b\": 1,\n\"a\": []\n}");
    SCFG_CHECK_EQ(t, json::dumps(L(t, "[1]"), {.indent = -3}), "[\n1\n]");  // ' ' * -3 is ''
    SCFG_CHECK_EQ(t, D(L(t, "[]")), "[]");
    SCFG_CHECK_EQ(t, D(L(t, "{}")), "{}");
    SCFG_CHECK_EQ(t, D(L(t, "[{}, [], [[]]]")), "[{}, [], [[]]]");
    SCFG_CHECK_EQ(t, D(Json()), "null");
    SCFG_CHECK_EQ(t, D(Json(true)), "true");
    SCFG_CHECK_EQ(t, D(Json(-7)), "-7");
    SCFG_CHECK_EQ(t, D(Json(BigInt(2).pow(70))), "1180591620717411303424");
    SCFG_CHECK_EQ(t, D(Json("x")), "\"x\"");
}

SCFG_TEST(JsonTests, SortKeysOrdersByCodePoint) {
    // U+FF5E (EF BD 9E) is below U+1F600 (F0 9F 98 80) by code point, as in Python (UTF-16 order would differ).
    Json o = Json::dict({{"b", 1}, {"B", 2}, {"a", 3}, {"\xEF\xBD\x9E", 4}, {"\xF0\x9F\x98\x80", 5}, {"ab", 6}});
    SCFG_CHECK_EQ(t, json::dumps(o, {.sort_keys = true, .ensure_ascii = false}),
                  "{\"B\": 2, \"a\": 3, \"ab\": 6, \"b\": 1, \"\xEF\xBD\x9E\": 4, \"\xF0\x9F\x98\x80\": 5}");
    SCFG_CHECK_EQ(t, D(o), "{\"b\": 1, \"B\": 2, \"a\": 3, \"\\uff5e\": 4, \"\\ud83d\\ude00\": 5, \"ab\": 6}");
}

SCFG_TEST(JsonTests, FloatsUsePythonRepr) {
    SCFG_CHECK_EQ(t, D(Json(1e16)), "1e+16");
    SCFG_CHECK_EQ(t, D(Json(0.1)), "0.1");
    struct Case {
        double x;
        const char* want;
    };
    const Case cases[] = {
        {1e-5, "1e-05"},
        {1e15, "1000000000000000.0"},
        {0.1 + 0.2, "0.30000000000000004"},
        {-0.0, "-0.0"},
        {1.5e300, "1.5e+300"},
        {123456789.0, "123456789.0"},
        {5e-324, "5e-324"},
        {1e22, "1e+22"},
        {0.0001, "0.0001"},
        {1.0, "1.0"},
        {-2.5, "-2.5"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {123456789012345678.0, "1.2345678901234568e+17"},
        {0.000123, "0.000123"},
        {1234.5e-10, "1.2345e-07"},
    };
    for (const Case& c : cases) SCFG_CHECK_EQ(t, D(Json(c.x)), c.want);
    double inf = std::numeric_limits<double>::infinity();
    SCFG_CHECK_EQ(t, D(Json::list({std::nan(""), inf, -inf})), "[NaN, Infinity, -Infinity]");
}

SCFG_TEST(JsonTests, StringEscapes) {
    // "\x7f\x1f\u2028\U0001F600\"\\\b\f\n\r\t/"
    std::string s = "\x7f\x1f\xE2\x80\xA8\xF0\x9F\x98\x80\"\\\b\f\n\r\t/";
    SCFG_CHECK_EQ(t, D(Json(s)), "\"\\u007f\\u001f\\u2028\\ud83d\\ude00\\\"\\\\\\b\\f\\n\\r\\t/\"");
    SCFG_CHECK_EQ(t, json::dumps(Json("\x7f\x1f\xE2\x80\xA8\xF0\x9F\x98\x80"), {.ensure_ascii = false}),
                  "\"\x7f\\u001f\xE2\x80\xA8\xF0\x9F\x98\x80\"");
    SCFG_CHECK_EQ(t, json::dumps(Json(std::string("a\0b", 3)), {}), "\"a\\u0000b\"");
    Json back = L(t, "\"\\ud83d\\ude00\\u00E9\\/\\b\\f\\n\\r\\t\\\"\\\\\"");
    SCFG_CHECK_EQ(t, back.as_str(), "\xF0\x9F\x98\x80\xC3\xA9/\b\f\n\r\t\"\\");
    // raw UTF-8 passes through loads
    SCFG_CHECK_EQ(t, L(t, "\"\xC3\xA9\xF0\x9F\x98\x80\"").as_str(), "\xC3\xA9\xF0\x9F\x98\x80");
}

SCFG_TEST(JsonTests, LoneSurrogatesSurviveAsWtf8) {
    // Python keeps a lone surrogate in a str; here it is WTF-8 (ED A0 80), and dumps writes it back.
    Json lone = L(t, "\"\\ud800\"");
    SCFG_CHECK_EQ(t, lone.as_str(), "\xED\xA0\x80");
    SCFG_CHECK_EQ(t, D(lone), "\"\\ud800\"");
    SCFG_CHECK_EQ(t, json::dumps(lone, {.ensure_ascii = false}), "\"\xED\xA0\x80\"");
    SCFG_CHECK_EQ(t, scfg::py::len(lone.as_str()), 1);
    // a low half alone, reversed halves (two code points), a high half before a non-surrogate escape
    SCFG_CHECK_EQ(t, D(L(t, "\"\\udc00x\"")), "\"\\udc00x\"");
    SCFG_CHECK_EQ(t, D(L(t, "\"\\ude00\\ud83d\"")), "\"\\ude00\\ud83d\"");
    SCFG_CHECK_EQ(t, scfg::py::len(L(t, "\"\\ude00\\ud83d\"").as_str()), 2);
    SCFG_CHECK_EQ(t, D(L(t, "\"\\ud83d\\u0041\"")), "\"\\ud83dA\"");
    SCFG_CHECK_EQ(t, D(L(t, "\"\\ud83d\\ud83d\\ude00\"")), "\"\\ud83d\\ud83d\\ude00\"");
    // raw WTF-8 surrogate bytes in the input are read as Python's surrogatepass decoding reads them
    SCFG_CHECK_EQ(t, D(L(t, "\"\xED\xB0\x80\"")), "\"\\udc00\"");
}

SCFG_TEST(JsonTests, ErrorMessagesAreJsonDecodeErrors) {
    struct Case {
        std::string text, want;
    };
    const std::vector<Case> cases = {
        {"nan", "Expecting value: line 1 column 1 (char 0)"},
        {"Infinityx", "Extra data: line 1 column 9 (char 8)"},
        {"{\"a\":1,}", "Expecting property name enclosed in double quotes: line 1 column 8 (char 7)"},
        {"[1,]", "Expecting value: line 1 column 4 (char 3)"},
        {"01", "Extra data: line 1 column 2 (char 1)"},
        {"\"\x01\"", "Invalid control character at: line 1 column 2 (char 1)"},
        {"\xEF\xBB\xBF" "1", "Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)"},
        {"\"\\x\"", "Invalid \\escape: line 1 column 2 (char 1)"},
        {"1 2", "Extra data: line 1 column 3 (char 2)"},
        {"", "Expecting value: line 1 column 1 (char 0)"},
        {"   ", "Expecting value: line 1 column 4 (char 3)"},
        {"-", "Expecting value: line 1 column 1 (char 0)"},
        {"[", "Expecting value: line 1 column 2 (char 1)"},
        {"{\"a\" 1}", "Expecting ':' delimiter: line 1 column 6 (char 5)"},
        {"\"\\u12\"", "Invalid \\uXXXX escape: line 1 column 3 (char 2)"},
        {"\"abc", "Unterminated string starting at: line 1 column 1 (char 0)"},
        {"[1.]", "Expecting ',' delimiter: line 1 column 3 (char 2)"},
        {"\"\\ud800\\uZZZZ\"", "Invalid \\uXXXX escape: line 1 column 9 (char 8)"},
        {"\"\\ud800", "Invalid \\uXXXX escape: line 1 column 3 (char 2)"},
        {"\"\\u0041", "Invalid \\uXXXX escape: line 1 column 3 (char 2)"},
        {"\"\\u004", "Invalid \\uXXXX escape: line 1 column 3 (char 2)"},
        // positions count code points: e-acute is two bytes and one character
        {"\"\xC3\xA9\n\" x", "Invalid control character at: line 1 column 3 (char 2)"},
        {"[\n1,\n2 x]", "Expecting ',' delimiter: line 3 column 3 (char 7)"},
    };
    for (const Case& c : cases) SCFG_CHECK_EQ(t, loads_error(c.text), c.want);
}

SCFG_TEST(JsonTests, RejectsWhatJsonLoadsRejects) {
    // python3 -c "import json; json.loads(s)" refuses each of these
    const std::vector<std::string> bad = {
        "{\"a\":1,}", "[1,]", "01", "\"\x01\"", "\"\\x\"", "1 2", "", "   ", "-", "[", "{\"a\" 1}", "\"\\u12\"",
        "\"abc", "nan", "+Infinity", "-NaN", "Infinityx", "NaNx", "-Inf", "Infinit", "[1.]", "[.5]", "[1e]",
        "{1: 2}", "tru", "nul", "[1] x", "\xC2\xA0" "1", "[\xEF\xBC\x91]", "\"\\", "\"\\u", "[1,,2]", "{\"a\":}",
        "{\"a\"", "{\"a\":1", "[1", "-x", "--1", "+1", "0x10", "\t\v1",
    };
    for (const std::string& s : bad) {
        scfg::Result<Json> r = json::loads(s);
        t.check(!r.ok() && r.err().kind == "ValueError", "loads accepted or mis-raised on " + s);
    }
}

SCFG_TEST(JsonTests, InvalidUtf8IsAValueError) {
    // json.loads(bytes) raises UnicodeDecodeError, a ValueError, with the codec's message.
    SCFG_CHECK_EQ(t, loads_error("\"\xFF\""), "'utf-8' codec can't decode byte 0xff in position 1: invalid start byte");
    SCFG_CHECK_EQ(t, loads_error("\"\xC3(\""), "'utf-8' codec can't decode byte 0xc3 in position 1: invalid continuation byte");
    SCFG_CHECK_EQ(t, loads_error("\"\xE2\x82"), "'utf-8' codec can't decode bytes in position 1-2: unexpected end of data");
    SCFG_CHECK_EQ(t, loads_error("\"\xF0\x9F\x98\""), "'utf-8' codec can't decode bytes in position 1-3: invalid continuation byte");
    SCFG_CHECK_EQ(t, loads_error("\"\xED\xA0\""), "'utf-8' codec can't decode byte 0xed in position 1: invalid continuation byte");
    SCFG_CHECK_EQ(t, loads_error("\"\xC0\x80\""), "'utf-8' codec can't decode byte 0xc0 in position 1: invalid start byte");
    SCFG_CHECK_EQ(t, loads_error("\"\xF4\x90\x80\x80\""), "'utf-8' codec can't decode byte 0xf4 in position 1: invalid continuation byte");
    for (std::string bad : {"\xC0\x80", "\xE0\x80\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\x80",
                            "\xED\xA0", "\xF0\x9F\x98"}) {
        scfg::Result<Json> r = json::loads("\"" + bad + "\"");
        t.check(!r.ok() && r.err().kind == "ValueError", "loads accepted invalid UTF-8");
    }
}

SCFG_TEST(JsonTests, ReadsTheConstantsCPythonReads) {
    SCFG_CHECK(t, std::isnan(L(t, "NaN").as_float()));
    SCFG_CHECK(t, L(t, "Infinity").as_float() == std::numeric_limits<double>::infinity());
    SCFG_CHECK(t, L(t, "-Infinity").as_float() == -std::numeric_limits<double>::infinity());
    SCFG_CHECK(t, std::isnan(L(t, " NaN ").as_float()));
    Json list = L(t, "[NaN,Infinity, -Infinity]");
    SCFG_CHECK_EQ(t, D(list), "[NaN, Infinity, -Infinity]");
    SCFG_CHECK(t, std::isnan(L(t, "{\"a\":NaN}").as_dict().get("a").as_float()));
    // and dumps' output reads back
    SCFG_CHECK_EQ(t, D(L(t, D(Json(-std::numeric_limits<double>::infinity())))), "-Infinity");
}

SCFG_TEST(JsonTests, DuplicateKeysKeepFirstPositionLastValue) {
    Json o = L(t, " {\"a\":1,\"b\":3,\"a\":2}\r\n\t");
    SCFG_CHECK_EQ(t, D(o), "{\"a\": 2, \"b\": 3}");
}

SCFG_TEST(JsonTests, DeepNestingIsAValueErrorNotACrash) {
    Json deep = L(t, std::string(1000, '[') + std::string(1000, ']'));
    SCFG_CHECK(t, deep.is_list());
    SCFG_CHECK_EQ(t, json::dumps(deep).size(), 2000);
    std::string err = loads_error(std::string(1001, '[') + std::string(1001, ']'));
    SCFG_CHECK_EQ(t, err, "JSON nested deeper than 1000 levels: line 1 column 1001 (char 1000)");
    SCFG_CHECK_EQ(t, loads_error(std::string(100000, '{')),
                  "Expecting property name enclosed in double quotes: line 1 column 2 (char 1)");
    SCFG_CHECK(t, loads_error(std::string(100000, '[') + std::string(100000, ']')).rfind("JSON nested deeper", 0) == 0);
}

SCFG_TEST(JsonTests, DictKeepsInsertionOrderThePythonWay) {
    Json o = Json::dict();
    Json::Dict& d = o.as_dict();
    d.set("a", 1);
    d.set("b", 2);
    SCFG_CHECK(t, d.erase("a"));
    d.set("a", 3);  // deleted and re-inserted: goes last
    SCFG_CHECK(t, d.keys() == std::vector<std::string>({"b", "a"}));
    SCFG_CHECK_EQ(t, d.size(), 2);
    SCFG_CHECK(t, d.contains("a") && !d.contains("c"));
    SCFG_CHECK(t, d.find("b") != nullptr && d.find("b")->as_int64() == 2);
    SCFG_CHECK(t, d.find("c") == nullptr);
    SCFG_CHECK(t, d.get("c").is_null());
    SCFG_CHECK_EQ(t, d.get("c", "fallback").as_str(), "fallback");
    SCFG_CHECK(t, !d.erase("c"));
    std::string seen;
    for (const auto& [k, v] : d) seen += k + "=" + json::dumps(v) + ";";
    SCFG_CHECK_EQ(t, seen, "b=2;a=3;");
    d.set("b", 4);  // reassigning keeps the position
    SCFG_CHECK_EQ(t, D(o), "{\"b\": 4, \"a\": 3}");
    SCFG_CHECK(t, !d.empty());
    SCFG_CHECK(t, Json::dict().as_dict().empty());
}

SCFG_TEST(JsonTests, DictSurvivesManyRemovals) {
    Json o = Json::dict();
    Json::Dict& d = o.as_dict();
    for (int i = 0; i < 1000; ++i) d.set("k" + std::to_string(i), i);
    for (int i = 0; i < 999; ++i) t.check(d.erase("k" + std::to_string(i)), "erase k" + std::to_string(i));
    SCFG_CHECK(t, d.keys() == std::vector<std::string>({"k999"}));
    d.set("z", Json());
    d.set("k5", 5);
    SCFG_CHECK_EQ(t, D(o), "{\"k999\": 999, \"z\": null, \"k5\": 5}");
    SCFG_CHECK(t, d.find("k999")->as_int64() == 999 && d.find("z")->is_null() && d.find("k5")->as_int64() == 5);
}

SCFG_TEST(JsonTests, ListsAndDictsAreSharedHandles) {
    Json a = L(t, "{\"xs\": [1], \"d\": {\"k\": 1}}");
    Json alias = a;  // as Python's `alias = a`
    Json copy = scfg::deep_copy(a);
    alias.as_dict().find("xs")->as_list().push_back(2);
    Json inner = alias.as_dict().get("d");
    inner.as_dict().set("k", 2);
    SCFG_CHECK_EQ(t, D(a), "{\"xs\": [1, 2], \"d\": {\"k\": 2}}");
    SCFG_CHECK_EQ(t, D(copy), "{\"xs\": [1], \"d\": {\"k\": 1}}");
    // the copy's containers are its own, all the way down
    copy.as_dict().find("d")->as_dict().set("new", true);
    SCFG_CHECK_EQ(t, D(a), "{\"xs\": [1, 2], \"d\": {\"k\": 2}}");
    SCFG_CHECK(t, scfg_test::json_eq(scfg::deep_copy(a), a));
    // scalars copy as values
    Json s = Json("x");
    Json s2 = s;
    s2 = Json("y");
    SCFG_CHECK_EQ(t, s.as_str(), "x");
    SCFG_CHECK(t, scfg::deep_copy(Json(BigInt(2).pow(70))).as_bigint() == BigInt(2).pow(70));
}

SCFG_TEST(JsonTests, ConstructionAndTypes) {
    SCFG_CHECK_EQ(t, Json().type_name(), "NoneType");
    SCFG_CHECK_EQ(t, Json(nullptr).type_name(), "NoneType");
    SCFG_CHECK_EQ(t, Json(false).type_name(), "bool");
    SCFG_CHECK_EQ(t, Json(0).type_name(), "int");
    SCFG_CHECK_EQ(t, Json(1.5).type_name(), "float");
    SCFG_CHECK_EQ(t, Json("s").type_name(), "str");
    SCFG_CHECK_EQ(t, Json::list().type_name(), "list");
    SCFG_CHECK_EQ(t, Json::dict().type_name(), "dict");
    SCFG_CHECK(t, Json(std::string_view("sv")).as_str() == "sv" && Json(std::string("s")).as_str() == "s");
    SCFG_CHECK(t, Json(true).is_bool() && !Json(true).is_int());  // a bool is not an int here (Python's bool is)
    // ints are normalised: a BigInt that fits is stored as int64, a uint64 beyond int64 as a BigInt
    SCFG_CHECK(t, Json(BigInt(42)).is_int64() && Json(BigInt(42)).as_int64() == 42);
    SCFG_CHECK(t, Json(BigInt(2).pow(63) - 1).is_int64());
    SCFG_CHECK(t, !Json(BigInt(2).pow(63)).is_int64());
    SCFG_CHECK(t, !Json(UINT64_MAX).is_int64() && Json(UINT64_MAX).as_bigint() == BigInt::from_uint64(UINT64_MAX));
    SCFG_CHECK(t, Json(uint64_t{5}).is_int64());
    SCFG_CHECK(t, Json(-3).as_bigint() == BigInt(-3));
    SCFG_CHECK(t, Json::list({1, "a", nullptr}).as_list().size() == 3);
    SCFG_CHECK(t, Json::list({1, "a", nullptr}).type() == Json::Type::List);
    SCFG_CHECK(t, Json(2).type() == Json::Type::Int && Json(BigInt(2).pow(80)).type() == Json::Type::Int);
}

SCFG_TEST(JsonTests, DumpsTheCycleItCannotWrite) {
    // A list that contains itself is a programming error (Python's "Circular reference detected"); repr prints it
    // as Python does. Break the cycle afterwards so the handles are freed.
    Json l = Json::list({1});
    l.as_list().push_back(l);
    SCFG_CHECK_EQ(t, scfg::py::repr(l), "[1, [...]]");
    l.as_list().clear();
}

// Every vector file is json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False) + "\n" (conformance/README):
// reading and re-dumping it reproduces it byte for byte.
SCFG_TEST(JsonTests, EveryVectorFileRoundTrips) {
    namespace fs = std::filesystem;
    fs::path root(scfg_test::vectors::root());
    std::error_code ec;
    size_t n = 0;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".json") continue;
        std::string rel = it->path().lexically_relative(root).generic_string();
        std::optional<std::string> text = scfg_test::vectors::read_text(rel);
        if (!t.check(text.has_value(), "cannot read " + rel)) continue;
        scfg::Result<Json> v = json::loads(*text);
        if (!t.check(v.ok(), rel + ": " + (v.ok() ? "" : v.err().str()))) continue;
        std::string again = json::dumps(v.value(), {.sort_keys = true, .indent = 1, .ensure_ascii = false}) + "\n";
        t.check(again == *text, rel + " does not round-trip");
        ++n;
    }
    t.check(!ec, "listing " + root.generic_string() + ": " + ec.message());
    t.check(n > 100, "only " + std::to_string(n) + " vector files under " + root.generic_string());
}

SCFG_TEST(JsonTests, JsonEqIsTheReadmeRule) {
    using scfg_test::first_diff;
    using scfg_test::json_eq;
    SCFG_CHECK(t, json_eq(L(t, "{\"a\":1,\"b\":[1,2]}"), L(t, "{\"b\":[1,2.0],\"a\":1.0}")));
    SCFG_CHECK(t, !json_eq(Json(true), Json(1)));
    SCFG_CHECK(t, !json_eq(Json(false), Json(0.0)));
    SCFG_CHECK(t, json_eq(Json(BigInt(2).pow(70)), Json(std::ldexp(1.0, 70))));
    SCFG_CHECK(t, !json_eq(Json(int64_t{9007199254740993}), Json(9007199254740992.0)));  // no lossy conversion
    SCFG_CHECK(t, !json_eq(Json(1), Json(1.5)));
    SCFG_CHECK(t, json_eq(Json(std::nan("")), Json(std::nan(""))));
    SCFG_CHECK(t, !json_eq(Json(1), Json(std::numeric_limits<double>::infinity())));
    SCFG_CHECK(t, json_eq(Json(), Json()));
    SCFG_CHECK(t, !json_eq(Json(), Json("")));
    SCFG_CHECK(t, !json_eq(Json("1"), Json(1)));
    SCFG_CHECK(t, !json_eq(L(t, "[1,2]"), L(t, "[2,1]")));
    SCFG_CHECK(t, !json_eq(L(t, "[]"), L(t, "{}")));
    SCFG_CHECK_EQ(t, first_diff(L(t, "{\"a\":{\"b\":[1,2]}}"), L(t, "{\"a\":{\"b\":[1,3]}}")).value_or("<equal>"), "a.b[1]: 2 != 3");
    SCFG_CHECK_EQ(t, first_diff(L(t, "{\"a\":1}"), L(t, "{\"a\":1,\"c\":2}")).value_or("<equal>"), "c: missing on the left");
    SCFG_CHECK_EQ(t, first_diff(L(t, "{\"a\":1,\"c\":2}"), L(t, "{\"a\":1}")).value_or("<equal>"), "c: missing on the right");
    SCFG_CHECK_EQ(t, first_diff(L(t, "[1]"), L(t, "[1,2]")).value_or("<equal>"), "length 1 != 2");
    SCFG_CHECK_EQ(t, first_diff(Json(true), Json(1)).value_or("<equal>"), "true != 1");
    SCFG_CHECK(t, !first_diff(Json("a"), Json("a")).has_value());
    std::string long_diff = first_diff(Json(std::string(300, 'x')), Json(1)).value_or("");
    SCFG_CHECK(t, long_diff.size() < 140 && long_diff.find("...") != std::string::npos);
}
