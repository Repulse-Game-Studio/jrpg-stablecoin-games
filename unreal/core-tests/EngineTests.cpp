// No Python twin: Engine behaviour the Tier 2 vectors do not reach (their runner always passes its own RPC factory
// and error handler), ported from unity/core-tests/EngineTests.cs, plus what the C++ port adds: cancellation and
// teardown (a suspended tick destroyed, a transport that drops its requests), stored values carried as Python carries
// them, the backends registry, and print_error's sink.
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Backends.h"
#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/Watcher.h"

#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using scfg::Err;
using scfg::Future;
using scfg::Json;
using scfg::Promise;
using scfg::Result;
namespace engine = scfg::engine;
namespace config = scfg::config;

namespace scfg_test::engine_tests {

constexpr const char* kWallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
constexpr const char* kBaseRpc = "https://base-sepolia-rpc.publicnode.com";

config::Config base_config() {
    Result<config::Config> c = config::build_config(Json::dict({{"wallet", kWallet}, {"chains", Json::list({"base"})},
                                                                {"testnet", true}}));
    config::Config out = c.take();
    Result<scfg::models::Item> item = config::add_item(
        out, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
    (void)item.take();
    return out;
}

Json draws() {
    return Json::list({Json::dict({{"fn", "randbelow"}, {"n", 100}, {"value", 6}}),
                       Json::dict({{"fn", "token_hex"}, {"n", 8}, {"value", "ea4702a60e526f21"}})});
}

// Answers eth_blockNumber with 0x3e8 at once and records every request.
struct HeadTransport final : scfg::coio::Transport {
    std::vector<std::string> methods;
    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)timeout;
        methods.push_back(payload.as_dict().get("method").as_str());
        return Future<Json>::completed(Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"result", "0x3e8"}}));
    }
};

// Keeps every request waiting until the test completes or drops it.
struct HeldTransport final : scfg::coio::Transport {
    std::deque<Promise<Json>> held;
    size_t posts = 0;
    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)payload, (void)timeout;
        ++posts;
        held.emplace_back();
        return held.back().get_future();
    }
};

Json awaiting(std::string chain, int64_t created_at) {
    return Json::dict({{"id", "ps_a"}, {"kind", "unlock"}, {"item", "chapter2"}, {"title", "Chapter 2"}, {"token", "USDC"},
                       {"chain", Json(std::move(chain))}, {"price", "4.99"}, {"code", 37}, {"nonce", 6}, {"units", 4993706},
                       {"start_block", 995}, {"cursor", 995}, {"created_at", created_at}, {"state", "awaiting"},
                       {"ui_status", "open"}, {"hint_tx", Json()}, {"hint_at", Json()}, {"match", Json()}});
}

template <class T>
std::optional<Result<T>> run_now(scfg::Co<T> co) {
    if (!co.done()) return std::nullopt;
    return co.take();
}

}  // namespace scfg_test::engine_tests

using namespace scfg_test::engine_tests;

SCFG_TEST(EngineTests, TheDefaultRpcFactoryUsesTheEnginesTransportAndKeepsOneClientPerChain) {
    HeadTransport transport;
    scfg_test::tier2::ScriptedEntropy entropy;
    entropy.load(draws());
    engine::Engine eng(base_config(), transport, Json(), Json(), [] { return int64_t{1700000000}; }, {}, {}, &entropy);
    std::optional<Result<Json>> rec = run_now(eng.start_session("unlock", "base", Json::dict({{"item", "chapter2"}})));
    if (!t.check(rec && rec->ok(), rec && !rec->ok() ? rec->err().str() : "suspended")) return;
    const Json::Dict& r = rec->value().as_dict();
    t.check(r.get("start_block").is_int64() && r.get("start_block").as_int64() == 995, "start_block");
    t.check(r.get("created_at").is_int64() && r.get("created_at").as_int64() == 1700000000, "created_at");
    SCFG_CHECK_EQ(t, r.get("id").as_str(), "ps_ea4702a60e526f21");
    t.check(transport.methods == std::vector<std::string>{"eth_blockNumber"}, "one eth_blockNumber");
    Result<scfg::backends::base::Rpc> a = eng.rpc("base");
    Result<scfg::backends::base::Rpc> b = eng.rpc("base");
    t.check(a.ok() && b.ok() && a.value() == b.value(), "one client per chain");
    t.check(a.ok() && scfg::backends::base::evm_client(a.value()).ok(), "an EVM chain gets an RpcClient");
    t.check(entropy.problems.empty() && entropy.leftover().empty(), "every draw used");
    // the client lists the developer's urls first: none here, the registry's after
    if (a.ok()) SCFG_CHECK_EQ(t, scfg::backends::base::client_of(a.value()).urls()[0], kBaseRpc);
}

