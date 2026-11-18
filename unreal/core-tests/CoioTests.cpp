// Copyright 2026 Repulse
// No Python twin: the transport rules of Coio.h (parse_rpc_body: rpc.UrllibTransport's body rules, as the GDScript
// ScfgCoio.parse_rpc_body and Coio.cs's ParseRpcBody read them; response_from_envelope: tests/unit/test_coio.py's
// envelope cases). Ported from unity/core-tests/CoioTests.cs.
#include "support/Helpers.h"
#include "support/JsonEq.h"
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Coio.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace coio = scfg::coio;
using scfg_test::same;

namespace scfg_test::coio_tests {

const std::string NINES(4301, '9');  // a 4301-digit int, one past the limit

// The TransportError parse_rpc_body gives, or a description of what it gave instead.
Err fails(int status, std::string_view body) {
    Result<Json> r = coio::parse_rpc_body(status, body);
    if (r.ok()) return Err::make("AssertionError", "parsed: " + scfg::json::dumps(r.value()).value());
    return r.err();
}

Json parsed(int status, std::string_view body) {
    Result<Json> r = coio::parse_rpc_body(status, body);
    if (!r.ok()) return Json("error: " + r.err().str());
    return r.value();
}

}  // namespace scfg_test::coio_tests

using namespace scfg_test::coio_tests;

SCFG_TEST(CoioTests, A2xxJsonBodyIsParsed) {
    same(t, parsed(200, "{\"result\": 1}"), Json::dict({{"result", 1}}), "200");
    same(t, parsed(204, "null"), Json(), "204");
    same(t, parsed(299, "[1, \"x\"]"), Json::list({1, "x"}), "299");
}

SCFG_TEST(CoioTests, A2xxBodyThatIsNotJsonIsInvalidJson) {
    for (const char* body : {"not json", "", "<html>", "{\"a\": 1", "1 2", "\xEF\xBB\xBF" "1"}) {
        Err e = fails(200, body);
        t.check(e.kind == "TransportError" && e.message == "invalid JSON response", std::string(body) + ": " + e.str());
        t.check(!e.status.has_value(), std::string(body) + ": a status");
    }
}

SCFG_TEST(CoioTests, BytesThatAreNotStrictUtf8AreInvalidJson) {
    for (const char* body : {
             "\"\xff\"",              // not UTF-8 at all
             "\"\xc0\x80\"",          // overlong NUL
             "\"\xed\xa0\x80\"",      // a UTF-16 surrogate
             "\"\xf4\x90\x80\x80\"",  // above U+10FFFF
             "\"\xe2\x82\"",          // truncated sequence
         }) {
        Err e = fails(200, body);
        t.check(e.message == "invalid JSON response", scfg::py::repr_str(body) + ": " + e.str());
    }
    same(t, parsed(200, "\"\xe2\x82\xac\""), Json("\xe2\x82\xac"), "euro");
    same(t, parsed(200, "\"\xf0\x9f\x98\x80\""), Json("\xf0\x9f\x98\x80"), "astral");
    // An escaped lone surrogate is JSON text, not a UTF-8 error: it is kept (as WTF-8).
    same(t, parsed(200, "\"\\ud800\""), Json("\xed\xa0\x80"), "escaped surrogate");
}

SCFG_TEST(CoioTests, AnIntegerOfMoreThan4300DigitsIsInvalidJson) {
    SCFG_CHECK_EQ(t, fails(200, "{\"result\": " + NINES + "}").message, "invalid JSON response");
    SCFG_CHECK_EQ(t, fails(200, "[1, -" + NINES + "]").message, "invalid JSON response");
    Err http = fails(400, "{\"error\": {\"code\": " + NINES + "}}");
    SCFG_CHECK_EQ(t, http.message, "HTTP 400");
    SCFG_CHECK(t, http.status == 400);
    // 4300 digits are read; strings, fractions and exponents do not count.
    Json r = parsed(200, "{\"r\": " + std::string(4300, '9') + "}");
    same(t, r, Json::dict({{"r", BigInt(10).pow(4300) - 1}}), "4300 digits");
    Json ok = parsed(200, "{\"s\": \"" + NINES + "\", \"f\": 1." + NINES + ", \"e\": 1e-" + NINES + "}");
    // Python: float("1." + "9" * 4301) == 2.0
    same(t, ok, Json::dict({{"s", NINES}, {"f", 2.0}, {"e", 0.0}}), "not integers");
}

