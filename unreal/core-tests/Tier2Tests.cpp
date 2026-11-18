// Copyright 2026 Repulse
// No Python twin: Tier2.<id> replays each conformance/vectors/tier2 vector through support/Tier2 (one runtime test
// per vector file), and Tier2Tests checks the runner itself (as unity/core-tests/Tier2Tests.cs): a vector that does
// not match fails, malformed data is a difference rather than an abort, and an Err that stands for no Python
// exception satisfies nothing.
#include "support/Helpers.h"
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Restore.h"
#include "Scfg/Co.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace tier2 = scfg_test::tier2;
using scfg_test::load_vector;

namespace scfg_test::tier2_tests {

bool contains(const std::optional<std::string>& s, std::string_view part) {
    return s.has_value() && s->find(part) != std::string::npos;
}

std::string shown(const std::optional<std::string>& s) { return s.value_or("(passed)"); }

// What a transport's Future holds, read through a coroutine (a Future is only awaited).
scfg::Co<Json> await_future(scfg::Future<Json> f) { co_return co_await std::move(f); }

Result<Json> answer(scfg::Future<Json> f) {
    scfg::Co<Json> c = await_future(std::move(f));
    if (!c.done()) return Err::make("AssertionError", "the answer is not there yet");
    return c.take();
}

Json& step(Json& v, size_t i) { return v.as_dict().find("steps")->as_list()[i]; }
Json& field(Json& d, std::string_view key) { return *d.as_dict().find(key); }
Json::List& calls(Json& v, size_t i) { return field(step(v, i), "calls").as_list(); }

// The smoke vector, untouched (it must pass), then with `change` applied.
std::optional<std::string> tampered(Ctx& t, const std::function<void(Json&)>& change) {
    std::optional<Json> v = load_vector("smoke/evm_unlock_paid");
    if (!t.check(v.has_value(), "smoke/evm_unlock_paid cannot be read")) return "unreadable";
    std::optional<std::string> clean = tier2::replay(*v);
    t.check(!clean.has_value(), "the untouched vector fails: " + shown(clean));
    Json copy = scfg::deep_copy(*v);
    change(copy);
    return tier2::replay(copy);
}

// A one-step vector over the smoke vector's setup.
Json one_step(Json setup, Json recorded_step) {
    return Json::dict({{"tier", 2}, {"id", "probe"}, {"setup", std::move(setup)}, {"steps", Json::list({std::move(recorded_step)})}});
}

Json smoke_setup() {
    std::optional<Json> v = load_vector("smoke/evm_unlock_paid");
    return v ? scfg::deep_copy(v->as_dict().get("setup")) : Json::dict();
}

Json empty_snapshot() {
    return Json::dict({{"pending", Json::dict()}, {"receipts", Json::dict()}, {"owned", Json::list()},
                       {"tip_total", "0"}, {"unready", Json::list()}});
}

Json plain_step(std::string action, Json args, Json raises = Json(), int64_t internal_errors = 0,
                Json snapshot = empty_snapshot()) {
    return Json::dict({{"at", 1700000000},
                       {"action", Json(std::move(action))},
                       {"args", std::move(args)},
                       {"entropy", Json::list()},
                       {"calls", Json::list()},
                       {"returns", Json()},
                       {"raises", std::move(raises)},
                       {"internal_errors", internal_errors},
                       {"snapshot", std::move(snapshot)}});
}

}  // namespace scfg_test::tier2_tests

using namespace scfg_test::tier2_tests;

SCFG_TEST_SOURCE(Tier2Vectors) {
    std::string error;
    std::vector<std::string> ids = scfg_test::vectors::tier2_ids(&error);
    if (!error.empty() || ids.empty()) {
        scfg_test::add("Tier2", "listing", [error](scfg_test::Ctx& t) {
            t.fail("cannot list " + scfg_test::vectors::root() + "/tier2: " + (error.empty() ? "no vectors" : error));
        });
    }
    for (const std::string& id : ids) {
        scfg_test::add("Tier2", id, [id](scfg_test::Ctx& t) {
            std::optional<Json> v = load_vector(id);
            if (!t.check(v.has_value(), "tier2/" + id + " cannot be read or parsed")) return;
            std::optional<std::string> diff = tier2::replay(*v);
            t.check(!diff.has_value(), "tier2/" + id + ": " + shown(diff));
        });
    }
}