SCFG_TEST(EngineTests, StateIsCopiedInAndOut) {
    HeadTransport transport;
    scfg_test::tier2::ScriptedEntropy entropy;
    entropy.load(draws());
    Json pending = Json::dict();
    engine::Engine eng(base_config(), transport, pending, Json(), [] { return int64_t{1700000000}; }, {}, {}, &entropy);
    std::optional<Result<Json>> rec = run_now(eng.start_session("unlock", "base", Json::dict({{"item", "chapter2"}})));
    if (!t.check(rec && rec->ok(), "start_session")) return;
    SCFG_CHECK_EQ(t, pending.as_dict().size(), 0);  // the caller's store is not the Engine's
    rec->value().as_dict().set("state", "changed");
    Result<engine::Snapshot> snap = eng.snapshot();
    if (!t.check(snap.ok(), "snapshot")) return;
    Json stored = snap.value().pending.as_dict().get("ps_ea4702a60e526f21");
    SCFG_CHECK_EQ(t, stored.as_dict().get("state").as_str(), "awaiting");
    stored.as_dict().set("state", "changed");
    SCFG_CHECK_EQ(t, eng.pending().as_dict().get("ps_ea4702a60e526f21").as_dict().get("state").as_str(), "awaiting");
}

SCFG_TEST(EngineTests, AnUnknownKeywordIsATypeError) {
    HeadTransport transport;
    engine::Engine eng(base_config(), transport, Json(), Json(), [] { return int64_t{0}; });
    std::optional<Result<Json>> r = run_now(eng.start_session("unlock", "base", Json::dict({{"colour", "red"}})));
    t.check(r && !r->ok() && r->err().kind == "TypeError", "unknown keyword");
    SCFG_CHECK(t, transport.methods.empty());
    // a chain that does not carry the token is the ConfigError, whatever the chain value is
    r = run_now(eng.start_session("unlock", Json::list({"base"}), Json::dict({{"item", "chapter2"}})));
    t.check(r && !r->ok() && r->err().code == "chain.token_unavailable", "a list chain");
}

SCFG_TEST(EngineTests, ACrashIsContainedAndCounted) {
    HeadTransport transport;
    Json pending = Json::dict({{"ps_bad", Json::dict({{"chain", "base"}, {"state", "awaiting"}, {"ui_status", "cancelled"}})}});
    std::vector<std::string> errors;
    engine::Engine eng(base_config(), transport, pending, Json(), [] { return int64_t{0}; }, {},
                       [&errors](const Err& e) { errors.push_back(e.kind); });
    std::optional<Result<std::vector<engine::StepOutcome>>> got = run_now(eng.tick());  // created_at is missing
    if (!t.check(got && got->ok(), "tick")) return;
    const std::vector<engine::StepOutcome>& outcomes = got->value();
    if (!t.check(outcomes.size() == 1, "one outcome")) return;
    SCFG_CHECK_EQ(t, outcomes[0].status, "waiting");
    t.check(outcomes[0].error == std::optional<std::string>("internal_error"), "internal_error");
    t.check(errors == std::vector<std::string>{"KeyError"}, "one KeyError reported");
    // an unset handler prints instead (Python's `on_error or print_error`), and the step is still contained
    eng.on_error = {};
    std::vector<std::string> printed;
    scfg::errors::PrintSink previous =
        scfg::errors::set_print_sink([&printed](std::string_view line) { printed.emplace_back(line); });
    got = run_now(eng.tick());
    scfg::errors::set_print_sink(std::move(previous));
    t.check(got && got->ok() && got->value().size() == 1, "contained with no handler");
    t.check(printed == std::vector<std::string>{"scfg: contained error: KeyError: 'created_at'"},
            "printed: " + (printed.empty() ? std::string("nothing") : printed[0]));
}

