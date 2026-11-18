// Copyright 2026 Repulse
// No Python twin: watcher::advance (the EVM step) over a real RpcClient and a fake transport, for what the Tier 2
// vectors pin only through the Engine: test_watcher_awaiting.py's
// test_hint_verdict_survives_a_network_failure_later_in_the_step (review V1; its vector is
// tier2/evm/hint_verdict_survives_a_network_failure), and the same verdict kept when the step ends with Abandoned (the
// tick yielding to a command, worker.py), which test_worker_priority.py reaches only through the worker.
#include "support/Test.h"

#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/Watcher.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Err;
using scfg::Future;
using scfg::Json;
using scfg::Result;
namespace watcher = scfg::watcher;

namespace scfg_test::watcher_tests {

constexpr const char* kDev = "0x52908400098527886E0F7030069857D2E4169EE7";
const std::string kTx = "0x" + std::string(64, 'a');
constexpr const char* kUrl = "https://arb1.arbitrum.io/rpc";

// How eth_blockNumber fails after the hint's receipt was read.
enum class Failure { Unavailable, RpcError, Abandoned };

// eth_getTransactionReceipt answers a reverted receipt (status 0x0); eth_blockNumber fails as `failure` says.
struct RevertedThenFailing final : scfg::coio::Transport {
    Failure failure = Failure::Unavailable;
    std::vector<std::string> methods;
    Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)timeout;
        const std::string method = payload.as_dict().get("method").as_str();
        methods.push_back(method);
        const Json id = payload.as_dict().get("id");
        if (method == "eth_getTransactionReceipt") {
            Json receipt = Json::dict({{"status", "0x0"}, {"blockNumber", "0x3e6"}, {"logs", Json::list()},
                                       {"transactionHash", kTx}});
            return Future<Json>::completed(Json::dict({{"jsonrpc", "2.0"}, {"id", id}, {"result", receipt}}));
        }
        switch (failure) {
            case Failure::Unavailable:
                return Future<Json>::completed(Err::transport_error("fake outage", std::nullopt));
            case Failure::RpcError:
                return Future<Json>::completed(Json::dict(
                    {{"jsonrpc", "2.0"}, {"id", id},
                     {"error", Json::dict({{"code", -32000}, {"message", "internal error"}})}}));
            case Failure::Abandoned:
                break;
        }
        return Future<Json>::completed(Err::make("Abandoned", "abandoned before its request to " + url));
    }
};

// test_watcher_awaiting.py's setup(): an awaiting unlock on arbitrum (nonce 42, start_block 995, created at 0).
Json awaiting(Json hint_tx) {
    return Json::dict({{"id", "ps_a"}, {"kind", "unlock"}, {"item", "chapter2"}, {"title", "Chapter 2"},
                       {"token", "USDC"}, {"chain", "arbitrum"}, {"price", "4.99"}, {"code", 37}, {"nonce", 42},
                       {"units", 4993742}, {"start_block", 995}, {"cursor", 995}, {"created_at", 0.0},
                       {"state", "awaiting"}, {"ui_status", "open"}, {"hint_tx", hint_tx},
                       {"hint_at", hint_tx.is_null() ? Json() : Json(0.0)}, {"match", Json()}});
}

scfg::OrderedMap<scfg::models::Item> items() {
    scfg::config::Config c =
        scfg::config::build_config(Json::dict({{"wallet", kDev}, {"chains", Json::list({"arbitrum"})}})).take();
    (void)scfg::config::add_item(
              c, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}))
        .take();
    return c.items;
}

// step(): advance() at now=10.0, run to its end (the fake answers at once).
std::optional<Result<watcher::StepOutcome>> step(Json pending, RevertedThenFailing& transport) {
    auto rpc = std::make_shared<scfg::rpc::RpcClient>(scfg::rpc::RpcClient::make({kUrl}, transport).take());
    const scfg::chains::Chain* chain = scfg::chains::MAINNET().find("arbitrum");
    if (!chain) return std::nullopt;
    scfg::Co<watcher::StepOutcome> co = watcher::advance(pending, Json::dict(), "ps_a", rpc, *chain, kDev, items(), Json(10.0));
    if (!co.done()) return std::nullopt;
    return co.take();
}

}  // namespace scfg_test::watcher_tests

using namespace scfg_test::watcher_tests;

// test_hint_verdict_survives_a_network_failure_later_in_the_step (review V1): the hint was judged (reverted) and
// cleared, then the scan's eth_blockNumber fails. The verdict is reported, not replaced by network_unavailable: the
// next step has no hint, so it would never be. The same when the step ends with Abandoned (the tick yields to a
// command): the verdict is the step's outcome, and the Engine keeps it.
SCFG_TEST(WatcherTests, AHintVerdictSurvivesANetworkFailureOrAbandonedLaterInTheStep) {
    for (const Failure failure : {Failure::Unavailable, Failure::RpcError, Failure::Abandoned}) {
        const std::string mode = failure == Failure::Unavailable ? "RpcUnavailable: "
                                 : failure == Failure::RpcError  ? "RpcError: "
                                                                 : "Abandoned: ";
        RevertedThenFailing transport;
        transport.failure = failure;
        Json pending = Json::dict({{"ps_a", awaiting(Json(kTx))}});
        std::optional<Result<watcher::StepOutcome>> got = step(pending, transport);
        if (!t.check(got && got->ok(), mode + (got && !got->ok() ? got->err().str() : "the step did not finish"))) {
            continue;
        }
        const watcher::StepOutcome& out = got->value();
        t.check(out.error == std::optional<std::string>("tx_reverted"), mode + "error " + out.error.value_or("None"));
        t.check(out.hint_tx.is_str() && out.hint_tx.as_str() == kTx, mode + "names the hint judged");
        SCFG_CHECK_EQ(t, out.status, "waiting");
        t.check(pending.as_dict().get("ps_a").as_dict().get("hint_tx").is_null(), mode + "the hint is cleared");
        t.check(transport.methods == std::vector<std::string>({"eth_getTransactionReceipt", "eth_blockNumber"}),
                mode + "the receipt, then the scan's eth_blockNumber");
    }
}

// The other side: with no verdict in the step, a network failure is network_unavailable and Abandoned is passed on
// (the tick yields there, keeping nothing of this session).
SCFG_TEST(WatcherTests, WithoutAVerdictAFailureIsNetworkUnavailableAndAbandonedIsPassedOn) {
    for (const Failure failure : {Failure::Unavailable, Failure::RpcError, Failure::Abandoned}) {
        RevertedThenFailing transport;
        transport.failure = failure;
        std::optional<Result<watcher::StepOutcome>> got = step(Json::dict({{"ps_a", awaiting(Json())}}), transport);
        if (!t.check(got.has_value(), "the step did not finish")) continue;
        if (failure == Failure::Abandoned) {
            if (t.check(!got->ok(), "Abandoned is passed on")) SCFG_CHECK_EQ(t, got->err().kind, "Abandoned");
            continue;
        }
        if (!t.check(got->ok(), got->ok() ? "" : got->err().str())) continue;
        t.check(got->value().error == std::optional<std::string>("network_unavailable"), "network_unavailable");
        t.check(got->value().hint_tx.is_null(), "no hint named");
    }
}
