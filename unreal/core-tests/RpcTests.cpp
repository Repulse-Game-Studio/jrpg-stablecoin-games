// Copyright 2026 Repulse
// No Python twin: RpcClient and SolanaRpc driven by fake transports, ported from unity/core-tests/RpcTests.cs (itself
// from tests/unit/test_rpc_client.py, tests/unit/test_solana_rpc.py and rpgmaker's rpc.test.ts). These pin what no
// vector reaches: rotation, cooldown doubling and its cap on the injected clock, rate errors, an error answer
// resetting a cooling endpoint, range-error narrowing, try_at/call_at, and block numbers beyond int64 (Python ints
// are unbounded). The scenarios marked "Python:" were run against scfg_core's RpcClient with a scripted transport
// (uv run python, CPython 3.9.25), and the logs and outcomes below are the ones Python produced.
#include "support/JsonEq.h"
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/SolanaRpc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Co;
using scfg::Err;
using scfg::Future;
using scfg::Json;
using scfg::Promise;
using scfg::Result;
namespace coio = scfg::coio;
namespace py = scfg::py;
namespace rpc = scfg::rpc;
namespace srpc = scfg::solana::rpc;
using rpc::RpcClient;

namespace scfg_test::rpc_tests {

using Handler = std::function<Result<Json>(const std::string& method, const Json& params)>;
using Handlers = std::vector<std::pair<std::string, Handler>>;

struct Call {
    std::string url, method;
    Json params, payload;
    double timeout = 0;
};

Call record(const std::string& url, const Json& payload, double timeout) {
    const Json::Dict& d = payload.as_dict();
    return {url, d.get("method").as_str(), d.get("params"), payload, timeout};
}

// Answers from one handler per URL, at once (rpc_fakes.ts's FakeTransport).
struct FakeTransport final : coio::Transport {
    std::vector<Call> calls;
    Handlers handlers;

    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        calls.push_back(record(url, payload, timeout));
        for (const auto& [u, h] : handlers) {
            if (u == url) return Future<Json>::completed(h(calls.back().method, calls.back().params));
        }
        return Future<Json>::completed(Err::key_error(py::repr_str(url)));
    }
};

// Answers from a script per URL; an empty script is Python's "pop from empty list".
struct ScriptedTransport final : coio::Transport {
    std::vector<Call> log;
    std::map<std::string, std::deque<Result<Json>>> script;

    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        log.push_back(record(url, payload, timeout));
        std::deque<Result<Json>>& q = script[url];
        if (q.empty()) return Future<Json>::completed(Err::index_error("pop from empty list"));
        Result<Json> r = q.front();
        q.pop_front();
        return Future<Json>::completed(std::move(r));
    }
};

// Answers that arrive later, as a frame-driven transport's do.
struct DeferredTransport final : coio::Transport {
    std::vector<std::string> urls;
    std::vector<Promise<Json>> pending;

    Future<Json> post_json(std::string url, Json, double) override {
        urls.push_back(url);
        pending.emplace_back();
        return pending.back().get_future();
    }
};

Json ok(Json result, int64_t id = 1) { return Json::dict({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); }
Json err(int64_t code, std::string message) {
    return Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"error", Json::dict({{"code", code}, {"message", std::move(message)}})}});
}
Json err_raw(Json error) { return Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"error", std::move(error)}}); }
Err down() { return Err::transport_error("down", std::nullopt); }

Handler answer(Json result) {
    return [result](const std::string&, const Json&) -> Result<Json> { return ok(result); };
}
Handler fails() {
    return [](const std::string&, const Json&) -> Result<Json> { return Err::transport_error("connection refused", std::nullopt); };
}

const std::string A = "https://a.rpc";
const std::string B = "https://b.rpc";

// An RpcClient over a FakeTransport, on a settable clock (starting at 1000).
struct Fx {
    double now = 1000;
    FakeTransport t;
    std::optional<RpcClient> c;

    explicit Fx(Handlers handlers, BigInt log_range = 1000) {
        t.handlers = std::move(handlers);
        std::vector<std::string> urls;
        for (const auto& h : t.handlers) urls.push_back(h.first);
        Result<RpcClient> r = RpcClient::make(urls, t, {.log_range = log_range, .clock = [this] { return now; }});
        c.emplace(r.take());
    }
    Fx(const Fx&) = delete;
    Fx& operator=(const Fx&) = delete;
    RpcClient* operator->() { return &*c; }
    std::vector<std::string> urls() const {
        std::vector<std::string> out;
        for (const Call& x : t.calls) out.push_back(x.url);
        return out;
    }
};

// A client over a ScriptedTransport with a fixed clock.
struct Sx {
    ScriptedTransport t;
    std::optional<RpcClient> c;

    Sx(std::vector<std::pair<std::string, std::vector<Result<Json>>>> script, BigInt log_range = 1000, double now = 1000) {
        std::vector<std::string> urls;
        for (auto& [url, answers] : script) {
            urls.push_back(url);
            t.script[url] = std::deque<Result<Json>>(answers.begin(), answers.end());
        }
        Result<RpcClient> r = RpcClient::make(urls, t, {.log_range = log_range, .clock = [now] { return now; }});
        c.emplace(r.take());
    }
    Sx(const Sx&) = delete;
    Sx& operator=(const Sx&) = delete;
    RpcClient* operator->() { return &*c; }
};

// A coroutine the fake transports let finish inside the call.
template <class T>
Result<T> run(Co<T> c) {
    if (!c.done()) return Err::make("RuntimeError", "the coroutine did not finish");
    return c.take();
}

std::string kind_of(const Result<Json>& r) { return r.ok() ? "ok" : r.err().kind; }

bool same(scfg_test::Ctx& t, const Json& got, const Json& want, const std::string& what) {
    std::optional<std::string> diff = scfg_test::first_diff(got, want);
    return t.check(!diff, what + ": " + diff.value_or("") + " (got " + scfg::json::dumps(got).value() + ")");
}

Json strs(const std::vector<std::string>& v) {
    Json out = Json::list();
    for (const std::string& s : v) out.as_list().emplace_back(s);
    return out;
}

int64_t hex_param(const Json& v) {
    Result<BigInt> n = py::py_int_hex(v);
    return n.ok() && n.value().fits_int64() ? n.value().to_int64().value() : -1;
}

// eth_getLogs answered with one log per chunk, or a range error past maxSpan blocks; records each (from, to).
Handler logs_handler(std::vector<std::pair<int64_t, int64_t>>& seen, int64_t max_span = 0) {
    return [&seen, max_span](const std::string&, const Json& p) -> Result<Json> {
        const Json f = p.as_list()[0];
        int64_t lo = hex_param(f.as_dict().get("fromBlock")), hi = hex_param(f.as_dict().get("toBlock"));
        seen.emplace_back(lo, hi);
        if (max_span > 0 && hi - lo + 1 > max_span) return err(-32600, "block range too large");
        return ok(Json::list({Json::dict({{"blockNumber", py::hex(lo)}})}));
    };
}

using Spans = std::vector<std::pair<int64_t, int64_t>>;

// test_rpc_client.py's TimedTransport: each url takes costs[url] seconds of the clock (absent: its whole timeout, a
// hang) and then fails, or answers 0x7 when it is in answers. Records (url, timeout).
struct TimedTransport final : coio::Transport {
    double now = 1000;  // FakeClock
    std::map<std::string, double> costs;
    std::vector<std::string> answers;
    std::vector<std::pair<std::string, double>> calls;

    Future<Json> post_json(std::string url, Json, double timeout) override {
        calls.emplace_back(url, timeout);
        auto cost = costs.find(url);
        now += cost == costs.end() ? timeout : std::min(cost->second, timeout);
        if (std::find(answers.begin(), answers.end(), url) != answers.end()) return Future<Json>::completed(ok("0x7"));
        return Future<Json>::completed(Err::transport_error("timed out", std::nullopt));
    }
};

RpcClient timed_client(std::vector<std::string> urls, TimedTransport& tr) {
    return RpcClient::make(std::move(urls), tr, {.clock = [&tr] { return tr.now; }}).take();
}

}  // namespace scfg_test::rpc_tests

using namespace scfg_test::rpc_tests;

SCFG_TEST(RpcTests, RequiresUrls) {
    FakeTransport tr;
    Result<RpcClient> r = RpcClient::make({}, tr);
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "ValueError");
        SCFG_CHECK_EQ(t, r.err().message, "at least one RPC url required");
    }
}

SCFG_TEST(RpcTests, CallSendsJsonRpcAndReturnsResult) {
    Fx c({{A, answer("0x10")}});
    Result<Json> r = run(c->call("eth_blockNumber", Json::list()));
    if (SCFG_CHECK(t, r.ok())) same(t, r.value(), "0x10", "result");
    if (!SCFG_CHECK_EQ(t, c.t.calls.size(), 1)) return;
    SCFG_CHECK_EQ(t, c.t.calls[0].url, A);
    SCFG_CHECK_EQ(t, scfg::json::dumps(c.t.calls[0].payload).value(),
                  "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"eth_blockNumber\", \"params\": []}");
    SCFG_CHECK(t, c.t.calls[0].timeout == 10.0);
    // The params list goes out as given (the same object).
    Json params = Json::list({"x"});
    (void)run(c->call("m", params));
    SCFG_CHECK(t, c.t.calls[1].params.same_object(params));
    c->timeout = 2.5;
    (void)run(c->call("m", Json::list()));
    SCFG_CHECK(t, c.t.calls[2].timeout == 2.5);
}