SCFG_TEST(Tier2Tests, EveryGroupHasVectors) {
    std::vector<std::string> ids = scfg_test::vectors::tier2_ids();
    for (std::string_view prefix : {"smoke/", "evm/", "config/", "custom_chains/", "rpc/", "solana/", "reverify/", "restore/"}) {
        t.check(std::any_of(ids.begin(), ids.end(), [prefix](const std::string& id) { return id.starts_with(prefix); }),
                std::string(prefix) + " has no vector");
    }
}

// ---- the runner catches a vector that does not match ----

SCFG_TEST(Tier2Tests, AMissingRecordedCallIsAnUnexpectedCall) {
    std::optional<std::string> diff = tampered(t, [](Json& v) { calls(v, 1).erase(calls(v, 1).begin() + 2); });
    t.check(contains(diff, "step 1 (tick): unexpected call https://base-sepolia-rpc.publicnode.com eth_getBlockByNumber"),
            shown(diff));
}

SCFG_TEST(Tier2Tests, AnUnusedRecordedCallFailsTheStep) {
    std::optional<std::string> diff = tampered(t, [](Json& v) { calls(v, 0).push_back(scfg::deep_copy(calls(v, 0)[0])); });
    t.check(contains(diff, "step 0 (start_session): expected call not made: https://base-sepolia-rpc.publicnode.com eth_blockNumber"),
            shown(diff));
}

SCFG_TEST(Tier2Tests, ParamsMustMatch) {
    std::optional<std::string> diff = tampered(t, [](Json& v) { field(calls(v, 1)[2], "params").as_list()[1] = Json(0); });
    t.check(contains(diff, "unexpected call") && contains(diff, "expected call not made"), shown(diff));  // false is not 0
}

SCFG_TEST(Tier2Tests, EntropyMustMatchInOrder) {
    std::optional<std::string> diff = tampered(t, [](Json& v) { field(field(step(v, 0), "entropy").as_list()[0], "n") = Json(99); });
    t.check(contains(diff, "entropy mismatch: drew randbelow(100), recorded randbelow(99)"), shown(diff));
    diff = tampered(t, [](Json& v) {
        field(step(v, 0), "entropy").as_list().push_back(Json::dict({{"fn", "token_hex"}, {"n", 8}, {"value", "00"}}));
    });
    t.check(contains(diff, "unconsumed entropy: [('token_hex', 8)]"), shown(diff));
    diff = tampered(t, [](Json& v) {
        Json::List& e = field(step(v, 0), "entropy").as_list();
        e.erase(e.begin() + 1);
    });
    t.check(contains(diff, "entropy exhausted at token_hex(8)"), shown(diff));
}

SCFG_TEST(Tier2Tests, RaisesReturnsErrorsAndSnapshotAreCompared) {
    std::optional<std::string> diff = tampered(t, [](Json& v) { field(step(v, 0), "raises") = Json::dict({{"type", "other"}}); });
    t.check(contains(diff, "step 0 (start_session): raises"), shown(diff));
    diff = tampered(t, [](Json& v) { field(step(v, 0), "returns").as_dict().set("nonce", 7); });
    t.check(contains(diff, "step 0 (start_session): returns: nonce"), shown(diff));
    diff = tampered(t, [](Json& v) { field(step(v, 1), "internal_errors") = Json(1); });
    t.check(contains(diff, "step 1 (tick): internal_errors: recorded 1, got 0"), shown(diff));
    diff = tampered(t, [](Json& v) { field(step(v, 2), "snapshot").as_dict().set("tip_total", "0.0"); });
    t.check(contains(diff, "step 2 (tick): snapshot: tip_total"), shown(diff));
}