// Python's except Exception does not catch a CancelledError: a dropped request leaves tick() at once, without
// stepping the next session, reporting anything or touching the network again.
SCFG_TEST(EngineTests, ADroppedRequestLeavesTickAtOnce) {
    HeldTransport transport;
    Json pending = Json::dict({{"ps_a", awaiting("base", 1700000000)}, {"ps_b", awaiting("base", 1700000000)}});
    pending.as_dict().find("ps_b")->as_dict().set("id", "ps_b");
    int reported = 0;
    engine::Engine eng(base_config(), transport, pending, Json(), [] { return int64_t{1700000100}; }, {},
                       [&reported](const Err&) { ++reported; });
    std::optional<Result<std::vector<engine::StepOutcome>>> got;
    {
        scfg::Job<std::vector<engine::StepOutcome>> job;
        job.start(eng.tick(), [&got](Result<std::vector<engine::StepOutcome>> r) { got.emplace(std::move(r)); });
        t.check(!job.done() && transport.held.size() == 1, "the first session waits for eth_blockNumber");
        std::deque<Promise<Json>> dropped = std::move(transport.held);  // a transport shutting down
        transport.held.clear();
        dropped.clear();
        t.check(job.done(), "the tick finished");
    }
    t.check(got && !got->ok() && got->err().kind == "CancelledError", "CancelledError leaves tick()");
    SCFG_CHECK_EQ(t, reported, 0);
    SCFG_CHECK_EQ(t, transport.posts, 1);
    Result<engine::Snapshot> snap = eng.snapshot();
    t.check(snap.ok() && snap.value().pending.as_dict().size() == 2, "both sessions are kept");
}

// Destroying a suspended tick (its Job) only runs destructors; a late answer resumes nothing.
SCFG_TEST(EngineTests, DestroyingASuspendedTickIsSafe) {
    HeldTransport transport;
    Json pending = Json::dict({{"ps_a", awaiting("base", 1700000000)}});
    engine::Engine eng(base_config(), transport, pending, Json(), [] { return int64_t{1700000100}; });
    bool finished = false;
    {
        scfg::Job<std::vector<engine::StepOutcome>> job;
        job.start(eng.tick(), [&finished](Result<std::vector<engine::StepOutcome>>) { finished = true; });
        t.check(!job.done(), "suspended");
    }
    transport.held.front().complete(Json::dict({{"result", "0x3e8"}}));
    t.check(!finished, "a cancelled tick never reports");
    // the Engine still works
    Result<engine::Snapshot> snap = eng.snapshot();
    t.check(snap.ok() && snap.value().pending.as_dict().size() == 1, "state intact");
}

// Teardown in the other order: the Engine destroyed while a command's Job is suspended, then the Job. The frame's
// busy guard shares its flag with the Engine (N1), so destroying the frame afterwards touches no freed memory (the
// asan preset proves it); the transport, still alive, then answers into nothing.
SCFG_TEST(EngineTests, DestroyingTheEngineBeforeASuspendedJobIsSafe) {
    using Lookup = std::vector<scfg::restore::RestoreCandidate>;
    const std::string tx = "0x" + std::string(64, 'a');
    for (int command = 0; command < 3; ++command) {
        HeldTransport transport;
        bool finished = false;
        std::optional<scfg::Job<std::vector<engine::StepOutcome>>> tick(std::in_place);
        std::optional<scfg::Job<Json>> session(std::in_place);
        std::optional<scfg::Job<Lookup>> lookup(std::in_place);
        {
            auto eng = std::make_unique<engine::Engine>(base_config(), transport,
                                                        Json::dict({{"ps_a", awaiting("base", 1700000000)}}), Json(),
                                                        [] { return int64_t{1700000100}; });
            if (command == 0) {
                tick->start(eng->tick(), [&finished](Result<std::vector<engine::StepOutcome>>) { finished = true; });
            } else if (command == 1) {
                session->start(eng->start_session("unlock", "base", Json::dict({{"item", "chapter2"}})),
                               [&finished](Result<Json>) { finished = true; });
            } else {
                lookup->start(eng->restore_lookup(tx), [&finished](Result<Lookup>) { finished = true; });
            }
            if (!t.check(eng->running() && transport.held.size() == 1,
                         "command " + std::to_string(command) + " suspended")) {
                return;
            }
        }  // the Engine goes first
        tick.reset();
        session.reset();
        lookup.reset();
        transport.held.front().complete(Json::dict({{"result", "0x3e8"}}));
        t.check(!finished, "a destroyed command never reports");
    }
}