SCFG_TEST(RpcTests, JsonRpcIdsCountFromOnePerClientAcrossEndpoints) {
    Fx c({{A, fails()}, {B, answer("0x1")}});
    (void)run(c->block_number());
    (void)run(c->block_number());
    std::vector<int64_t> ids;
    for (const Call& x : c.t.calls) ids.push_back(x.payload.as_dict().get("id").as_int64());
    SCFG_CHECK(t, ids == std::vector<int64_t>({1, 2, 3}));
}

SCFG_TEST(RpcTests, BlockNumberIsPythonsIntBase16) {
    // Python: block_number of these answers is 436, 2**160 - 1, -16, 31, 18
    Sx c({{"https://a", {ok("0x1b4"), ok("0x" + std::string(40, 'f')), ok("-0x10"), ok(" 0x_1F "), ok("\xd9\xa1\xd9\xa2")}}});
    std::vector<BigInt> want = {436, (BigInt(1) << 160) - 1, -16, 31, 18};
    for (const BigInt& w : want) {
        Result<BigInt> r = run(c->block_number());
        if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value().to_decimal(), w.to_decimal());
    }
}

SCFG_TEST(RpcTests, BlockNumberRejectsMalformedHex) {
    for (const Json& bad : {Json(""), Json("zz"), Json("12 34"), Json("0x"), Json("1__2"), Json("0x__1"), Json("ff_"),
                            Json("\x1c" "0x1"), Json("\xef\xbd\x86"), Json(5), Json(), Json::dict(), Json(true), Json(1.5)}) {
        Fx c({{A, answer(bad)}});
        Result<BigInt> r = run(c->block_number());
        t.check(!r.ok() && (r.err().kind == "TypeError" || r.err().kind == "ValueError"),
                py::repr(bad).value() + ": " + (r.ok() ? "ok" : r.err().str()));
    }
}

SCFG_TEST(RpcTests, FallsBackToNextEndpointOnTransportError) {
    Fx c({{A, fails()}, {B, answer("0x1")}});
    Result<BigInt> r = run(c->block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 1);
    SCFG_CHECK(t, c.urls() == std::vector<std::string>({A, B}));
}

SCFG_TEST(RpcTests, FailedEndpointCoolsDownThenIsRetried) {
    bool a_up = false;
    Handler a = [&a_up](const std::string&, const Json&) -> Result<Json> {
        if (a_up) return ok("0xa");
        return Err::transport_error("HTTP 503", 503);
    };
    Fx c({{A, a}, {B, answer("0xb")}});
    Result<BigInt> r = run(c->block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 0xb);
    c.t.calls.clear();
    r = run(c->block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 0xb);
    SCFG_CHECK(t, c.urls() == std::vector<std::string>({B}));
    a_up = true;
    c.now += rpc::COOLDOWN_START;
    r = run(c->block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 0xa);
}

SCFG_TEST(RpcTests, CooldownDoublesAndCaps) {
    Fx c({{A, fails()}});
    std::vector<double> delays;
    for (int i = 0; i < 8; ++i) {
        Result<Json> r = run(c->call("eth_blockNumber", Json::list()));
        SCFG_CHECK_EQ(t, kind_of(r), "RpcUnavailable");
        double d = c->cooldown_of(A);
        delays.push_back(d);
        c.now += d;
    }
    SCFG_CHECK(t, delays == std::vector<double>({30.0, 60.0, 120.0, 240.0, 480.0, 600.0, 600.0, 600.0}));
    SCFG_CHECK(t, rpc::COOLDOWN_MAX == 600.0);
}

SCFG_TEST(RpcTests, CooldownBoundaryIsUntilLeNow) {
    Fx c({{A, fails()}});
    SCFG_CHECK_EQ(t, kind_of(run(c->call("m", Json::list()))), "RpcUnavailable");
    SCFG_CHECK(t, c->live_urls().empty());
    c.now += 29;
    SCFG_CHECK(t, c->live_urls().empty());
    c.now += 1;
    SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({A}));
    SCFG_CHECK(t, c->cooldown_until(A) == 1030.0);
}

SCFG_TEST(RpcTests, SuccessResetsCooldown) {
    bool fail = true;
    Handler a = [&fail](const std::string&, const Json&) -> Result<Json> {
        if (fail) return Err::transport_error("x", std::nullopt);
        return ok("0x1");
    };
    Fx c({{A, a}});
    SCFG_CHECK_EQ(t, kind_of(run(c->call("eth_blockNumber", Json::list()))), "RpcUnavailable");
    fail = false;
    c.now += 31;
    SCFG_CHECK(t, run(c->call("eth_blockNumber", Json::list())).ok());
    SCFG_CHECK(t, c->cooldown_of(A) == 0.0 && c->cooldown_until(A) == 0.0);
}

SCFG_TEST(RpcTests, AnErrorAnswerResetsACoolingEndpoint) {
    // Python _attempt: an endpoint that answers with a node error is up (_ok), even one that was cooling down.
    Sx c({{"https://a", {down(), err(3, "x"), down(), err(3, "x")}}});
    SCFG_CHECK_EQ(t, kind_of(run(c->call_at("https://a", "m", Json::list()))), "RpcUnavailable");
    SCFG_CHECK(t, c->cooldown_of("https://a") == 30.0 && c->live_urls().empty());
    SCFG_CHECK_EQ(t, kind_of(run(c->call_at("https://a", "m", Json::list()))), "RpcError");
    SCFG_CHECK(t, c->cooldown_of("https://a") == 0.0);
    SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({"https://a"}));
    // And the delay starts over: the next failure is 30 s again, not 60.
    (void)run(c->call_at("https://a", "m", Json::list()));
    SCFG_CHECK(t, c->cooldown_of("https://a") == 30.0);
    (void)run(c->call_at("https://a", "m", Json::list()));
    SCFG_CHECK(t, c->cooldown_of("https://a") == 0.0);
}

SCFG_TEST(RpcTests, AllEndpointsDownRaisesUnavailable) {
    Fx c({{A, fails()}, {B, fails()}});
    Result<BigInt> r = run(c->block_number());
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "RpcUnavailable");
        SCFG_CHECK_EQ(t, r.err().message, "all endpoints failed for eth_blockNumber");
        SCFG_CHECK(t, r.err().is_a("Exception") && !r.err().is_a("RpcError"));
    }
}

SCFG_TEST(RpcTests, ACallSpendsAtMostItsBudgetAcrossEndpoints) {
    const std::string C = "https://c.rpc", D = "https://d.rpc";
    TimedTransport tr;  // every endpoint hangs
    RpcClient c = timed_client({A, B, C, D}, tr);
    SCFG_CHECK(t, rpc::CALL_BUDGET == 15.0);
    SCFG_CHECK_EQ(t, kind_of(run(c.call("eth_blockNumber", Json::list()))), "RpcUnavailable");
    // The rest of the 15 s, then nothing is left for C, D.
    using Calls = std::vector<std::pair<std::string, double>>;
    SCFG_CHECK(t, tr.calls == Calls({{A, 10.0}, {B, 5.0}}));
    SCFG_CHECK(t, tr.now == 1000.0 + rpc::CALL_BUDGET);
    // Skipped, not cooled down: the next call asks C.
    SCFG_CHECK(t, c.live_urls() == std::vector<std::string>({C, D}));
}

SCFG_TEST(RpcTests, EachAttemptWaitsAtMostWhatIsLeftOfTheBudget) {
    const std::string C = "https://c.rpc";
    TimedTransport tr;
    tr.costs = {{A, 3}, {B, 10}, {C, 1}};
    tr.answers = {C};
    RpcClient c = timed_client({A, B, C}, tr);
    Result<BigInt> r = run(c.block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 7);
    using Calls = std::vector<std::pair<std::string, double>>;
    SCFG_CHECK(t, tr.calls == Calls({{A, 10.0}, {B, 10.0}, {C, 2.0}}));
}

SCFG_TEST(RpcTests, TheClockIsSettableAndReadOnEveryCall) {
    // Python: a float clock; until = 5.5 + 30.0 = 35.5, live again at 35.5 and not at 35.4
    Fx c({{A, fails()}});
    double now = 5.5;
    c->clock = [&now] { return now; };
    SCFG_CHECK_EQ(t, kind_of(run(c->call("m", Json::list()))), "RpcUnavailable");
    SCFG_CHECK(t, c->cooldown_until(A) == 35.5);
    now = 35.4;
    SCFG_CHECK(t, c->live_urls().empty());
    now = 35.5;
    SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({A}));
}

SCFG_TEST(RpcTests, TheDefaultClockIsMonotonicSeconds) {
    FakeTransport tr;
    tr.handlers = {{A, fails()}};
    Result<RpcClient> made = RpcClient::make({A}, tr);
    if (!SCFG_CHECK(t, made.ok())) return;
    RpcClient c = made.take();
    double a = c.clock(), b = c.clock();
    SCFG_CHECK(t, a > 0 && b >= a && b - a < 60);
    (void)run(c.call("m", Json::list()));
    SCFG_CHECK(t, c.live_urls().empty());
    SCFG_CHECK(t, c.cooldown_until(A) >= a + 30 && c.cooldown_until(A) < c.clock() + 31);
}

SCFG_TEST(RpcTests, ANonTransportErrorPropagatesFromAttempt) {
    // Python: run_sync throws any transport exception into the generator; only TransportError is an endpoint
    // failure (KeyError here, CancelledError when a transport drops a request).
    for (const char* kind : {"KeyError", "CancelledError", "HostGone"}) {
        Handler boom = [kind](const std::string&, const Json&) -> Result<Json> { return Err::make(kind, "boom"); };
        Fx c({{A, boom}, {B, answer(1)}});
        Result<Json> r = run(c->call("m", Json::list()));
        if (SCFG_CHECK(t, !r.ok())) {
            SCFG_CHECK_EQ(t, r.err().kind, kind);
            SCFG_CHECK_EQ(t, r.err().message, "boom");
        }
        SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({A, B}));
        SCFG_CHECK(t, c.urls() == std::vector<std::string>({A}));
    }
}