// ---- what a recorded raise or a contained error may stand for ----

SCFG_TEST(Tier2Tests, OnlyPythonExceptionsStandForOther) {
    for (const char* kind : {"ValueError", "TypeError", "KeyError", "IndexError", "AttributeError", "OverflowError",
                             "InvalidOperation", "TransportError", "RpcError", "RpcUnavailable", "DataTooLongError"}) {
        std::optional<Json> r = tier2::raises_json(Err::make(kind, "x"));
        t.check(r.has_value() && scfg_test::json_eq(*r, Json::dict({{"type", "other"}})), std::string(kind) + " is not other");
    }
    std::optional<Json> config = tier2::raises_json(Err::config_error("chain.token_unavailable", "x"));
    t.check(config && scfg_test::json_eq(*config, Json::dict({{"type", "ConfigError"}, {"code", "chain.token_unavailable"}})),
            "a ConfigError keeps its code");
    std::optional<Json> restore_e = tier2::raises_json(Err::restore_error("tx_not_found", "tx_not_found"));
    t.check(restore_e && scfg_test::json_eq(*restore_e, Json::dict({{"type", "RestoreError"}, {"code", "tx_not_found"}})),
            "a RestoreError keeps its code");
    for (const char* kind : {"CancelledError", "HostGone"}) {
        t.check(!tier2::raises_json(Err::make(kind, "x")).has_value(), std::string(kind) + " stands for a recorded raise");
        t.check(!tier2::stands_for_python(Err::make(kind, "x")), std::string(kind) + " stands for a contained error");
    }
}

namespace scfg_test::tier2_tests {

// A vector's first step alone, its calls cut to the first `keep`, then changed.
Json first_step_only(const std::string& id, size_t keep, const std::function<void(Json& step)>& change) {
    std::optional<Json> v = load_vector(id);
    if (!v) return Json();
    Json copy = scfg::deep_copy(*v);
    Json::List& steps = field(copy, "steps").as_list();
    steps.resize(1);
    Json::List& c = field(steps[0], "calls").as_list();
    c.resize(std::min(c.size(), keep));
    change(steps[0]);
    return copy;
}

std::optional<std::string> replay_with_fault(const Json& v, const std::string& kind) {
    if (v.is_null()) return std::string("the vector cannot be read");
    tier2::ReplayTransport transport;
    transport.fault = [kind](const std::string&, const Json&) -> std::optional<Err> { return Err::make(kind, "x"); };
    return tier2::replay(v, transport);
}

}  // namespace scfg_test::tier2_tests

// An Err that stands for no Python exception (the Godot-only HostGone, from a host's transport) raised where the
// vector recorded "other" fails the step; a Python exception in its place satisfies it. restore/unlock_found's
// lookup, its first request failing: Python's lookup swallows only RpcUnavailable and RpcError.
SCFG_TEST(Tier2Tests, ANonPythonErrNeverSatisfiesARecordedOther) {
    Json v = first_step_only("restore/unlock_found", 1, [](Json& s) {
        s.as_dict().set("raises", Json::dict({{"type", "other"}}));
        s.as_dict().set("returns", Json());
    });
    std::optional<std::string> python = replay_with_fault(v, "KeyError");
    t.check(!python.has_value(), "a KeyError is a recorded other: " + shown(python));
    std::optional<std::string> diff = replay_with_fault(v, "HostGone");
    t.check(contains(diff, "step 0 (restore_lookup): raised an Err that no recorded raise stands for: HostGone"),
            shown(diff));
}