SCFG_TEST(CoioTests, ANon2xxAnswerIsHttpStatus) {
    Err e = fails(502, "bad gateway");
    SCFG_CHECK_EQ(t, e.kind, "TransportError");
    SCFG_CHECK_EQ(t, e.message, "HTTP 502");
    SCFG_CHECK(t, e.status == 502);
    const std::vector<std::pair<int, std::string>> rows = {
        {503, ""}, {429, "{\"jsonrpc\": \"2.0\"}"}, {404, "{\"error\": 0}"}, {404, "{\"error\": null}"},
        {404, "[{\"error\": 1}]"}, {199, "{\"result\": 1}"}, {300, "{\"result\": 1}"}, {0, ""},
        {-1, ""}, {404, "{\"error\": \"\"}"}, {404, "{\"error\": {}}"},
    };
    for (const auto& [status, body] : rows) {
        Err x = fails(status, body);
        std::string want = "HTTP " + std::to_string(status);
        t.check(x.message == want && x.status == status, body + ": " + x.str());
    }
}

SCFG_TEST(CoioTests, ANon2xxJsonRpcErrorBodyPassesThrough) {
    same(t, parsed(400, "{\"error\": {\"code\": -32005, \"message\": \"range too large\"}}"),
         Json::dict({{"error", Json::dict({{"code", -32005}, {"message", "range too large"}})}}), "400");
    same(t, parsed(500, "{\"error\": \"x\"}"), Json::dict({{"error", "x"}}), "500");
    same(t, parsed(429, "{\"error\": [0], \"id\": 1}"), Json::dict({{"error", Json::list({0})}, {"id", 1}}), "429");
}

SCFG_TEST(CoioTests, ABodyOverTheCapIsResponseTooLarge) {
    SCFG_CHECK_EQ(t, coio::MAX_RESPONSE_BYTES, 16 * 1024 * 1024);
    std::string body(coio::MAX_RESPONSE_BYTES + 1, ' ');
    body[0] = '1';
    Err e = fails(200, body);
    SCFG_CHECK_EQ(t, e.message, "response too large");
    SCFG_CHECK(t, !e.status.has_value());
    SCFG_CHECK_EQ(t, fails(502, body).message, "response too large");
    // exactly at the cap is read
    same(t, parsed(200, std::string_view(body).substr(0, coio::MAX_RESPONSE_BYTES)), Json(1), "at the cap");
}

SCFG_TEST(CoioTests, TransportErrorStatusIsOptional) {
    Err plain = Err::transport_error("x", std::nullopt);
    SCFG_CHECK(t, !plain.status.has_value());
    Err with = Err::transport_error("x", 500);
    SCFG_CHECK(t, with.status == 500);
    SCFG_CHECK_EQ(t, with.message, "x");
    SCFG_CHECK(t, with.is_a("Exception") && !with.is_a("ValueError"));
}

SCFG_TEST(CoioTests, RequestHoldsWhatPostJsonTakes) {
    coio::Request r{"https://a", Json::dict({{"id", 1}}), 10.0};
    SCFG_CHECK_EQ(t, r.url, "https://a");
    SCFG_CHECK(t, r.timeout == 10.0);
    same(t, r.payload, Json::dict({{"id", 1}}), "payload");
}

// ---- response_from_envelope (tests/unit/test_coio.py) ----