SCFG_TEST(RpcTests, JsonRpcErrorIsRaisedWithoutRotation) {
    Handler a = [](const std::string&, const Json&) -> Result<Json> { return err(-32000, "execution reverted"); };
    Fx c({{A, a}, {B, answer("0x1")}});
    Result<Json> r = run(c->call("eth_call", Json::list()));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "RpcError");
        SCFG_CHECK(t, r.err().rpc_code == -32000);
        SCFG_CHECK_EQ(t, r.err().url, A);
        SCFG_CHECK_EQ(t, r.err().message, "execution reverted");
    }
    SCFG_CHECK(t, c.urls() == std::vector<std::string>({A}));
}

SCFG_TEST(RpcTests, RateLimitJsonRpcErrorRotates) {
    Handler a = [](const std::string&, const Json&) -> Result<Json> { return err(-32005, "Too Many Requests: rate limit"); };
    Fx c({{A, a}, {B, answer("0x3")}});
    Result<BigInt> r = run(c->block_number());
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 3);
    SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({B}));
    SCFG_CHECK(t, c->cooldown_of(A) == 30.0);
    for (const char* m : {"RATE LIMIT", "rate-limited", "429 Too many requests", "Rate Limit exceeded"})
        t.check(rpc::is_rate_error(Err::rpc_error(1, m)), std::string("rate: ") + m);
    // Python: "RATE LİMIT".lower() has no "limit" in it
    SCFG_CHECK(t, !rpc::is_rate_error(Err::rpc_error(1, "RATE L\xc4\xb0MIT")));
    SCFG_CHECK(t, !rpc::is_rate_error(Err::rpc_error(1, "limit")));
}

SCFG_TEST(RpcTests, IsRangeError) {
    SCFG_CHECK(t, rpc::is_range_error(Err::rpc_error(-32600, "eth_getLogs block range is too large")));
    SCFG_CHECK(t, rpc::is_range_error(Err::rpc_error(-32005, "query returned more than 10000 results; limit exceeded")));
    SCFG_CHECK(t, rpc::is_range_error(Err::rpc_error(-32602, "Archive requests require a personal token. Get one at x")));
    SCFG_CHECK(t, !rpc::is_range_error(Err::rpc_error(-32000, "execution reverted")));
    SCFG_CHECK(t, rpc::is_range_error(Err::rpc_error(1, "LIMIT")));
    // Python: "LİMIT".lower() is "li̇mit", which does not contain "limit"; the Kelvin sign lowers to k
    SCFG_CHECK(t, !rpc::is_range_error(Err::rpc_error(1, "L\xc4\xb0MIT")));
    SCFG_CHECK(t, !rpc::is_range_error(Err::rpc_error(1, "ARCH\xc4\xb0VE")));
    SCFG_CHECK(t, rpc::is_range_error(Err::rpc_error(1, "MAX RESULTS")));
}

SCFG_TEST(RpcTests, ReceiptAndBlockHash) {
    Handler h = [](const std::string& m, const Json& p) -> Result<Json> {
        if (m == "eth_getTransactionReceipt") {
            if (py::eq(p.as_list()[0], "0xmissing")) return ok(Json());
            return ok(Json::dict({{"status", "0x1"}}));
        }
        if (m == "eth_getBlockByNumber") {
            if (scfg::json::dumps(p).value() != "[\"0x64\", false]") return err(1, "bad params " + scfg::json::dumps(p).value());
            return ok(Json::dict({{"hash", "0xABC"}}));
        }
        return ok(Json());
    };
    Fx c({{A, h}});
    Result<Json> missing = run(c->get_transaction_receipt("0xmissing"));
    if (SCFG_CHECK(t, missing.ok())) SCFG_CHECK(t, missing.value().is_null());
    Result<Json> found = run(c->get_transaction_receipt("0xok"));
    if (SCFG_CHECK(t, found.ok())) same(t, found.value(), Json::dict({{"status", "0x1"}}), "receipt");
    Result<std::optional<std::string>> hash = run(c->get_block_hash(100));
    if (SCFG_CHECK(t, hash.ok() && hash.value())) SCFG_CHECK_EQ(t, *hash.value(), "0xabc");
    // A stored hint goes out as given (not a str here).
    (void)run(c->get_transaction_receipt(5));
    same(t, c.t.calls.back().params, Json::list({5}), "receipt params");
}

SCFG_TEST(RpcTests, BlockHashNoneForUnknownOrEmptyBlock) {
    for (const Json& block : {Json(), Json::dict(), Json(""), Json(0), Json(false), Json::list()}) {
        Fx c({{A, answer(block)}});
        Result<std::optional<std::string>> r = run(c->get_block_hash(1000000000));
        t.check(r.ok() && !r.value().has_value(), py::repr(block).value());
    }
}

SCFG_TEST(RpcTests, BlockHashRejectsMalformedBlocksAsPythonDoes) {
    // Python: KeyError for a missing hash, TypeError for a block that is not subscriptable by str, AttributeError
    // for a hash that is not a str (it has no .lower())
    struct Row {
        Json block;
        std::string kind, message;
    };
    const std::vector<Row> rows = {
        {Json::dict({{"number", "0x1"}}), "KeyError", "'hash'"},
        {Json::dict({{"hash", Json()}}), "AttributeError", "'NoneType' object has no attribute 'lower'"},
        {Json::dict({{"hash", 5}}), "AttributeError", "'int' object has no attribute 'lower'"},
        {Json("str"), "TypeError", "string indices must be integers"},
        {Json::list({1}), "TypeError", "list indices must be integers or slices, not str"},
        {Json(5), "TypeError", "'int' object is not subscriptable"},
        {Json(true), "TypeError", "'bool' object is not subscriptable"},
    };
    for (const Row& row : rows) {
        Fx c({{A, answer(row.block)}});
        Result<std::optional<std::string>> r = run(c->get_block_hash(1));
        t.check(!r.ok() && r.err().kind == row.kind && r.err().message == row.message,
                py::repr(row.block).value() + ": " + (r.ok() ? "ok" : r.err().str()));
    }
}

SCFG_TEST(RpcTests, BlockHashAndBlockNumberFormsMatchPython) {
    Sx c({{"https://a", {ok(Json::dict({{"hash", "0xAB"}})), ok(Json::dict({{"hash", "0xCD"}})), ok(Json::dict({{"hash", "0xEF"}}))}}});
    Result<std::optional<std::string>> h = run(c->get_block_hash(-5));
    if (SCFG_CHECK(t, h.ok() && h.value())) SCFG_CHECK_EQ(t, *h.value(), "0xab");
    h = run(c->get_block_hash(int64_t{1} << 40));
    if (SCFG_CHECK(t, h.ok() && h.value())) SCFG_CHECK_EQ(t, *h.value(), "0xcd");
    h = run(c->get_block_hash(BigInt(1) << 70));
    if (SCFG_CHECK(t, h.ok() && h.value())) SCFG_CHECK_EQ(t, *h.value(), "0xef");
    Json params = Json::list();
    for (const Call& x : c.t.log) params.as_list().push_back(x.params);
    same(t, params,
         Json::list({Json::list({"-0x5", false}), Json::list({"0x10000000000", false}), Json::list({"0x400000000000000000", false})}),
         "params");
}

// ---- eth_getLogs chunking ----

SCFG_TEST(RpcTests, GetLogsChunksByLogRange) {
    Spans seen;
    Fx c({{A, logs_handler(seen)}}, 100);
    Result<std::vector<Json>> logs = run(c->get_logs("0xtoken", Json::list({"0xtopic"}), 1000, 1250));
    SCFG_CHECK(t, seen == Spans({{1000, 1099}, {1100, 1199}, {1200, 1250}}));
    if (SCFG_CHECK(t, logs.ok())) SCFG_CHECK_EQ(t, logs.value().size(), 3);
}

SCFG_TEST(RpcTests, GetLogsEmptyRangeMakesNoCalls) {
    Spans seen;
    Fx c({{A, logs_handler(seen)}});
    Result<std::vector<Json>> logs = run(c->get_logs("0xtoken", Json::list(), 10, 9));
    SCFG_CHECK(t, logs.ok() && logs.value().empty());
    SCFG_CHECK(t, seen.empty());
}

SCFG_TEST(RpcTests, GetLogsHalvesRangeOnRangeErrorAndRemembers) {
    Spans seen;
    Fx c({{A, logs_handler(seen, 300)}}, 1000);
    SCFG_CHECK(t, run(c->get_logs("0xtoken", Json::list(), 0, 499)).ok());
    SCFG_CHECK(t, c->log_range() == 250);
    SCFG_CHECK(t, seen == Spans({{0, 499}, {0, 249}, {250, 499}}));
}

SCFG_TEST(RpcTests, GetLogsGivesUpAtMinimumRange) {
    Spans seen;
    Fx c({{A, logs_handler(seen, 1)}}, 40);
    Result<std::vector<Json>> r = run(c->get_logs("0xtoken", Json::list(), 0, 100));
    if (SCFG_CHECK(t, !r.ok())) SCFG_CHECK_EQ(t, r.err().kind, "RpcError");
    SCFG_CHECK(t, c->log_range() == rpc::MIN_LOG_RANGE);
}

SCFG_TEST(RpcTests, GetLogsSendsFilterShape) {
    Fx c({{A, answer(Json::list())}});
    SCFG_CHECK(t, run(c->get_logs("0xToken", Json::list({"0xt0", Json(), "0xt2"}), 16, 31)).ok());
    if (!SCFG_CHECK_EQ(t, c.t.calls.size(), 1)) return;
    SCFG_CHECK_EQ(t, c.t.calls[0].method, "eth_getLogs");
    SCFG_CHECK_EQ(t, scfg::json::dumps(c.t.calls[0].params).value(),
                  "[{\"address\": \"0xToken\", \"topics\": [\"0xt0\", null, \"0xt2\"], \"fromBlock\": \"0x10\", "
                  "\"toBlock\": \"0x1f\"}]");
}