// One command at a time: running() is set while a network command's coroutine is alive, released when it returns
// (before its Job's on_done, which may start the next command) or when its frame is destroyed. Starting a command
// while it is set aborts (programming_error); that path is not run here, as the runner has no death tests.
SCFG_TEST(EngineTests, ACommandHoldsTheEngineWhileItRuns) {
    HeldTransport transport;
    Json pending = Json::dict({{"ps_a", awaiting("base", 1700000000)}});
    engine::Engine eng(base_config(), transport, pending, Json(), [] { return int64_t{1700000100}; });
    t.check(!eng.running(), "idle at first");
    std::optional<bool> running_in_on_done;
    scfg::Status next = Err::value_error("not run");
    {
        scfg::Job<std::vector<engine::StepOutcome>> job;
        job.start(eng.tick(), [&](Result<std::vector<engine::StepOutcome>>) {
            running_in_on_done = eng.running();
            next = eng.set_ui_status("ps_a", "pending");  // the next command, from on_done
        });
        t.check(!job.done() && eng.running(), "a suspended tick holds the Engine");
        std::deque<Promise<Json>> dropped = std::move(transport.held);  // CancelledError ends the tick
        transport.held.clear();
        dropped.clear();
        t.check(job.done(), "the tick finished");
    }
    t.check(running_in_on_done == std::optional<bool>(false), "released before on_done");
    t.check(next.ok(), "on_done started the next command");
    t.check(!eng.running(), "idle after the tick");
    {
        scfg::Co<Json> verify = eng.reverify();  // no receipts: check_ready asks nothing of an EVM chain
        t.check(verify.done() && !eng.running(), "a command that never waits releases at once");
    }
    {
        scfg::Co<std::vector<engine::StepOutcome>> held = eng.tick();
        t.check(!held.done() && eng.running(), "suspended again");
    }  // destroyed while suspended
    t.check(!eng.running(), "destroying a suspended command releases the Engine");
    transport.held.clear();
}

// A stored session whose fields are not strs is carried as Python carries it: its expiry still reports, with the
// stored values, and the tick's other outcomes are not lost.
SCFG_TEST(EngineTests, StoredValuesAreCarriedIntoTheExpiry) {
    HeadTransport transport;
    Json rec = Json::dict({{"chain", 7}, {"kind", 5}, {"item", Json::list({1})}, {"token", Json()},
                           {"created_at", 1700000000}, {"ui_status", "open"}, {"state", "awaiting"}});
    engine::Engine eng(base_config(), transport, Json::dict({{"ps_odd", rec}}), Json(), [] { return int64_t{1700003600}; });
    std::optional<Result<std::vector<engine::StepOutcome>>> got = run_now(eng.tick());
    if (!t.check(got && got->ok(), got && !got->ok() ? got->err().str() : "suspended")) return;
    if (!t.check(got->value().size() == 1, "one outcome")) return;
    const engine::StepOutcome& o = got->value()[0];
    SCFG_CHECK_EQ(t, o.status, "expired");  // chain 7 is not watchable: kept until its horizon, but its window closed
    if (!t.check(o.result.has_value(), "a result")) return;
    t.check(scfg_test::json_eq(o.result->kind, Json(5)) && scfg_test::json_eq(o.result->item, Json::list({1})) &&
                o.result->token.is_null() && scfg_test::json_eq(o.result->chain, Json(7)),
            "stored values carried");
    SCFG_CHECK(t, transport.methods.empty());
}

// The watcher's arithmetic follows Python's: a float cursor reaches eth_getLogs' hex() (a TypeError, contained), a
// NaN cursor skips the scan and is reset to head + 1.
SCFG_TEST(EngineTests, AFloatCursorFailsWherePythonFails) {
    struct LogsTransport final : scfg::coio::Transport {
        std::vector<std::string> methods;
        Future<Json> post_json(std::string url, Json payload, double timeout) override {
            (void)url, (void)timeout;
            std::string m = payload.as_dict().get("method").as_str();
            methods.push_back(m);
            Json result = m == "eth_blockNumber" ? Json("0x3e8") : Json::list();
            return Future<Json>::completed(Json::dict({{"result", result}}));
        }
    };
    for (double cursor : {995.0, std::nan("")}) {
        LogsTransport transport;
        Json rec = awaiting("base", 1700000000);
        rec.as_dict().set("cursor", cursor);
        std::vector<std::string> errors;
        engine::Engine eng(base_config(), transport, Json::dict({{"ps_a", rec}}), Json(), [] { return int64_t{1700000100}; },
                           {}, [&errors](const Err& e) { errors.push_back(e.kind); });
        std::optional<Result<std::vector<engine::StepOutcome>>> got = run_now(eng.tick());
        if (!t.check(got && got->ok() && got->value().size() == 1, "tick")) continue;
        t.check(transport.methods == std::vector<std::string>{"eth_blockNumber"}, "no eth_getLogs");
        Json stored = eng.pending().as_dict().get("ps_a").as_dict().get("cursor");
        if (!std::isnan(cursor)) {
            t.check(errors == std::vector<std::string>{"TypeError"}, "a float cursor is hex()'s TypeError");
            t.check(stored.is_float(), "the cursor is untouched");
        } else {
            t.check(errors.empty(), "a NaN cursor is no error");
            t.check(stored.is_int64() && stored.as_int64() == 1001, "the cursor is head + 1");
            SCFG_CHECK_EQ(t, got->value()[0].status, "waiting");
        }
    }
}