// What the Engine contains (a check that failed on node data in reverify, a session step's crash in tick) must be
// an exception Python would have contained too: an Err that stands for none (HostGone) reaching on_error fails the
// step, end to end, even where the vector counted that internal error.
SCFG_TEST(Tier2Tests, AContainedNonPythonErrFailsTheStep) {
    // reverify/genuine_receipt_is_kept: eth_blockNumber fails, so the receipt is unverifiable: kept, one report
    Json v = first_step_only("reverify/genuine_receipt_is_kept", 1, [](Json& s) { s.as_dict().set("internal_errors", 1); });
    std::optional<std::string> python = replay_with_fault(v, "KeyError");
    t.check(!python.has_value(), "a contained KeyError is one internal error: " + shown(python));
    std::optional<std::string> diff = replay_with_fault(v, "HostGone");
    t.check(contains(diff, "step 0 (reverify): on_error got an Err that no contained Python exception stands for: HostGone"),
            shown(diff));
    // smoke/evm_unlock_paid's first tick: its session's step crashes on the first request (contained by tick)
    std::optional<Json> smoke = load_vector("smoke/evm_unlock_paid");
    if (!t.check(smoke.has_value(), "smoke/evm_unlock_paid")) return;
    Json ticked = scfg::deep_copy(*smoke);
    Json::List& steps = field(ticked, "steps").as_list();
    steps.resize(2);
    calls(ticked, 1).resize(1);
    field(steps[1], "internal_errors") = Json(1);
    tier2::ReplayTransport transport;
    transport.fault = [&transport](const std::string&, const Json&) -> std::optional<Err> {
        if (transport.posts() < 2) return std::nullopt;  // start_session's request is answered as recorded
        return Err::make("HostGone", "the host left");
    };
    std::optional<std::string> ticks = tier2::replay(ticked, transport);
    t.check(contains(ticks, "step 1 (tick): on_error got an Err that no contained Python exception stands for: HostGone"),
            shown(ticks));
}

// The rule both checks apply.
SCFG_TEST(Tier2Tests, OnlyAPythonExceptionStandsForAContainedError) {
    for (const char* kind : {"KeyError", "TypeError", "ValueError", "AttributeError", "OverflowError", "RestoreError",
                             "ConfigError", "InvalidOperation"}) {
        t.check(tier2::stands_for_python(Err::make(kind, "x")), std::string(kind) + " stands for Python's");
    }
    for (const char* kind : {"HostGone", "CancelledError"}) {
        t.check(!tier2::stands_for_python(Err::make(kind, "x")), std::string(kind) + " stands for nothing of Python's");
    }
}

// Deferred mode's own checks: a networked step that never suspended, and a request left in the queue, each fail it.
SCFG_TEST(Tier2Tests, DeferredModeFailsAStepThatNeverSuspendedOrLeftARequestQueued) {
    Json call = Json::dict({{"url", "https://x.example"}, {"method", "getSlot"}, {"params", Json::list()},
                            {"response_text", "{\"jsonrpc\": \"2.0\", \"id\": 1, \"result\": 7}"}});
    Json calls = Json::list({call});
    tier2::ReplayTransport immediate;
    t.check(!tier2::deferred_problem(immediate, calls, false).has_value(), "immediate mode has no such checks");
    tier2::ReplayTransport deferred(tier2::ReplayMode::Deferred);
    deferred.load(calls);
    t.check(!tier2::deferred_problem(deferred, calls, true).has_value(), "a step that suspended and drained passes");
    t.check(!tier2::deferred_problem(deferred, Json::list(), false).has_value(), "a step with no calls need not suspend");
    std::optional<std::string> never = tier2::deferred_problem(deferred, calls, false);
    t.check(contains(never, "the step's requests were answered, yet the action never suspended (deferred mode)"),
            shown(never));
    scfg::Future<Json> unanswered =
        deferred.post_json("https://x.example", Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "getSlot"},
                                                           {"params", Json::list()}}), 10);
    t.check(deferred.problems.empty() && deferred.queued() == 1, "one request queued");
    std::optional<std::string> queued = tier2::deferred_problem(deferred, calls, true);
    t.check(contains(queued, "1 request(s) left queued after the action finished (deferred mode)"), shown(queued));
    t.check(deferred.pump_one(), "answered");
    t.check(!tier2::deferred_problem(deferred, calls, true).has_value(), "nothing queued any more");
}