SCFG_TEST(RpcTests, GetLogsExtendsAsPythonsListExtend) {
    // Python: out.extend(logs or []): None, {}, "", 0 and False are empty; a dict extends with its keys, a str with
    // its characters; anything else is "'int' object is not iterable"
    for (const Json& empty : {Json(), Json::dict(), Json(""), Json(0), Json(false)}) {
        Fx c({{A, answer(empty)}});
        Result<std::vector<Json>> r = run(c->get_logs("0xt", Json::list(), 1, 2));
        t.check(r.ok() && r.value().empty(), py::repr(empty).value());
    }
    Fx d({{A, answer(Json::dict({{"a", 1}}))}});
    Result<std::vector<Json>> keys = run(d->get_logs("0xt", Json::list(), 1, 1));
    if (SCFG_CHECK(t, keys.ok())) same(t, Json(keys.value()), Json::list({"a"}), "dict");
    Fx s({{A, answer("a\xf0\x9f\x98\x80")}});
    Result<std::vector<Json>> chars = run(s->get_logs("0xt", Json::list(), 1, 1));
    if (SCFG_CHECK(t, chars.ok())) same(t, Json(chars.value()), Json::list({"a", "\xf0\x9f\x98\x80"}), "str");
    for (const auto& [v, msg] : {std::pair<Json, std::string>{Json(5), "'int' object is not iterable"},
                                 std::pair<Json, std::string>{Json(1.5), "'float' object is not iterable"}}) {
        Fx n({{A, answer(v)}});
        Result<std::vector<Json>> r = run(n->get_logs("0xt", Json::list(), 1, 1));
        t.check(!r.ok() && r.err().kind == "TypeError" && r.err().message == msg, msg);
    }
}

SCFG_TEST(RpcTests, LogRangeIsRememberedPerEndpoint) {
    bool a_down = false;
    Spans seen;
    Handler a_logs = logs_handler(seen);
    Handler a = [&a_down, a_logs](const std::string& m, const Json& p) -> Result<Json> {
        if (a_down) return Err::transport_error("down", std::nullopt);
        return a_logs(m, p);
    };
    Fx c({{A, a}, {B, logs_handler(seen, 50)}}, 1000);
    SCFG_CHECK(t, run(c->get_logs("0xtoken", Json::list(), 0, 999)).ok());
    SCFG_CHECK(t, c->log_range() == 1000);
    a_down = true;
    SCFG_CHECK(t, run(c->get_logs("0xtoken", Json::list(), 0, 199)).ok());
    SCFG_CHECK(t, c->range_of(B) < 1000);
    SCFG_CHECK(t, c->log_range() == c->range_of(B));
    a_down = false;
    c.now += 31;
    SCFG_CHECK(t, c->log_range() == 1000);
}

SCFG_TEST(RpcTests, SuggestedRangeUsesStatedLimitOrHalves) {
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(-32602, "eth_getLogs is limited to 0 - 50 blocks range"), 2000) == 50);
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(35, "ranges over 10000 blocks are not supported on free plan"), 2000) == 1000);
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(-32602, "Archive requests require a personal token"), 500) == 250);
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(-32005, "block range is too large"), 15) == rpc::MIN_LOG_RANGE);
    // Python: tried 2**70 halves to 2**69; a stated 50 is taken
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "x"), BigInt(1) << 70) == (BigInt(1) << 69));
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "max 50"), BigInt(1) << 70) == 50);
    // Python's floor division of a negative tried (never asked for, but defined)
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "x"), -3) == rpc::MIN_LOG_RANGE);
}

SCFG_TEST(RpcTests, SuggestedRangeIgnoresNumbersBeyond18Digits) {
    auto rep = [](std::string_view s, size_t n) {
        std::string out;
        for (size_t i = 0; i < n; ++i) out += s;
        return out;
    };
    struct Row {
        std::string message;
        int64_t tried, expected;
    };
    const std::vector<Row> rows = {
        {"limited to 50 blocks; request " + std::string(4301, '9'), 2000, 50},
        {"block range too large: " + std::string(4301, '9'), 5000, 2500},
        {"block range too large: " + std::string(1000000, '9'), 5000, 2500},
        {"max " + std::string(30, '0') + "50 blocks", 2000, 50},
        {"max " + std::string(19, '9') + " or 50", 2000, 50},
        {"max \xd9\xa5\xd9\xa0 blocks", 2000, 50},
        {"max " + rep("\xd9\xa0", 25) + "\xd9\xa5\xd9\xa0", 2000, 50},
        {"max " + rep("\xd9\xa0", 1000000) + "\xd9\xa5\xd9\xa0", 2000, 50},
        {"max \xef\xbc\x95\xef\xbc\x90", 2000, 50},
        {"max \xe0\xa5\xab\xe0\xa5\xa6 or \xe0\xa7\xab\xe0\xa7\xa6", 2000, 50},
        {"max \xf0\x9d\x9f\x93\xf0\x9d\x9f\x8e", 2000, 50},    // mathematical bold digits: outside the BMP
        {"max \xf0\x96\xab\x85\xf0\x96\xab\x80", 2000, 1000},  // Tangsa digits (Unicode 14): not digits in Python 3.9
    };
    for (const Row& row : rows) {
        BigInt got = rpc::suggested_range(Err::rpc_error(-32005, row.message), row.tried);
        t.check(got == row.expected, row.message.substr(0, 40) + ": " + got.to_decimal());
    }
    BigInt big = BigInt::from_uint64(9000000000000000000u);
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "max " + std::string(18, '9')), big) == int64_t{999999999999999999});
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "max 00000" + std::string(18, '9')), big) == int64_t{999999999999999999});
    BigInt half = big.floordiv(2).value();
    SCFG_CHECK(t, rpc::suggested_range(Err::rpc_error(0, "max 1" + std::string(18, '0')), big) == half);
}

SCFG_TEST(RpcTests, AnOverlongNumberInARangeErrorDoesNotBreakGetLogs) {
    std::string message = "block range too large: " + std::string(4301, '9');
    Handler h = [message](const std::string&, const Json& p) -> Result<Json> {
        const Json::Dict& f = p.as_list()[0].as_dict();
        if (hex_param(f.get("toBlock")) - hex_param(f.get("fromBlock")) + 1 > 100) return err(-32005, message);
        return ok(Json::list());
    };
    Fx c({{A, h}}, 400);
    Result<std::vector<Json>> r = run(c->get_logs("0xT", Json::list(), 1, 400));
    SCFG_CHECK(t, r.ok() && r.value().empty());
    SCFG_CHECK(t, c->log_range() == 100);
}

SCFG_TEST(RpcTests, StatedLimitIsAdoptedInOneStep) {
    Spans seen;
    Handler h = [&seen](const std::string&, const Json& p) -> Result<Json> {
        const Json::Dict& f = p.as_list()[0].as_dict();
        int64_t lo = hex_param(f.get("fromBlock")), hi = hex_param(f.get("toBlock"));
        seen.emplace_back(lo, hi);
        if (hi - lo + 1 > 50) return err(-32602, "eth_getLogs is limited to 0 - 50 blocks range");
        return ok(Json::list());
    };
    Fx c({{A, h}}, 2000);
    SCFG_CHECK(t, run(c->get_logs("0xtoken", Json::list(), 0, 99)).ok());
    SCFG_CHECK(t, seen == Spans({{0, 99}, {0, 49}, {50, 99}}));
    SCFG_CHECK(t, c->log_range() == 50);
}

SCFG_TEST(RpcTests, BlockArithmeticBeyondInt64IsPythons) {
    // Python: get_logs(2**63 - 5, 2**63 + 5) with log_range 2**70 is one request, 0x7ffffffffffffffb..0x8000000000000005
    Sx one({{"https://a", {ok(Json::list({Json::dict({{"n", 1}})}))}}}, BigInt(1) << 70);
    BigInt top = BigInt(1) << 63;
    Result<std::vector<Json>> r = run(one->get_logs("0xT", Json::list(), top - 5, top + 5));
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value().size(), 1);
    if (SCFG_CHECK_EQ(t, one.t.log.size(), 1)) {
        const Json::Dict& f = one.t.log[0].params.as_list()[0].as_dict();
        same(t, f.get("fromBlock"), "0x7ffffffffffffffb", "from");
        same(t, f.get("toBlock"), "0x8000000000000005", "to");
    }
    // Python: a range error over 0..2**64 halves to 2**63 and splits at the int64 boundary
    Sx wide({{"https://a", {err(-32005, "block range too large"), ok(Json::list()), ok(Json::list()), ok(Json::list())}}},
            BigInt(1) << 70);
    SCFG_CHECK(t, run(wide->get_logs("0xT", Json::list(), 0, BigInt(1) << 64)).ok());
    Json spans = Json::list();
    for (const Call& x : wide.t.log) {
        const Json::Dict& f = x.params.as_list()[0].as_dict();
        spans.as_list().push_back(Json::list({f.get("fromBlock"), f.get("toBlock")}));
    }
    same(t, spans,
         Json::list({Json::list({"0x0", "0x10000000000000000"}), Json::list({"0x0", "0x7fffffffffffffff"}),
                     Json::list({"0x8000000000000000", "0xffffffffffffffff"}),
                     Json::list({"0x10000000000000000", "0x10000000000000000"})}),
         "spans");
    SCFG_CHECK(t, wide->range_of("https://a") == top);
}

// ---- malformed answers (Python: _parse_error and _attempt) ----