SCFG_TEST(EngineTests, TheBackendsRegistry) {
    Result<const scfg::backends::base::Backend*> evm = scfg::backends::backend_for_family("evm");
    Result<const scfg::backends::base::Backend*> sol = scfg::backends::backend_for_family("solana");
    Result<const scfg::backends::base::Backend*> none = scfg::backends::backend_for_family("bitcoin");
    t.check(evm.ok() && evm.value()->family() == "evm", "evm");
    t.check(sol.ok() && sol.value()->family() == "solana", "solana");
    t.check(!none.ok() && none.err().kind == "ValueError" && none.err().message == "unknown chain family 'bitcoin'",
            "an unknown family");
    HeadTransport transport;
    scfg::chains::Chain chain;
    chain.key = "solana";
    chain.family = "solana";
    chain.rpc = {"https://solana.example"};
    chain.tokens.set("USDC", scfg::chains::Token::make("4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU").take());
    Result<scfg::backends::base::Rpc> rpc = sol.value()->make_rpc(chain, transport);
    t.check(rpc.ok() && scfg::backends::base::solana_client(rpc.value()).ok(), "a SolanaRpc");
    if (rpc.ok()) SCFG_CHECK_EQ(t, scfg::backends::base::client_of(rpc.value()).urls()[0], "https://solana.example");
    scfg_test::tier2::ScriptedEntropy entropy;
    std::optional<Result<Json>> started =
        run_now(sol.value()->start(rpc.value(), chain, std::nullopt, "USDC", std::ref<scfg::Entropy>(entropy)));
    // no wallet: associated_token_address(None, mint) is decode_fixed's ValueError, before any request
    t.check(started && !started->ok() && started->err().kind == "ValueError" && started->err().message == "not base58: None",
            "solana start without a wallet");
    t.check(transport.methods.empty(), "no request");
    t.check(rpc.ok() && !scfg::backends::base::evm_client(rpc.value()).ok(), "a SolanaRpc is no RpcClient");
    chain.rpc.clear();
    Result<scfg::backends::base::Rpc> no_urls = evm.value()->make_rpc(chain, transport);
    t.check(!no_urls.ok() && no_urls.err().kind == "ValueError", "no urls");
}

// ---- a node's log index beyond str()'s 4300-digit limit ----

namespace scfg_test::engine_tests {

std::optional<Json> load_vector(const std::string& id) {
    std::optional<std::string> text = vectors::read_text("tier2/" + id + ".json");
    if (!text) return std::nullopt;
    Result<Json> v = scfg_test::tier2::parse(*text);
    if (!v.ok()) return std::nullopt;
    return v.take();
}

// Every log of every `method` answer in calls gets the logIndex "0x" + "f" * 4000 (16000 bits, about 4817 decimal
// digits): parse_transfer_log reads it (int(x, 16) is unbounded), and receipt_key's str() of it is the ValueError.
void huge_log_index(Json& calls, std::string_view method) {
    const std::string huge = "0x" + std::string(4000, 'f');
    for (Json& c : calls.as_list()) {
        Json::Dict& call = c.as_dict();
        if (call.get("method").as_str() != method) continue;
        Json body = scfg::json::loads(call.get("response_text").as_str()).take();
        Json& result = *body.as_dict().find("result");
        if (result.is_null()) continue;
        Json& logs = result.is_dict() ? *result.as_dict().find("logs") : result;
        for (Json& lg : logs.as_list()) lg.as_dict().set("logIndex", Json(huge));
        call.set("response_text", Json(scfg::json::dumps(body).value()));
    }
}

}  // namespace scfg_test::engine_tests