// Python's set_ui_status and discard_session look the sid up (`sid in pending`, pending.get(sid)): a sid that is not
// a str is simply not there, and only an unhashable one raises.
SCFG_TEST(Tier2Tests, ANonStrSidIsANoOp) {
    for (const char* action : {"set_ui_status", "discard_session"}) {
        for (Json sid : {Json(5), Json(), Json(true), Json(1.5)}) {
            Json args = Json::dict({{"sid", sid}, {"status", "open"}});
            std::optional<std::string> diff = tier2::replay(one_step(smoke_setup(), plain_step(action, args)));
            t.check(!diff.has_value(), std::string(action) + " " + scfg::py::repr(sid).value() + ": " + shown(diff));
        }
        Json args = Json::dict({{"sid", Json::list({1})}, {"status", "open"}});
        std::optional<std::string> diff =
            tier2::replay(one_step(smoke_setup(), plain_step(action, args, Json::dict({{"type", "other"}}))));
        t.check(!diff.has_value(), std::string(action) + " of an unhashable sid raises: " + shown(diff));
    }
}

// Malformed recorded data is a difference, never an abort.
SCFG_TEST(Tier2Tests, MalformedVectorsAreDifferences) {
    auto with = [](const std::function<void(Json&)>& change) {
        Json v = one_step(smoke_setup(), plain_step("discard_session", Json::dict({{"sid", "x"}})));
        change(v);
        return tier2::replay(v);
    };
    std::optional<std::string> clean = with([](Json&) {});
    t.check(!clean.has_value(), "the probe itself fails: " + shown(clean));
    t.check(contains(with([](Json& v) { v.as_dict().set("tier", 1); }), "not a Tier 2 vector"), "tier");
    t.check(contains(with([](Json& v) { v.as_dict().set("steps", "x"); }), "steps is not a list"), "steps");
    t.check(contains(with([](Json& v) { field(v, "setup").as_dict().set("clock", "now"); }), "setup.clock"), "clock");
    t.check(contains(with([](Json& v) { field(v, "setup").as_dict().set("pending", Json::list()); }), "setup:"), "pending");
    t.check(contains(with([](Json& v) { field(v, "setup").as_dict().erase("items"); }), "setup: KeyError"), "items");
    t.check(contains(with([](Json& v) { step(v, 0) = Json(3); }), "step 0: not an object"), "step");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().erase("calls"); }), "nothing recorded for calls"), "no calls");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().set("at", "later"); }), "at is not an int"), "at");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().set("args", Json::list()); }), "args: AttributeError"), "args");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().set("calls", 5); }), "calls is not a list"), "calls");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().set("entropy", Json::list({5})); }), "unconsumed entropy"),
            "entropy");
    t.check(contains(with([](Json& v) { step(v, 0).as_dict().set("calls", Json::list({Json::dict({{"url", "u"}})})); }),
                     "a recorded call is malformed"),
            "a call");
    t.check(contains(with([](Json& v) {
                         step(v, 0).as_dict().set("args", Json::dict({{"sid", Json::dict({{"float", "nope"}})}}));
                     }),
                     "args: ValueError"),
            "float");
}

// ---- the transport and the entropy ----

SCFG_TEST(Tier2Tests, IdenticalCallsTakeTheirCopiesInRecordedOrder) {
    auto call = [](const char* result) {
        return Json::dict({{"url", "u"}, {"method", "m"}, {"params", Json::list({Json::dict({{"a", 1}, {"b", true}})})},
                           {"response_text", std::string("{\"result\": \"") + result + "\"}"}});
    };
    tier2::ReplayTransport transport;
    transport.load(Json::list({call("first"), call("second")}));
    // key order is ignored, 1 equals 1.0, and the JSON-RPC id is not compared
    Json payload = Json::dict({{"id", 9}, {"method", "m"}, {"params", Json::list({Json::dict({{"b", true}, {"a", 1.0}})})}});
    Result<Json> a = answer(transport.post_json("u", payload, 1));
    Result<Json> b = answer(transport.post_json("u", payload, 1));
    t.check(a.ok() && scfg_test::json_eq(a.value(), Json::dict({{"result", "first"}})), "the first copy");
    t.check(b.ok() && scfg_test::json_eq(b.value(), Json::dict({{"result", "second"}})), "the second copy");
    t.check(transport.leftover().empty() && transport.problems.empty(), "every call used, no problem");
    // true is not 1
    transport.load(Json::list({call("x")}));
    Json wrong = Json::dict({{"method", "m"}, {"params", Json::list({Json::dict({{"a", 1}, {"b", 1}})})}});
    Result<Json> w = answer(transport.post_json("u", wrong, 1));
    t.check(!w.ok() && w.err().kind == "AssertionError", "an unrecorded call is answered with an AssertionError");
    SCFG_CHECK_EQ(t, transport.problems.size(), 1);
    SCFG_CHECK_EQ(t, transport.leftover().size(), 1);
}