SCFG_TEST(RpcTests, ErrorClassificationMatchesPython) {
    // Python: scen.py "classify". code/message are the RpcError's; otherwise the call's result, or "fallback" when
    // endpoint a failed and b answered.
    struct Row {
        std::string name;
        Json response;
        std::optional<int64_t> code;
        std::string message;
        Json result;
        std::vector<std::string> urls, live;
    };
    const std::vector<std::string> a = {"a"}, ab = {"a", "b"}, b = {"b"};
    const std::vector<Row> rows = {
        {"str_code_int_message", err_raw(Json::dict({{"code", "12"}, {"message", 5}})), 12, "5", Json(), a, ab},
        {"falsy_error_zero", err_raw(0), 0, "0", Json(), a, ab},
        {"error_list", err_raw(Json::list()), 0, "[]", Json(), a, ab},
        {"float_code", err_raw(Json::dict({{"code", 1.9}})), 1, "", Json(), a, ab},
        {"bool_code", err_raw(Json::dict({{"code", true}, {"message", Json()}})), 1, "None", Json(), a, ab},
        {"nan_code", Json::dict({{"error", Json::dict({{"code", std::nan("")}, {"message", "m"}})}}), 0, "m", Json(), a, ab},
        {"spaced_str_code", err_raw(Json::dict({{"code", " 7 "}})), 7, "", Json(), a, ab},
        {"underscored_code", err_raw(Json::dict({{"code", "1_0"}})), 10, "", Json(), a, ab},
        {"list_code", err_raw(Json::dict({{"code", Json::list({1})}, {"message", 1.5}})), 0, "1.5", Json(), a, ab},
        {"dict_message", err_raw(Json::dict({{"message", Json::dict({{"a", Json()}})}})), 0, "{'a': None}", Json(), a, ab},
        {"error_dict_empty", err_raw(Json::dict()), 0, "", Json(), a, ab},
        {"rate_limit_dotted_I", err_raw(Json::dict({{"code", -1}, {"message", "RATE L\xc4\xb0MIT"}})), -1,
         "RATE L\xc4\xb0MIT", Json(), a, ab},
        {"result_and_null_error", Json::dict({{"result", 7}, {"error", Json()}}), std::nullopt, "", 7, a, ab},
        {"ids_ignored", ok(5, 999), std::nullopt, "", 5, a, ab},
        {"list_response", Json::list({1}), std::nullopt, "", "fallback", ab, b},
        {"string_response", "x", std::nullopt, "", "fallback", ab, b},
        {"null_response", Json(), std::nullopt, "", "fallback", ab, b},
        {"only_id", Json::dict({{"id", 3}}), std::nullopt, "", "fallback", ab, b},
        {"rate_limit_string", err_raw("Rate Limit exceeded"), std::nullopt, "", "fallback", ab, b},
    };
    for (const Row& row : rows) {
        Sx c({{"https://a", {row.response}}, {"https://b", {ok("fallback")}}});
        Result<Json> r = run(c->call("m", Json::list({1})));
        if (row.code) {
            t.check(!r.ok() && r.err().kind == "RpcError" && r.err().rpc_code == row.code &&
                        r.err().message == row.message && r.err().url == "https://a",
                    row.name + ": " + (r.ok() ? "ok" : r.err().str()));
        } else if (t.check(r.ok(), row.name + ": " + (r.ok() ? "" : r.err().str()))) {
            same(t, r.value(), row.result, row.name);
        }
        std::vector<std::string> urls, live;
        for (const Call& x : c.t.log) urls.push_back(x.url.substr(8));
        for (const std::string& u : c->live_urls()) live.push_back(u.substr(8));
        t.check(urls == row.urls, row.name + ": urls " + scfg::json::dumps(strs(urls)).value());
        t.check(live == row.live, row.name + ": live " + scfg::json::dumps(strs(live)).value());
    }
}

SCFG_TEST(RpcTests, ACodeBeyondInt64IsNoCode) {
    // Python: RpcError(10**30, "m"), whose code equals none the core compares with. The port keeps no code
    // (rpc_code is empty), so str(e)'s "1000000000000000000000000000000: m" is not reproduced (a divergence only
    // a hostile node reaches; nothing in the core prints str(RpcError)).
    Sx c({{"https://a", {err_raw(Json::dict({{"code", BigInt(10).pow(30)}, {"message", "m"}}))}}});
    Result<Json> r = run(c->call("m", Json::list()));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "RpcError");
        SCFG_CHECK(t, !r.err().rpc_code.has_value());
        SCFG_CHECK_EQ(t, r.err().message, "m");
    }
}

SCFG_TEST(RpcTests, AnInfiniteCodeIsAnOverflowErrorAsInPython) {
    // Python: int_from(inf) raises OverflowError, which _parse_error does not catch
    Fx c({{A, answer(Json())}});
    c.t.handlers[0].second = [](const std::string&, const Json&) -> Result<Json> {
        return err_raw(Json::dict({{"code", std::numeric_limits<double>::infinity()}}));
    };
    Result<Json> r = run(c->call("m", Json::list()));
    SCFG_CHECK_EQ(t, kind_of(r), "OverflowError");
}

SCFG_TEST(RpcTests, ErrorNullWithoutResultRaisesKeyErrorAndDoesNotCoolDown) {
    Handler a = [](const std::string&, const Json&) -> Result<Json> {
        return Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"error", Json()}});
    };
    Fx c({{A, a}, {B, answer("0x1")}});
    Result<Json> r = run(c->call("eth_blockNumber", Json::list()));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "KeyError");
        SCFG_CHECK_EQ(t, r.err().message, "'result'");
    }
    SCFG_CHECK(t, c->live_urls() == std::vector<std::string>({A, B}));
}

// ---- scenarios recorded from Python (scen.py) ----

SCFG_TEST(RpcTests, RotationAndCooldownOnAnIntegerClockMatchPython) {
    const std::string a = "https://a", b = "https://b";
    ScriptedTransport tr;
    tr.script[a] = {down(), down(), ok("0xa")};
    tr.script[b] = {ok("0xb"), ok("0xb"), ok("0xb"), ok("0xb")};
    double now = 1000;
    Result<RpcClient> made = RpcClient::make({a, b}, tr, {.clock = [&now] { return now; }});
    if (!SCFG_CHECK(t, made.ok())) return;
    RpcClient c = made.take();
    auto number = [&c] {
        Result<BigInt> r = run(c.block_number());
        return r.ok() ? r.value().to_decimal() : r.err().str();
    };
    SCFG_CHECK_EQ(t, number(), "11");
    now += 29;
    SCFG_CHECK_EQ(t, number(), "11");
    SCFG_CHECK(t, c.live_urls() == std::vector<std::string>({b}));
    now += 1;
    SCFG_CHECK(t, c.live_urls() == std::vector<std::string>({a, b}));
    SCFG_CHECK_EQ(t, number(), "11");
    SCFG_CHECK(t, c.cooldown_of(a) == 60.0 && c.cooldown_until(a) == 1090.0);
    now += 60;
    SCFG_CHECK_EQ(t, number(), "10");
    SCFG_CHECK(t, c.cooldown_of(a) == 0.0 && c.cooldown_until(a) == 0.0);
    std::string log;
    for (const Call& x : tr.log)
        log += x.url.substr(8) + std::to_string(x.payload.as_dict().get("id").as_int64()) + x.method + " ";
    SCFG_CHECK_EQ(t, log,
                  "a1eth_blockNumber b2eth_blockNumber b3eth_blockNumber a4eth_blockNumber b5eth_blockNumber "
                  "a6eth_blockNumber ");
}

SCFG_TEST(RpcTests, GetLogsSplittingAcrossAFailoverMatchesPython) {
    Sx c({{"https://a", {err(-32005, "block range is too large"), ok(Json::list({Json::dict({{"n", 1}})})), down()}},
          {"https://b",
           {err(-32602, "limited to 0 - 50 blocks range"), ok(Json::list({Json::dict({{"n", 2}})})), ok("ab"),
            ok(Json::dict({{"k", 1}})), ok(Json()), ok(Json::list()), down()}}},
         400);
    Result<std::vector<Json>> r = run(c->get_logs("0xT", Json::list({"0xt"}), 100, 599));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "RpcUnavailable");
        SCFG_CHECK_EQ(t, r.err().message, "all endpoints failed for eth_getLogs");
    }
    std::string log;
    for (const Call& x : c.t.log) {
        const Json::Dict& f = x.params.as_list()[0].as_dict();
        log += x.url.substr(8) + std::to_string(x.payload.as_dict().get("id").as_int64()) + ":" +
               f.get("fromBlock").as_str() + "-" + f.get("toBlock").as_str() + " ";
    }
    SCFG_CHECK_EQ(t, log,
                  "a1:0x64-0x1f3 a2:0x64-0x12b a3:0x12c-0x1f3 b4:0x12c-0x1f3 b5:0x12c-0x15d b6:0x15e-0x18f "
                  "b7:0x190-0x1c1 b8:0x1c2-0x1f3 b9:0x1f4-0x225 b10:0x226-0x257 ");
    SCFG_CHECK(t, c->range_of("https://a") == 200 && c->range_of("https://b") == 50);
    SCFG_CHECK(t, c->log_range() == 200);  // every endpoint cooling down: the first one's
}

SCFG_TEST(RpcTests, GetLogsCollectsWhatPythonCollects) {
    Sx c({{"https://a", {ok(Json::list({Json::dict({{"n", 1}})})), ok("ab"), ok(Json::dict({{"k", 1}})), ok(Json())}}}, 10, 0);
    Result<std::vector<Json>> logs = run(c->get_logs("0xT", Json::list(), 0, 39));
    if (SCFG_CHECK(t, logs.ok())) same(t, Json(logs.value()), Json::list({Json::dict({{"n", 1}}), "a", "b", "k"}), "logs");
}