// Expectations from the Python reference:
//
// ---- huge_index.py (run from the repo root: python3 -I huge_index.py .) ----
//   import copy, json, sys
//   sys.path[:0] = [sys.argv[1] + "/plugin/game/python-packages", sys.argv[1] + "/conformance/python"]
//   from scfg_conformance import replay as rp
//   HUGE = "0x" + "f" * 4000
//
//   def huge(calls, method):
//       for c in calls:
//           if c["method"] == method:
//               body = json.loads(c["response_text"])
//               r = body["result"]
//               for lg in (r["logs"] if method == "eth_getTransactionReceipt" and r else (r or [])):
//                   lg["logIndex"] = HUGE
//               c["response_text"] = json.dumps(body)
//
//   for vid, upto, method in [("restore/unlock_found", 1, "eth_getTransactionReceipt"),
//                             ("smoke/evm_unlock_paid", 2, "eth_getLogs")]:
//       v = json.load(open(f"{sys.argv[1]}/conformance/vectors/tier2/{vid}.json"))
//       steps = copy.deepcopy(v["steps"][:upto])
//       huge(steps[-1]["calls"], method)
//       t = rp.ReplayTransport()
//       errors = []
//       now = [0]
//       engine = rp.build_engine(v["setup"], transport=t, clock=lambda: now[0], on_error=errors.append)
//       for i, s in enumerate(steps):
//           now[0] = s["at"]
//           t.load(s["calls"])
//           with rp.patched_entropy(rp.ScriptedEntropy(s["entropy"])):
//               result, raises = rp.run_action(engine, s["action"], s["args"], steps[:i])
//       print(vid, raises, None if raises else rp.encode_returns(s["action"], result), errors,
//             [c["method"] for c, u in zip(t._calls, t._used) if not u],
//             rp.snapshot_json(engine) == v["steps"][upto - 1]["snapshot"],
//             upto > 1 and rp.snapshot_json(engine) == v["steps"][upto - 2]["snapshot"])
// ---- end of huge_index.py ----
//
// Python printed: restore/unlock_found raises {"type": "other"} (the ValueError "Exceeds the limit (4300 digits)
// for integer string conversion", Python 3.12's wording), every call made, the snapshot as recorded; the
// smoke/evm_unlock_paid tick returns the session "waiting" with error "internal_error" (one ValueError contained),
// eth_getBlockByNumber never asked, and the snapshot is the previous step's (the cursor is not moved).
SCFG_TEST(EngineTests, AHugeLogIndexFailsTheRestoreLookup) {
    std::optional<Json> v = load_vector("restore/unlock_found");
    if (!t.check(v.has_value(), "restore/unlock_found")) return;
    Json copy = scfg::deep_copy(*v);
    Json::List& steps = copy.as_dict().find("steps")->as_list();
    steps.resize(1);
    Json::Dict& step = steps[0].as_dict();
    huge_log_index(*step.find("calls"), "eth_getTransactionReceipt");
    step.set("raises", Json::dict({{"type", "other"}}));
    step.set("returns", Json());
    std::optional<std::string> diff = scfg_test::tier2::replay(copy);
    t.check(!diff.has_value(), diff.value_or(""));
    // and the raise is str()'s ValueError, not a port's own
    scfg_test::tier2::ReplayTransport transport;
    std::unique_ptr<engine::Engine> eng =
        scfg_test::tier2::build_engine(copy.as_dict().get("setup"), transport, [] { return int64_t{1700000000}; }, {},
                                       nullptr)
            .take();
    transport.load(step.get("calls"));
    scfg::Co<std::vector<scfg::restore::RestoreCandidate>> co = eng->restore_lookup(step.get("args").as_dict().get("tx"));
    if (!t.check(co.done(), "suspended")) return;
    Result<std::vector<scfg::restore::RestoreCandidate>> got = co.take();
    t.check(!got.ok() && got.err().kind == "ValueError" &&
                got.err().message.starts_with("Exceeds the limit (4300) for integer string conversion"),
            got.ok() ? "candidates" : got.err().str());
}

SCFG_TEST(EngineTests, AHugeLogIndexIsAContainedWatcherError) {
    std::optional<Json> v = load_vector("smoke/evm_unlock_paid");
    if (!t.check(v.has_value(), "smoke/evm_unlock_paid")) return;
    Json copy = scfg::deep_copy(*v);
    Json::List& steps = copy.as_dict().find("steps")->as_list();
    steps.resize(2);
    const Json before = steps[0].as_dict().get("snapshot");
    Json::Dict& step = steps[1].as_dict();
    Json::List& calls = step.find("calls")->as_list();
    Json::List kept;
    for (const Json& c : calls) {
        if (c.as_dict().get("method").as_str() != "eth_getBlockByNumber") kept.push_back(c);
    }
    calls = kept;
    huge_log_index(*step.find("calls"), "eth_getLogs");
    const std::string sid = step.get("returns").as_list()[0].as_dict().get("session_id").as_str();
    step.set("returns", Json::list({Json::dict({{"session_id", Json(sid)},
                                                {"status", "waiting"},
                                                {"confirmations", 0},
                                                {"required", 0},
                                                {"result", Json()},
                                                {"error", "internal_error"},
                                                {"hint_tx", Json()}})}));
    step.set("internal_errors", 1);
    step.set("snapshot", scfg::deep_copy(before));
    std::optional<std::string> diff = scfg_test::tier2::replay(copy);
    t.check(!diff.has_value(), diff.value_or(""));
}

// ---- a node's block number beyond str()'s 4300-digit limit ----