namespace scfg_test::coio_tests {

Json::Dict env(Json status, Json body) { return Json::Dict{{"status", std::move(status)}, {"body", std::move(body)}}; }

Json from_env(const Json::Dict& e) {
    Result<Json> r = coio::response_from_envelope(e);
    if (!r.ok()) return Json("error: " + r.err().str());
    return r.value();
}

}  // namespace scfg_test::coio_tests

SCFG_TEST(CoioTests, EnvelopeToResponse) {
    same(t, from_env(env(200, "{\"result\": 1}")), Json::dict({{"result", 1}}), "200");
    same(t, from_env(env(400, "{\"error\": {\"code\": -32005, \"message\": \"range\"}}")),
         Json::dict({{"error", Json::dict({{"code", -32005}, {"message", "range"}})}}), "400");
    same(t, from_env(env(200, "null")), Json(), "null");
    // int_from(status): a str or a float status is read as Python's int() reads it
    same(t, from_env(env("200", "1")), Json(1), "str status");
    same(t, from_env(env(200.7, "[]")), Json::list(), "float status");
    same(t, from_env(env(true, "1")), Json("error: TransportError(status=1): HTTP 1"), "bool status");
    // The body is an already-decoded str: a lone surrogate in it (WTF-8) is kept, not an encoding error.
    same(t, from_env(env(200, "\"\xed\xa0\x80\"")), Json("\xed\xa0\x80"), "surrogate in a str");
}

SCFG_TEST(CoioTests, EnvelopeFailuresAreTransportErrors) {
    struct Row {
        Json::Dict envelope;
        std::string message;
        std::optional<int> status;
    };
    const std::vector<Row> rows = {
        {Json::Dict{{"error", "timed out"}}, "timed out", std::nullopt},
        {Json::Dict{{"error", Json()}}, "None", std::nullopt},
        {Json::Dict{{"error", Json::dict({{"a", Json::list({1, true})}})}}, "{'a': [1, True]}", std::nullopt},
        {env(503, "busy"), "HTTP 503", 503},
        {env(429, "{\"jsonrpc\": \"2.0\"}"), "HTTP 429", 429},
        {env(200, "<html>"), "invalid JSON response", std::nullopt},
        {env(200, "\xEF\xBB\xBF" "1"), "invalid JSON response", std::nullopt},
        {env(200, "{\"result\": " + NINES + "}"), "invalid JSON response", std::nullopt},
        {env(0, ""), "HTTP 0", 0},
        {env(Json(), Json()), "HTTP 0", 0},
        {env(502, ""), "HTTP 502", 502},
        {env(404, "{\"error\": 0}"), "HTTP 404", 404},
        {env(BigInt(10).pow(20), ""), "HTTP 100000000000000000000", std::nullopt},
    };
    for (const Row& row : rows) {
        Result<Json> r = coio::response_from_envelope(row.envelope);
        if (!t.check(!r.ok(), row.message + ": parsed")) continue;
        t.check(r.err().kind == "TransportError" && r.err().message == row.message && r.err().status == row.status,
                row.message + ": " + r.err().str());
    }
}

SCFG_TEST(CoioTests, AnEnvelopeBodyThatIsNotAStrIsATypeError) {
    // json.loads(5) is a TypeError, which response_from_envelope does not catch
    Result<Json> r = coio::response_from_envelope(env(200, 5));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "TypeError");
        SCFG_CHECK_EQ(t, r.err().message, "the JSON object must be str, bytes or bytearray, not int");
    }
    // int_from of a bad status: int()'s ValueError and TypeError, uncaught too
    Result<Json> v = coio::response_from_envelope(env("2x", "1"));
    if (SCFG_CHECK(t, !v.ok())) SCFG_CHECK_EQ(t, v.err().kind, "ValueError");
    Result<Json> l = coio::response_from_envelope(env(Json::list({1}), "1"));
    if (SCFG_CHECK(t, !l.ok())) SCFG_CHECK_EQ(t, l.err().kind, "TypeError");
}
