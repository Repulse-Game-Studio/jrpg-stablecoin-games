// No Python twin: Engine behaviour the Tier 2 vectors do not reach (their runner always passes its own RPC factory
// and error handler), ported from unity/core-tests/EngineTests.cs, plus what the C++ port adds: cancellation and
// teardown (a suspended tick destroyed, a transport that drops its requests), stored values carried as Python carries
// them, the backends registry, and print_error's sink.
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"

#include "Scfg/Backends.h"
#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/Watcher.h"

#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
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