namespace scfg_test::engine_tests {

// Answers eth_blockNumber with hex(head) and eth_getLogs with [], recording every request.
struct HugeHeadTransport final : scfg::coio::Transport {
    scfg::BigInt head;
    std::vector<Json> asked;
    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)timeout;
        asked.push_back(payload);
        const bool block_number = payload.as_dict().get("method").as_str() == "eth_blockNumber";
        return Future<Json>::completed(Json::dict(
            {{"jsonrpc", "2.0"}, {"id", 1}, {"result", block_number ? Json(scfg::py::hex(head)) : Json::list()}}));
    }
};

}  // namespace scfg_test::engine_tests

// int(x, 16) has no digit limit, so a node's eth_blockNumber of 5000 decimal digits becomes the session's
// start_block and cursor, and every later request carries it in hex. Python raises only where it formats the int in
// decimal: json.dumps of the record (when the host saves it) and repr. Expectations from the Python reference:
//
// ---- huge_head.py (run from the repo root: python3 -I huge_head.py .) ----
//   import copy, json, sys
//   sys.path[:0] = [sys.argv[1] + "/plugin/game/python-packages", sys.argv[1] + "/conformance/python"]
//   from scfg_conformance import replay as rp
//   HEAD = 10 ** 4999 + 1000
//   v = json.load(open(f"{sys.argv[1]}/conformance/vectors/tier2/smoke/evm_unlock_paid.json"))
//   step = copy.deepcopy(v["steps"][0])
//   step["calls"][0]["response_text"] = json.dumps({"id": 1, "jsonrpc": "2.0", "result": hex(HEAD)})
//   t = rp.ReplayTransport()
//   errors = []
//   engine = rp.build_engine(v["setup"], transport=t, clock=lambda: step["at"], on_error=errors.append)
//   t.load(step["calls"])
//   with rp.patched_entropy(rp.ScriptedEntropy(step["entropy"])):
//       result, raises = rp.run_action(engine, step["action"], step["args"], [])
//   print(raises, errors, result["start_block"] - HEAD, result["cursor"] - HEAD)
//   for obj in (result, engine.pending):
//       try: json.dumps(obj)
//       except ValueError as e: print(e)
//   try: repr(result)
//   except ValueError as e: print(e)
//   class Answering:
//       def __init__(self): self.asked = []
//       def post_json(self, url, payload, timeout):
//           self.asked.append((payload["method"], payload["params"]))
//           result = hex(HEAD + 7) if payload["method"] == "eth_blockNumber" else []
//           return {"jsonrpc": "2.0", "id": 1, "result": result}
//   engine.transport = a = Answering()
//   result, raises = rp.run_action(engine, "tick", {}, [])
//   print(raises, rp.encode_returns("tick", result), errors, a.asked)
//   rec = engine.pending[result[0].session_id]
//   print(rec["start_block"] - HEAD, rec["cursor"] - HEAD)
// ---- end of huge_head.py ----
//
// Python 3.9.25 printed: no raise and no contained error, start_block and cursor HEAD - 5; json.dumps of the result
// and of engine.pending, and repr(result), each "Exceeds the limit (4300) for integer string conversion; use
// sys.set_int_max_str_digits() to increase the limit" (at once); the tick returns the session "waiting" (0 of 3
// confirmations, no error) after eth_blockNumber and one eth_getLogs from hex(HEAD - 5) to hex(HEAD + 7), and the
// cursor is then HEAD + 8.
SCFG_TEST(EngineTests, AHugeBlockNumberIsStoredAndRefusedOnlyByDumps) {
    std::optional<Json> v = load_vector("smoke/evm_unlock_paid");
    if (!t.check(v.has_value(), "smoke/evm_unlock_paid")) return;
    const Json first_step = v->as_dict().get("steps").as_list()[0];
    const Json::Dict& first = first_step.as_dict();
    const scfg::BigInt head = scfg::BigInt(10).pow(4999) + scfg::BigInt(1000);
    HugeHeadTransport transport;
    transport.head = head;
    std::vector<std::string> errors;
    scfg_test::tier2::ScriptedEntropy entropy;
    entropy.load(first.get("entropy"));
    const int64_t at = first.get("at").as_int64();
    Result<std::unique_ptr<engine::Engine>> built = scfg_test::tier2::build_engine(
        v->as_dict().get("setup"), transport, [at] { return at; }, [&errors](const Err& e) { errors.push_back(e.str()); },
        &entropy);
    if (!t.check(built.ok(), built.ok() ? "" : built.err().str())) return;
    engine::Engine& eng = *built.value();
    const Json args_json = first.get("args");
    const Json::Dict& args = args_json.as_dict();
    std::optional<Result<Json>> rec =
        run_now(eng.start_session(args.get("kind"), args.get("chain"), Json::dict({{"item", args.get("item")}})));
    if (!t.check(rec && rec->ok(), rec && !rec->ok() ? rec->err().str() : "suspended")) return;
    const Json record = rec->value();
    t.check(record.as_dict().get("start_block").as_bigint() == head - scfg::BigInt(5), "start_block is HEAD - 5");
    t.check(record.as_dict().get("cursor").as_bigint() == head - scfg::BigInt(5), "cursor is HEAD - 5");
    t.check(errors.empty(), "no contained error");
    const std::string limit = "ValueError: Exceeds the limit (4300) for integer string conversion; use "
                              "sys.set_int_max_str_digits() to increase the limit";
    auto outcome = [](const Result<std::string>& r) { return r.ok() ? std::string("<ok>") : r.err().str(); };
    SCFG_CHECK_EQ(t, outcome(scfg::json::dumps(record)), limit);
    SCFG_CHECK_EQ(t, outcome(scfg::json::dumps(eng.pending())), limit);
    SCFG_CHECK_EQ(t, outcome(scfg::py::repr(record)), limit);

    transport.head = head + scfg::BigInt(7);
    transport.asked.clear();
    std::optional<Result<std::vector<scfg::watcher::StepOutcome>>> ticked = run_now(eng.tick());
    if (!t.check(ticked && ticked->ok(), ticked && !ticked->ok() ? ticked->err().str() : "suspended")) return;
    const std::vector<scfg::watcher::StepOutcome>& outcomes = ticked->value();
    if (!t.check(outcomes.size() == 1, "one outcome")) return;
    SCFG_CHECK_EQ(t, outcomes[0].status, "waiting");
    t.check(outcomes[0].confirmations == 0 && outcomes[0].required == 3 && !outcomes[0].error, "0 of 3, no error");
    t.check(errors.empty(), "no contained error in the tick");
    if (t.check(transport.asked.size() == 2, "eth_blockNumber and one eth_getLogs")) {
        SCFG_CHECK_EQ(t, transport.asked[0].as_dict().get("method").as_str(), "eth_blockNumber");
        const Json::Dict& logs = transport.asked[1].as_dict();
        SCFG_CHECK_EQ(t, logs.get("method").as_str(), "eth_getLogs");
        const Json filter_json = logs.get("params").as_list()[0];
        const Json::Dict& filter = filter_json.as_dict();
        SCFG_CHECK_EQ(t, filter.get("fromBlock").as_str(), scfg::py::hex(head - scfg::BigInt(5)));
        SCFG_CHECK_EQ(t, filter.get("toBlock").as_str(), scfg::py::hex(head + scfg::BigInt(7)));
    }
    const Json stored = eng.pending().as_dict().get(record.as_dict().get("id").as_str());
    t.check(stored.as_dict().get("start_block").as_bigint() == head - scfg::BigInt(5), "start_block kept");
    t.check(stored.as_dict().get("cursor").as_bigint() == head + scfg::BigInt(8), "cursor is HEAD + 8");
}