SCFG_TEST(RpcTests, CallAtAndTryAtMatchPython) {
    Sx c({{"https://a", {down(), err(3, "x"), ok(1)}}, {"https://b", {}}});
    Result<Json> e = run(c->call_at("https://a", "m", Json::list()));
    if (SCFG_CHECK(t, !e.ok())) {
        SCFG_CHECK_EQ(t, e.err().kind, "RpcUnavailable");
        SCFG_CHECK_EQ(t, e.err().message, "https://a failed for m");
    }
    // call_at/try_at ask the endpoint even while it cools down
    Result<std::optional<rpc::Attempt>> failed = run(c->try_at("https://a", "m", Json::list()));
    if (SCFG_CHECK(t, failed.ok() && failed.value() && failed.value()->error)) {
        const Err& x = *failed.value()->error;
        SCFG_CHECK(t, x.kind == "RpcError" && x.rpc_code == 3 && x.message == "x" && x.url == "https://a");
    }
    Result<std::optional<rpc::Attempt>> answered = run(c->try_at("https://a", "m", Json::list()));
    if (SCFG_CHECK(t, answered.ok() && answered.value() && !answered.value()->error))
        same(t, answered.value()->result, 1, "result");
    SCFG_CHECK(t, c->cooldown_of("https://a") == 0.0);
    Fx d({{A, fails()}});
    Result<std::optional<rpc::Attempt>> none = run(d->try_at(A, "m", Json::list()));
    SCFG_CHECK(t, none.ok() && !none.value().has_value());
    SCFG_CHECK(t, d->cooldown_of(A) == 30.0);
    // An endpoint not in the list is Python's KeyError from _fail (or _ok): self._state[url].
    c.t.script["https://nope"] = {down(), ok(1)};
    for (int i = 0; i < 2; ++i) {
        Result<std::optional<rpc::Attempt>> unknown = run(c->try_at("https://nope", "m", Json::list()));
        if (SCFG_CHECK(t, !unknown.ok())) {
            SCFG_CHECK_EQ(t, unknown.err().kind, "KeyError");
            SCFG_CHECK_EQ(t, unknown.err().message, "'https://nope'");
        }
    }
}

// ---- the continuations never block ----

SCFG_TEST(RpcTests, ACallWaitsForEachAnswerWithoutBlocking) {
    DeferredTransport tr;
    Result<RpcClient> made = RpcClient::make({A, B}, tr, {.clock = [] { return 0.0; }});
    if (!SCFG_CHECK(t, made.ok())) return;
    RpcClient c = made.take();
    Co<BigInt> call = c.block_number();
    SCFG_CHECK(t, !call.done());
    SCFG_CHECK(t, tr.urls == std::vector<std::string>({A}));
    tr.pending[0].complete(Err::transport_error("down", std::nullopt));
    SCFG_CHECK(t, !call.done());
    SCFG_CHECK(t, tr.urls == std::vector<std::string>({A, B}));
    tr.pending[1].complete(ok("0x2a"));
    if (!SCFG_CHECK(t, call.done())) return;
    Result<BigInt> r = call.take();
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK(t, r.value() == 42);
}

SCFG_TEST(RpcTests, ATransportThatDropsARequestCancelsTheCall) {
    // Phase 2's transport on shutdown: its promises are destroyed incomplete. CancelledError is no endpoint
    // failure: it reaches the caller and cools nothing down.
    DeferredTransport tr;
    Result<RpcClient> made = RpcClient::make({A, B}, tr, {.clock = [] { return 0.0; }});
    if (!SCFG_CHECK(t, made.ok())) return;
    RpcClient c = made.take();
    Co<BigInt> call = c.block_number();
    std::vector<Promise<Json>> dropped = std::move(tr.pending);
    tr.pending.clear();
    dropped.clear();
    if (!SCFG_CHECK(t, call.done())) return;
    Result<BigInt> r = call.take();
    if (SCFG_CHECK(t, !r.ok())) SCFG_CHECK_EQ(t, r.err().kind, "CancelledError");
    SCFG_CHECK(t, c.live_urls() == std::vector<std::string>({A, B}));
    SCFG_CHECK(t, tr.urls == std::vector<std::string>({A}));
    // Cancelling from the other side: destroying the call while it waits; the late answer resumes nothing.
    Co<BigInt> waiting = c.block_number();
    { Co<BigInt> gone = std::move(waiting); }
    tr.pending[0].complete(ok("0x1"));
    SCFG_CHECK(t, c.cooldown_of(A) == 0.0);
}

// ---- SolanaRpc ----

namespace scfg_test::rpc_tests {

const std::string SA = "https://a.example";
const std::string SB = "https://b.example";

// A SolanaRpc over an RpcClient over a FakeTransport.
struct Sol {
    FakeTransport t;
    std::optional<RpcClient> c;
    std::optional<srpc::SolanaRpc> rpc;

    explicit Sol(Handlers handlers) {
        t.handlers = std::move(handlers);
        std::vector<std::string> urls;
        for (const auto& h : t.handlers) urls.push_back(h.first);
        Result<RpcClient> r = RpcClient::make(urls, t, {.clock = [] { return 0.0; }});
        c.emplace(r.take());
        rpc.emplace(*c);
    }
    Sol(const Sol&) = delete;
    Sol& operator=(const Sol&) = delete;
    srpc::SolanaRpc* operator->() { return &*rpc; }
};

// A SolanaRpc over a ScriptedTransport.
struct SolS {
    Sx x;
    std::optional<srpc::SolanaRpc> rpc;

    explicit SolS(std::vector<std::pair<std::string, std::vector<Result<Json>>>> script) : x(std::move(script)) {
        rpc.emplace(*x.c);
    }
    srpc::SolanaRpc* operator->() { return &*rpc; }
};

int64_t max_version(const Json& p) {
    Json v = p.as_list()[1].as_dict().get("maxSupportedTransactionVersion");
    return v.is_int64() ? v.as_int64() : -1;
}

const std::string kV2 =
    "Transaction version (2) is not supported by the requesting client. Please try the request again with the "
    "following configuration parameter: \"maxSupportedTransactionVersion\": 2";

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
    return s;
}

// getTransaction answers -32015 naming `needs` until asked for it; getFirstAvailableBlock is 0.
Handler versioned(int64_t needs) {
    return [needs](const std::string& m, const Json& p) -> Result<Json> {
        if (m == "getFirstAvailableBlock") return ok(0);
        std::string n = std::to_string(needs);
        if (max_version(p) < needs) return err(-32015, replace_all(replace_all(kV2, "(2)", "(" + n + ")"), ": 2", ": " + n));
        return ok(Json::dict({{"slot", 5}}));
    };
}

Handlers history(int64_t first_a, int64_t first_b, Json tx_b = Json()) {
    auto make = [](int64_t first, Json tx) -> Handler {
        return [first, tx](const std::string& m, const Json&) -> Result<Json> {
            if (m == "getFirstAvailableBlock") return ok(first);
            return ok(tx);
        };
    };
    return {{SA, make(first_a, Json())}, {SB, make(first_b, std::move(tx_b))}};
}

std::string tx_any(const Result<srpc::TxAny>& r) {
    if (!r.ok()) return r.err().str();
    return scfg::json::dumps(r.value().tx).value() + (r.value().authoritative ? " authoritative" : " not authoritative");
}

}  // namespace scfg_test::rpc_tests

SCFG_TEST(SolanaRpcTests, GetTransactionAsksForNewestVersionAndJson) {
    Sol s({{SA, answer(Json::dict({{"slot", 5}}))}});
    Result<Json> r = run(s->get_transaction("SIG"));
    if (SCFG_CHECK(t, r.ok())) same(t, r.value(), Json::dict({{"slot", 5}}), "tx");
    SCFG_CHECK_EQ(t, s.t.calls[0].method, "getTransaction");
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls[0].params).value(),
                  "[\"SIG\", {\"encoding\": \"json\", \"commitment\": \"confirmed\", \"maxSupportedTransactionVersion\": 1}]");
    SCFG_CHECK_EQ(t, srpc::TX_VERSION, 1);
}

SCFG_TEST(SolanaRpcTests, GetTransactionLowersVersionWhenTheNodeRejectsTheParameter) {
    Handler h = [](const std::string&, const Json& p) -> Result<Json> {
        if (max_version(p) == 1) return err(-32602, "Invalid params: unknown maxSupportedTransactionVersion 1");
        return ok(Json::dict({{"slot", 5}}));
    };
    Sol s({{SA, h}});
    Result<Json> r = run(s->get_transaction("SIG"));
    if (SCFG_CHECK(t, r.ok())) same(t, r.value(), Json::dict({{"slot", 5}}), "tx");
    SCFG_CHECK_EQ(t, s->tx_version, 0);
    // At version 0 there is nothing lower to try: the node's error is raised.
    Handler never = [](const std::string&, const Json&) -> Result<Json> { return err(-32602, "bad VERSION"); };
    Sol n({{SA, never}});
    n->tx_version = 0;
    SCFG_CHECK_EQ(t, kind_of(run(n->get_transaction("SIG"))), "RpcError");
    SCFG_CHECK_EQ(t, n.t.calls.size(), 1);
}