SCFG_TEST(Tier2Tests, ARecordedTransportErrorIsTheTransportsFailure) {
    tier2::ReplayTransport transport;
    transport.load(Json::list({Json::dict({{"url", "u"}, {"method", "m"}, {"params", Json::list()},
                                           {"transport_error", Json::dict({{"message", "HTTP 503"}, {"status", 503}})}})}));
    Result<Json> r = answer(transport.post_json("u", Json::dict({{"method", "m"}, {"params", Json::list()}}), 1));
    t.check(!r.ok() && r.err().kind == "TransportError" && r.err().message == "HTTP 503" && r.err().status == 503,
            r.ok() ? "answered" : r.err().str());
    t.check(transport.problems.empty() && transport.leftover().empty(), "problems");
}

SCFG_TEST(Tier2Tests, ScriptedEntropyKeepsTheContract) {
    tier2::ScriptedEntropy e;
    e.load(Json::list({Json::dict({{"fn", "token_bytes"}, {"n", 32}, {"value", "00ff"}}),
                       Json::dict({{"fn", "token_hex"}, {"n", 8}, {"value", "ea4702a60e526f21"}}),
                       Json::dict({{"fn", "randbelow"}, {"n", 100}, {"value", 6}})}));
    SCFG_CHECK_EQ(t, e.token_bytes(32).size(), 32);  // a short recorded value: still exactly n bytes, and a problem
    t.check(e.problems.size() == 1 && e.problems[0].find("token_bytes(32)") != std::string::npos, "a short value is a problem");
    SCFG_CHECK_EQ(t, e.token_hex(8), "ea4702a60e526f21");
    Result<int64_t> r = e.randbelow(100);
    t.check(r.ok() && r.value() == 6, "randbelow");
    SCFG_CHECK_EQ(t, e.token_bytes(32).size(), 32);  // exhausted
    t.check(e.problems.size() == 2 && e.problems[1] == "entropy exhausted at token_bytes(32)", "exhausted");
    e.load(Json::list({Json::dict({{"fn", "token_hex"}, {"n", 8}, {"value", "00"}})}));
    t.check(e.problems.empty(), "load resets the problems");
    t.check(!e.randbelow(100).ok(), "a mismatched draw is an Err");
    t.check(e.problems.size() == 1 && e.problems[0] == "entropy mismatch: drew randbelow(100), recorded token_hex(8)",
            e.problems.empty() ? "" : e.problems[0]);
    t.check(e.leftover().empty(), "the mismatched draw is consumed");
}

// Deferred mode: every request waits in the queue until the runner pumps it, one per turn.
SCFG_TEST(Tier2Tests, DeferredModeAnswersOnlyFromThePump) {
    std::optional<Json> v = load_vector("smoke/evm_unlock_paid");
    if (!t.check(v.has_value(), "unreadable")) return;
    tier2::ReplayTransport transport(tier2::ReplayMode::Deferred);
    size_t waiting_at_post = 0;
    transport.on_call = [&transport, &waiting_at_post](const std::string&, const Json&) {
        waiting_at_post += transport.queued();  // the previous request has been pumped before the next is made
    };
    std::optional<std::string> diff = tier2::replay(*v, transport);
    t.check(!diff.has_value(), shown(diff));
    size_t recorded = 0;
    for (const Json& s : v->as_dict().get("steps").as_list()) recorded += s.as_dict().get("calls").as_list().size();
    SCFG_CHECK_EQ(t, transport.posts(), recorded);
    SCFG_CHECK_EQ(t, transport.queued(), 0);
    SCFG_CHECK_EQ(t, waiting_at_post, 0);
}