// N1 at every suspension, not only the first: for every Tier 2 vector, every step that makes requests and every
// k below its request count, the Engine is destroyed while the step's action waits for request k + 1, then the
// action's Job. That covers every network command (start_session, build_solana_payment, tick, reverify,
// restore_lookup) at every depth the vectors reach; under the asan preset a destructor that touched the freed Engine
// fails here.
SCFG_TEST(EngineTests, DestroyingTheEngineAtEverySuspensionIsSafe) {
    std::vector<std::string> ids = scfg_test::vectors::tier2_ids();
    SCFG_CHECK(t, !ids.empty());
    size_t probes = 0;
    std::set<std::string> actions;
    for (const std::string& id : ids) {
        std::optional<Json> v = load_vector(id);
        if (!t.check(v.has_value(), id + " cannot be read")) continue;
        const Json recorded = v->as_dict().get("steps");
        const Json::List& steps = recorded.as_list();
        for (size_t i = 0; i < steps.size(); ++i) {
            const size_t calls = steps[i].as_dict().get("calls").as_list().size();
            for (size_t k = 0; k < calls; ++k) {
                std::optional<std::string> why = scfg_test::tier2::destroy_engine_while_suspended(*v, i, k);
                t.check(!why.has_value(), id + " step " + std::to_string(i) + " after " + std::to_string(k) +
                                              " answer(s): " + why.value_or(""));
                ++probes;
            }
            if (calls > 0) actions.insert(steps[i].as_dict().get("action").as_str());
        }
    }
    t.check(probes > 500, "only " + std::to_string(probes) + " suspensions probed");
    for (const char* a : {"start_session", "build_solana_payment", "tick", "reverify", "restore_lookup"}) {
        t.check(actions.count(a) == 1, std::string(a) + " was never probed");
    }
}