SCFG_TEST(SolanaRpcTests, TryAtEachLiveUrlReportsPerEndpointAndDeadOnesCoolDown) {
    Handler boom = [](const std::string&, const Json&) -> Result<Json> { return err(-1, "boom"); };
    Sol s({{SA, fails()}, {SB, boom}});
    RpcClient& c = s->client();
    std::vector<std::pair<std::string, rpc::Attempt>> outs;
    for (const std::string& url : c.live_urls()) {
        Result<std::optional<rpc::Attempt>> res = run(c.try_at(url, "getSlot", Json::list()));
        if (res.ok() && res.value()) outs.emplace_back(url, *res.value());
    }
    if (SCFG_CHECK_EQ(t, outs.size(), 1)) {
        SCFG_CHECK_EQ(t, outs[0].first, SB);
        SCFG_CHECK(t, outs[0].second.error && outs[0].second.error->kind == "RpcError");
    }
    SCFG_CHECK(t, c.live_urls() == std::vector<std::string>({SB}));
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyReturnsFirstFound) {
    Sol s(history(1000000000, 0, Json::dict({{"slot", 50}})));
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(50)))), "{\"slot\": 50} not authoritative");
}

SCFG_TEST(SolanaRpcTests, NullFromFullHistoryEndpointIsAuthoritative) {
    Sol s(history(1000000000, 0));
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(50)))), "null authoritative");
    // Python: first_available <= min_slot, so history starting at the slot itself is enough
    Sol edge(history(51, 50));
    SCFG_CHECK_EQ(t, tx_any(run(edge->get_transaction_any("SIG", BigInt(50)))), "null authoritative");
    Sol pruned(history(51, 51));
    SCFG_CHECK_EQ(t, tx_any(run(pruned->get_transaction_any("SIG", BigInt(50)))), "null not authoritative");
}

SCFG_TEST(SolanaRpcTests, NullOnlyFromPrunedEndpointsIsNotAuthoritative) {
    Sol s(history(1000000000, 1000000000));
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(50)))), "null not authoritative");
    // Without min_slot no history is asked for.
    Sol n(history(0, 0));
    SCFG_CHECK_EQ(t, tx_any(run(n->get_transaction_any("SIG"))), "null not authoritative");
    for (const Call& x : n.t.calls) SCFG_CHECK_EQ(t, x.method, "getTransaction");
    // The first slot is cached per endpoint.
    (void)run(s->get_transaction_any("SIG", BigInt(50)));
    int firsts = 0;
    for (const Call& x : s.t.calls) firsts += x.method == "getFirstAvailableBlock";
    SCFG_CHECK_EQ(t, firsts, 2);
}

SCFG_TEST(SolanaRpcTests, UnreadableFirstAvailableBlockCountsAsPruned) {
    Handler garbage = [](const std::string& m, const Json&) -> Result<Json> {
        if (m == "getFirstAvailableBlock") return ok("abc");
        return ok(Json());
    };
    Sol s({{SA, garbage}, {SB, garbage}});
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(50)))), "null not authoritative");
    // not cached: asked again next time
    (void)run(s->get_transaction_any("SIG", BigInt(50)));
    int firsts = 0;
    for (const Call& x : s.t.calls) firsts += x.method == "getFirstAvailableBlock";
    SCFG_CHECK_EQ(t, firsts, 4);
    // A float infinity is int_from's OverflowError, which _first_available does not catch.
    Handler inf = [](const std::string& m, const Json&) -> Result<Json> {
        if (m == "getFirstAvailableBlock") return ok(std::numeric_limits<double>::infinity());
        return ok(Json());
    };
    Sol o({{SA, inf}});
    SCFG_CHECK(t, tx_any(run(o->get_transaction_any("SIG", BigInt(50)))).rfind("OverflowError", 0) == 0);
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyWithoutAnswersRaises) {
    Sol s({{SA, fails()}, {SB, fails()}});
    Result<srpc::TxAny> r = run(s->get_transaction_any("SIG", BigInt(1)));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "RpcUnavailable");
        SCFG_CHECK_EQ(t, r.err().message, "no endpoint answered getTransaction for SIG");
    }
    // str(sig) of a signature that is not a str
    Sol n({{SA, fails()}});
    Result<srpc::TxAny> e = run(n->get_transaction_any(Json::list({1})));
    if (SCFG_CHECK(t, !e.ok())) SCFG_CHECK_EQ(t, e.err().message, "no endpoint answered getTransaction for [1]");
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyDefaultCommitmentIsFinalized) {
    Sol s({{SA, answer(Json::dict({{"slot", 1}}))}});
    (void)run(s->get_transaction_any("SIG"));
    same(t, s.t.calls[0].params.as_list()[1].as_dict().get("commitment"), "finalized", "commitment");
}

SCFG_TEST(SolanaRpcTests, SmallHelpers) {
    std::map<std::string, Json> answers = {
        {"getSlot", 77},
        {"getSignaturesForAddress", Json::list({Json::dict({{"signature", "S2"}}), Json::dict({{"signature", "S1"}})})},
        {"getSignatureStatuses", Json::dict({{"value", Json::list({Json(), Json::dict({{"confirmationStatus", "finalized"}})})}})},
        {"getAccountInfo", Json::dict({{"value", Json()}})},
        {"getLatestBlockhash", Json::dict({{"value", Json::dict({{"blockhash", "BH"}, {"lastValidBlockHeight", 9}})}})},
        {"getBalance", Json::dict({{"value", 1234}})},
        {"getRecentPrioritizationFees",
         Json::list({Json::dict({{"slot", 1}, {"prioritizationFee", 9}}), Json::dict({{"slot", 1}, {"prioritizationFee", 1}}),
                     Json::dict({{"slot", 1}, {"prioritizationFee", 5}})})},
    };
    Handler h = [&answers](const std::string& m, const Json&) -> Result<Json> { return ok(answers[m]); };
    Sol s({{SA, h}});
    Result<BigInt> slot = run(s->get_slot());
    SCFG_CHECK(t, slot.ok() && slot.value() == 77);
    same(t, s.t.calls.back().params, Json::list({Json::dict({{"commitment", "confirmed"}})}), "getSlot params");
    Result<Json> sigs = run(s->get_signatures_for_address("X", {.until = "S0"}));
    if (SCFG_CHECK(t, sigs.ok())) same(t, sigs.value(), answers["getSignaturesForAddress"], "signatures");
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls.back().params).value(),
                  "[\"X\", {\"limit\": 100, \"commitment\": \"confirmed\", \"until\": \"S0\"}]");
    Result<Json> statuses = run(s->get_signature_statuses(Json::list({"a", "b"})));
    if (SCFG_CHECK(t, statuses.ok()))
        same(t, statuses.value().as_list()[1].as_dict().get("confirmationStatus"), "finalized", "status");
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls.back().params).value(), "[[\"a\", \"b\"], {\"searchTransactionHistory\": true}]");
    Result<bool> exists = run(s->account_exists("X"));
    SCFG_CHECK(t, exists.ok() && !exists.value());
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls.back().params).value(), "[\"X\", {\"encoding\": \"base64\", \"commitment\": \"confirmed\"}]");
    Result<BigInt> token = run(s->get_token_balance("X"));
    SCFG_CHECK(t, token.ok() && token.value() == 0);
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls.back().params).value(),
                  "[\"X\", {\"encoding\": \"jsonParsed\", \"commitment\": \"confirmed\"}]");
    Result<Json> bh = run(s->get_latest_blockhash());
    if (SCFG_CHECK(t, bh.ok())) same(t, bh.value(), "BH", "blockhash");
    Result<BigInt> balance = run(s->get_balance("X"));
    SCFG_CHECK(t, balance.ok() && balance.value() == 1234);
    Result<BigInt> fee = run(s->get_priority_fee(Json::list({"X"})));
    SCFG_CHECK(t, fee.ok() && fee.value() == 5);
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls.back().params).value(), "[[\"X\"]]");
}

SCFG_TEST(SolanaRpcTests, SignatureOptionsBeforeAndEmptyResult) {
    Sol s({{SA, answer(Json())}});
    Result<Json> r = run(s->get_signatures_for_address("X", {.before = "B1", .limit = 5, .commitment = "finalized"}));
    if (SCFG_CHECK(t, r.ok())) same(t, r.value(), Json::list(), "empty");
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.t.calls[0].params).value(),
                  "[\"X\", {\"limit\": 5, \"commitment\": \"finalized\", \"before\": \"B1\"}]");
}

SCFG_TEST(SolanaRpcTests, EmptyResultsReadAsEmptyListsLikePythonOr) {
    for (const Json& empty : {Json::dict(), Json(""), Json(0), Json(false)}) {
        Sol s({{SA, answer(empty)}});
        Result<Json> sigs = run(s->get_signatures_for_address("X"));
        t.check(sigs.ok() && sigs.value().is_list() && sigs.value().as_list().empty(), py::repr(empty).value());
        Result<BigInt> fee = run(s->get_priority_fee(Json::list({"X"})));
        t.check(fee.ok() && fee.value() == 0, py::repr(empty).value());
    }
}

SCFG_TEST(SolanaRpcTests, TokenBalanceReadsParsedAmount) {
    Json body = Json::dict({{"value", Json::dict({{"data", Json::dict({{"parsed", Json::dict({{"info",
                Json::dict({{"tokenAmount", Json::dict({{"amount", "18446744073709551615"}})}})}})}})}})}});
    Sol s({{SA, answer(body)}});
    Result<BigInt> r = run(s->get_token_balance("T"));
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value().to_decimal(), "18446744073709551615");
}

SCFG_TEST(SolanaRpcTests, GetTransactionRetriesOnceWithTheVersionTheNodeNames) {
    Sol s({{SA, versioned(2)}});
    Result<Json> r = run(s->get_transaction("SIG"));
    if (SCFG_CHECK(t, r.ok())) same(t, r.value(), Json::dict({{"slot", 5}}), "tx");
    std::vector<int64_t> asked;
    for (const Call& x : s.t.calls) asked.push_back(max_version(x.params));
    SCFG_CHECK(t, asked == std::vector<int64_t>({1, 2}));
    SCFG_CHECK_EQ(t, s->tx_version, 1);
}