// ---- restore candidates ----

SCFG_TEST(Tier2Tests, RecordedRestoreCandidatesRoundTrip) {
    size_t seen = 0;
    for (const std::string& id : scfg_test::vectors::tier2_ids()) {
        std::optional<Json> v = load_vector(id);
        if (!t.check(v.has_value(), id + " unreadable")) continue;
        for (const Json& s : v->as_dict().get("steps").as_list()) {
            if (s.as_dict().get("action").as_str() != "restore_lookup" || !s.as_dict().get("returns").is_list()) continue;
            for (const Json& c : s.as_dict().get("returns").as_list()) {
                Result<scfg::restore::RestoreCandidate> back = tier2::candidate_from_json(c);
                if (!t.check(back.ok(), id + ": " + (back.ok() ? "" : back.err().str()))) continue;
                Result<Json> again = tier2::candidate_to_json(back.value());
                t.check(again.ok() && scfg_test::json_eq(c, again.value()), id + ": the candidate does not round-trip");
                ++seen;
            }
        }
    }
    t.check(seen > 0, "no recorded candidate");
}

SCFG_TEST(Tier2Tests, CandidateFromJsonRefusesTheWrongClassOrFields) {
    Json transfer = Json::dict({{"token", "t"}, {"sender", "s"}, {"recipient", "r"}, {"value", 5}, {"tx_hash", "h"},
                                {"log_index", 0}, {"block_number", 1}, {"class", "Transfer"}});
    auto candidate = [](Json tr) {
        return Json::dict({{"chain", "base"}, {"token", "USDC"}, {"transfer", std::move(tr)}, {"kind", Json()},
                           {"item", Json()}, {"choices", Json::list({"tip"})}, {"already_restored", false}});
    };
    Result<scfg::restore::RestoreCandidate> ok_one = tier2::candidate_from_json(candidate(transfer));
    t.check(ok_one.ok() && std::holds_alternative<scfg::transfers::Transfer>(ok_one.value().transfer), "a Transfer");
    Json wrong_class = scfg::deep_copy(transfer);
    wrong_class.as_dict().set("class", "Nope");
    Result<scfg::restore::RestoreCandidate> a = tier2::candidate_from_json(candidate(wrong_class));
    t.check(!a.ok() && a.err().kind == "KeyError", "an unknown class is a KeyError");
    Json log = scfg::deep_copy(transfer);
    log.as_dict().set("class", "TransferLog");  // without block_hash
    Result<scfg::restore::RestoreCandidate> b = tier2::candidate_from_json(candidate(log));
    t.check(!b.ok() && b.err().kind == "TypeError", "missing fields are a TypeError");
    Result<scfg::restore::RestoreCandidate> c = tier2::candidate_from_json(Json::list());
    t.check(!c.ok() && c.err().kind == "TypeError", "a list is the TypeError of d[\"transfer\"]");
}

SCFG_TEST(Tier2Tests, EncodersKeepTheVectorRules) {
    Result<Json> big = tier2::to_jsonable(Json::dict({{"a", Json::list({Json(int64_t{1} << 53)})}}));
    t.check(!big.ok() && big.err().message.find("at a[0] is not safe in JSON") != std::string::npos, "2**53 is refused");
    Result<Json> half = tier2::to_jsonable(Json(1.5));
    t.check(!half.ok() && half.err().message.find("non-integral float 1.5") != std::string::npos, "1.5 is refused");
    Result<Json> whole = tier2::to_jsonable(Json(3.0));
    t.check(whole.ok() && whole.value().is_int() && whole.value().as_int64() == 3, "3.0 is written as 3");
}