SCFG_TEST(SolanaRpcTests, UnsupportedVersionErrorNamingAnOlderVersionIsRaised) {
    Handler h = [](const std::string&, const Json&) -> Result<Json> { return err(-32015, replace_all(kV2, "(2)", "(0)")); };
    Sol s({{SA, h}});
    SCFG_CHECK_EQ(t, kind_of(run(s->get_transaction("SIG"))), "RpcError");
    SCFG_CHECK_EQ(t, s.t.calls.size(), 1);
}

SCFG_TEST(SolanaRpcTests, NamedVersionIgnoresNumbersBeyond18Digits) {
    struct Row {
        std::string message;
        std::optional<int64_t> expected;
    };
    const std::vector<Row> rows = {
        {"Transaction version (" + std::string(19, '9') + ") is not supported", std::nullopt},
        {"Transaction version (" + std::string(4301, '9') + ") is not supported", std::nullopt},
        {"Transaction version (" + std::string(1000000, '9') + ") is not supported", std::nullopt},
        {"Transaction version (" + std::string(15, '9') + ") is not supported", 999999999999999},
        {"Transaction version (" + std::string(30, '0') + "3) is not supported", 3},
        {"version (" + std::string(19, '9') + ") version (5)", std::nullopt},
        {"version (12 version (5)", 5},
        {"version (\xd9\xa3)", 3},
        {"Transaction VER\xc5\xbfION (2) x", 2},
        {"vers\xc4\xb1on (4)", 4},
        {"vers\xc4\xb0on (4)", 4},
        {"Version (2", std::nullopt},
        {"version(2)", std::nullopt},
        {"version (x) version (6)", 6},
        {"version (1)", std::nullopt},
        {"version (\xf0\x91\xbd\x93)", std::nullopt},  // a Kawi digit (Unicode 15): not a digit in Python 3.9
        {"version (\xf0\x9d\x9f\x91)", 3},             // a mathematical bold digit: outside the BMP, a digit in 3.9
        {"VERSION (\xef\xbc\x97)", 7},
        {"\xe2\x84\xaaversion (8)", 8},                // a Kelvin sign before: no letter of "version"
        {"versio\xe2\x84\xaa (8)", std::nullopt},
        {"version \xef\xbc\x88" "8)", std::nullopt},    // a fullwidth parenthesis is not "("
        {std::string("version (8") + std::string(100000, '0'), std::nullopt},
    };
    for (const Row& row : rows) {
        std::optional<int64_t> got = srpc::named_version(Err::rpc_error(-32015, row.message), 1);
        t.check(got == row.expected, row.message.substr(0, 40) + ": " + (got ? std::to_string(*got) : "None"));
    }
    SCFG_CHECK(t, !srpc::named_version(Err::rpc_error(-32000, "version (5)"), 1).has_value());
    SCFG_CHECK(t, !srpc::named_version(Err::rpc_error(-32015, "version (5)"), 5).has_value());
    Err no_code = Err::make("RpcError", "version (5)");
    SCFG_CHECK(t, !srpc::named_version(no_code, 1).has_value());
}

SCFG_TEST(SolanaRpcTests, AnOverlongNamedVersionRaisesTheNodeError) {
    std::string message = replace_all(kV2, "(2)", "(" + std::string(4301, '9') + ")");
    Handler h = [message](const std::string&, const Json&) -> Result<Json> { return err(-32015, message); };
    Sol s({{SA, h}});
    SCFG_CHECK_EQ(t, kind_of(run(s->get_transaction("SIG"))), "RpcError");
    SCFG_CHECK_EQ(t, s.t.calls.size(), 1);
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyRetriesWithTheNamedVersion) {
    Sol s({{SA, versioned(3)}});
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(1)))), "{\"slot\": 5} not authoritative");
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyOverThreeEndpointsMatchesPython) {
    // Python: scen.py "sol_any"
    SolS s({{"https://a", {down()}},
            {"https://b", {err(-32015, "Transaction version (3) is not supported"), ok(Json()), ok(5)}},
            {"https://c", {ok(Json()), ok("bad")}}});
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG", BigInt(10)))), "null authoritative");
    std::string log;
    for (const Call& x : s.x.t.log) {
        log += x.url.substr(8) + std::to_string(x.payload.as_dict().get("id").as_int64()) + x.method +
               (x.method == "getTransaction" ? std::to_string(max_version(x.params)) : "") + " ";
    }
    SCFG_CHECK_EQ(t, log, "a1getTransaction1 b2getTransaction1 b3getTransaction3 b4getFirstAvailableBlock c5getTransaction1 "
                          "c6getFirstAvailableBlock ");
    same(t, s.x.t.log[0].params.as_list()[1].as_dict().get("commitment"), "finalized", "commitment");
}

SCFG_TEST(SolanaRpcTests, GetTransactionAnyLowersTheVersionAndStartsOver) {
    // Python: a -32602 naming the version lowers tx_version and asks every live endpoint again
    SolS s({{"https://a", {err(-32602, "unknown maxSupportedTransactionVersion"), ok(Json::dict({{"slot", 2}}))}}});
    SCFG_CHECK_EQ(t, tx_any(run(s->get_transaction_any("SIG"))), "{\"slot\": 2} not authoritative");
    SCFG_CHECK_EQ(t, s->tx_version, 0);
    if (SCFG_CHECK_EQ(t, s.x.t.log.size(), 2)) SCFG_CHECK_EQ(t, max_version(s.x.t.log[1].params), 0);
    // A node error that names no version skips that endpoint; a failing retry with the named version does too.
    SolS k({{"https://a", {err(-32000, "nope")}},
            {"https://b", {err(-32015, "Transaction version (2) is not supported"), down()}},
            {"https://c", {ok(Json::dict({{"slot", 3}}))}}});
    SCFG_CHECK_EQ(t, tx_any(run(k->get_transaction_any("SIG"))), "{\"slot\": 3} not authoritative");
    // Nothing answered but node errors: no endpoint answered.
    SolS e({{"https://a", {err(-32000, "nope")}}});
    SCFG_CHECK(t, tx_any(run(e->get_transaction_any("SIG"))).rfind("RpcUnavailable", 0) == 0);
}

SCFG_TEST(SolanaRpcTests, PriorityFeeAndSignatureFormsMatchPython) {
    // Python: scen.py "sol_misc"
    SolS s({{"https://a",
             {ok(Json::dict({{"a", 1}})),
              ok(Json::list({Json::dict({{"prioritizationFee", "7"}}), Json::dict({{"prioritizationFee", 3}}),
                             Json::dict({{"prioritizationFee", 5.9}}), Json::dict({{"prioritizationFee", true}})})),
              ok(Json::dict({{"x", 1}}))}}});
    Result<BigInt> e = run(s->get_priority_fee(Json::list({"X"})));
    if (SCFG_CHECK(t, !e.ok())) {
        SCFG_CHECK_EQ(t, e.err().kind, "TypeError");
        SCFG_CHECK_EQ(t, e.err().message, "string indices must be integers");
    }
    Result<BigInt> fee = run(s->get_priority_fee(Json::list({"X"})));
    SCFG_CHECK(t, fee.ok() && fee.value() == 5);
    Result<Json> sigs = run(s->get_signatures_for_address("X", {.until = "", .before = "B", .limit = 3}));
    if (SCFG_CHECK(t, sigs.ok())) same(t, sigs.value(), Json::dict({{"x", 1}}), "signatures");
    SCFG_CHECK_EQ(t, scfg::json::dumps(s.x.t.log[2].params).value(), "[\"X\", {\"limit\": 3, \"commitment\": \"confirmed\", \"before\": \"B\"}]");
}

SCFG_TEST(SolanaRpcTests, MalformedRepliesRaiseWherePythonRaises) {
    struct Row {
        std::string name;
        Json body;
        int which;
    };
    const std::vector<Row> rows = {
        {"account_exists_missing_value", Json::dict(), 0},
        {"account_exists_non_dict", Json(), 0},
        {"signature_statuses_missing_value", Json::dict(), 1},
        {"latest_blockhash_missing_value", Json::dict(), 2},
        {"latest_blockhash_missing_blockhash", Json::dict({{"value", Json::dict()}}), 2},
        {"token_balance_missing_value", Json::dict(), 3},
        {"token_balance_missing_nested", Json::dict({{"value", Json::dict({{"data", Json::dict()}})}}), 3},
        {"token_balance_non_object_nested", Json::dict({{"value", Json::dict({{"data", "x"}})}}), 3},
        {"balance_missing_value", Json::dict(), 4},
        {"priority_fee_missing_field", Json::list({Json::dict({{"slot", 1}})}), 5},
    };
    for (const Row& row : rows) {
        Sol s({{SA, answer(row.body)}});
        std::string kind;
        switch (row.which) {
            case 0: {
                Result<bool> r = run(s->account_exists("X"));
                kind = r.ok() ? "ok" : r.err().kind;
                break;
            }
            case 1: kind = kind_of(run(s->get_signature_statuses(Json::list({"a"})))); break;
            case 2: kind = kind_of(run(s->get_latest_blockhash())); break;
            case 3: {
                Result<BigInt> r = run(s->get_token_balance("X"));
                kind = r.ok() ? "ok" : r.err().kind;
                break;
            }
            case 4: {
                Result<BigInt> r = run(s->get_balance("X"));
                kind = r.ok() ? "ok" : r.err().kind;
                break;
            }
            default: {
                Result<BigInt> r = run(s->get_priority_fee(Json::list({"X"})));
                kind = r.ok() ? "ok" : r.err().kind;
                break;
            }
        }
        t.check(kind == "KeyError" || kind == "TypeError", row.name + ": " + kind);
    }
}
