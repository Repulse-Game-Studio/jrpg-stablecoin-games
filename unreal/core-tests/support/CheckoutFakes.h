// Copyright 2026 Repulse
// No Python twin: the test doubles of tests/unit/fakes.py that the checkout and restore controller tests use
// (FakeClock, FakeChain, FakeSolanaChain, transfer_log, immediate_submitter) and test_checkout_abandoned_start.py's
// DeferredSubmitter, for CheckoutTests, CheckoutStepsTests, CheckoutSolanaTests, CheckoutAbandonedStartTests and
// RestoreFlowTests.
//
// Python's fakes replace the RPC objects; the C++ RPC objects are concrete, so the fakes here are a transport that
// answers JSON-RPC as the fake chains would (FakeNodes), and an RPC factory that points each chain's client at it.
// A Python fake has no failover cooldown, so these clients' clock jumps a day per read: an endpoint that failed is
// asked again on the next call, as a Python fake that comes back up answers at once. On that clock every call's
// CALL_BUDGET would be spent before its first attempt, so these clients have none (an infinite budget).
#pragma once

#include "Scfg/Checkout.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scfg_test::fakes {

// FakeClock: whole seconds (the Engine's clock), shared by every copy of fn().
struct FakeClock {
    explicit FakeClock(int64_t start = 1000) : now(std::make_shared<int64_t>(start)) {}
    std::function<int64_t()> fn() const;
    void advance(int64_t seconds) { *now += seconds; }
    std::shared_ptr<int64_t> now;
};

// FakeChain: an EVM chain's node. hashes: {block: hash} (unlisted blocks get "0x" + 64 hex digits of the number);
// logs: raw logs eth_getLogs returns (filtered by block range and address only, as Python's); receipts: {tx hash:
// receipt} (unlisted: None); down: every request fails, so the client raises RpcUnavailable.
struct FakeChain {
    int64_t head = 1000;
    std::map<int64_t, std::string> hashes;
    std::vector<scfg::Json> logs;
    std::map<std::string, scfg::Json> receipts;
    bool down = false;
    std::vector<std::string> calls;
    std::string block_hash(int64_t n) const;
};

// FakeSolanaChain: a Solana node with no history (what the controller tests reach: getSlot and
// getSignaturesForAddress for a session start).
struct FakeSolanaChain {
    int64_t slot = 1000;
    bool down = false;
    std::vector<std::string> calls;
};

// The fake chains' nodes as one transport, by chain key, each made on first use (FakeChain(head=1000) and
// FakeSolanaChain(slot=1000), as the tests' setdefault does). A request no fake answers is a problem (and a
// JSON-RPC error).
class FakeNodes final : public scfg::coio::Transport {
public:
    std::map<std::string, FakeChain> evm;
    std::map<std::string, FakeSolanaChain> solana;
    std::vector<std::string> problems;

    scfg::Future<scfg::Json> post_json(std::string url, scfg::Json payload, double timeout) override;
    // The Engine's rpc_factory: each chain's client posts to this transport only, under the chain's key.
    scfg::engine::RpcFactory rpc_factory();
};

// An Engine over nodes (which must outlive it) with the clock; contained errors are recorded in *errors when given.
std::unique_ptr<scfg::engine::Engine> make_engine(scfg::config::Config config, FakeNodes& nodes,
                                                  std::function<int64_t()> clock,
                                                  std::vector<std::string>* errors = nullptr);

// transfer_log(token, sender, recipient, value, block=, tx_hash=, log_index=0, block_hash=None).
scfg::Json transfer_log(const std::string& token, const std::string& sender, const std::string& recipient,
                        int64_t value, int64_t block, const std::string& tx_hash, int64_t log_index = 0,
                        std::optional<std::string> block_hash = std::nullopt);

// Runs the Engine's commands. dispatch() decides when: ImmediateSubmitter at once (immediate_submitter, whose
// Future is done when submit returns), DeferredSubmitter when the test calls run().
class EngineSubmitter : public scfg::worker::Submitter {
public:
    explicit EngineSubmitter(scfg::engine::Engine& engine) : engine_(&engine) {}
    // Honours the abandon flag as worker.py does: a start abandoned before it runs is cancelled (CancelledError),
    // and one abandoned while it runs is refused its next request (Engine::refuse_requests).
    void start_session(scfg::Json kind, scfg::Json chain, scfg::Json opts, scfg::worker::OnDone<scfg::Json> done,
                       scfg::worker::AbandonFlag abandoned) override;
    void build_solana_payment(scfg::Json sid, scfg::Json account, scfg::worker::OnDone<scfg::Json> done) override;
    void report_hint(scfg::Json sid, scfg::Json tx_hash, scfg::worker::OnDone<void> done) override;
    void set_ui_status(scfg::Json sid, scfg::Json status, scfg::worker::OnDone<void> done) override;
    void discard_session(scfg::Json sid, scfg::worker::OnDone<void> done) override;
    void restore_lookup(scfg::Json tx_hash,
                        scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>> done) override;
    void restore_apply(std::vector<scfg::restore::RestoreCandidate> candidates,
                       std::optional<std::map<int64_t, scfg::Json>> choices,
                       scfg::worker::OnDone<scfg::Json> done) override;

protected:
    // run performs the command and calls its done; fail calls done with an Err instead (a test's
    // fut.set_exception).
    virtual void dispatch(std::string name, std::function<void()> run, std::function<void(scfg::Err)> fail,
                          scfg::worker::AbandonFlag abandoned = {}) = 0;
    scfg::engine::Engine* engine_;
};

class ImmediateSubmitter final : public EngineSubmitter {
public:
    using EngineSubmitter::EngineSubmitter;

protected:
    void dispatch(std::string name, std::function<void()> run, std::function<void(scfg::Err)> fail,
                  scfg::worker::AbandonFlag abandoned) override;
};

class DeferredSubmitter final : public EngineSubmitter {
public:
    struct Queued {
        std::string name;
        std::function<void()> run;
        std::function<void(scfg::Err)> fail;
        scfg::worker::AbandonFlag abandoned;  // a session start's (empty for the other commands)
    };
    using EngineSubmitter::EngineSubmitter;
    // Runs the queue until it is empty, commands queued meanwhile included.
    void run();
    std::deque<Queued> queue;

protected:
    void dispatch(std::string name, std::function<void()> run, std::function<void(scfg::Err)> fail,
                  scfg::worker::AbandonFlag abandoned) override;
};

// Records the commands submitted (by name) and never runs them (`lambda fn: submitted.append(fn)`, and `lambda fn:
// None`).
class RecordingSubmitter final : public scfg::worker::Submitter {
public:
    std::vector<std::string> submitted;
    void start_session(scfg::Json, scfg::Json, scfg::Json, scfg::worker::OnDone<scfg::Json>,
                       scfg::worker::AbandonFlag) override;
    void build_solana_payment(scfg::Json, scfg::Json, scfg::worker::OnDone<scfg::Json>) override;
    void report_hint(scfg::Json, scfg::Json, scfg::worker::OnDone<void>) override;
    void set_ui_status(scfg::Json, scfg::Json, scfg::worker::OnDone<void>) override;
    void discard_session(scfg::Json, scfg::worker::OnDone<void>) override;
    void restore_lookup(scfg::Json, scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>>) override;
    void restore_apply(std::vector<scfg::restore::RestoreCandidate>, std::optional<std::map<int64_t, scfg::Json>>,
                       scfg::worker::OnDone<scfg::Json>) override;
};

// The tests' FakeServer (test_checkout.py's, test_checkout_steps.py's and its PageWithPump in one): what the
// controller handed the factory, and what the controller did to the page. Kept by the test, so it outlives the page.
inline constexpr const char* kServerUrl = "http://127.0.0.1:1234/c/secret/";
struct ServerLog {
    scfg::checkout::ServerArgs args;
    bool started = false;
    bool stopped = false;
    int pumps = 0;
    int reopens = 0;
};
using Servers = std::vector<std::shared_ptr<ServerLog>>;

class FakeServer final : public scfg::checkout::Server {
public:
    explicit FakeServer(std::shared_ptr<ServerLog> log) : log_(std::move(log)) {}
    scfg::Status start() override;
    void stop() override { log_->stopped = true; }
    std::string url() const override { return kServerUrl; }
    void reopen() override { ++log_->reopens; }
    void pump() override { ++log_->pumps; }

private:
    std::shared_ptr<ServerLog> log_;
};

// A factory that makes FakeServers and appends each one's log to *servers.
scfg::checkout::ServerFactory server_factory(std::shared_ptr<Servers> servers);
// `def broken(**kw): raise OSError("no sockets")` (a RuntimeError here: the core's Err table has no OSError).
scfg::checkout::ServerFactory broken_factory();

// What the test files' setup() functions build: the config (build_config(**config), then add_item(cfg, "chapter2",
// code=37, price="4.99", token="USDC", title="Chapter 2")), an Engine over fake nodes at FakeClock(now), the spec
// (prepare_payment(cfg, "unlock", item="chapter2"), or a tip of "3" USDC), and the controller with an immediate or a
// deferred submitter.
struct RigOptions {
    std::string kind = "unlock";
    scfg::Json config{};  // build_config's keyword arguments
    bool mobile = false;
    bool server = true;  // server_factory=FakeServer (else None)
    std::function<bool(const scfg::chains::Chain&)> browser_wallet;
    std::function<scfg::checkout::Routes()> unready;
    std::string game_name = "Game";
    bool deferred = false;
    int64_t now = 100;
};

// The parts, in teardown order (the controller first, the nodes last).
struct Rig {
    std::unique_ptr<FakeNodes> nodes = std::make_unique<FakeNodes>();
    FakeClock clock;
    std::shared_ptr<std::vector<std::string>> errors = std::make_shared<std::vector<std::string>>();
    std::unique_ptr<scfg::engine::Engine> engine;
    std::shared_ptr<Servers> servers = std::make_shared<Servers>();
    std::shared_ptr<ImmediateSubmitter> immediate;  // set unless deferred
    std::shared_ptr<DeferredSubmitter> deferred;    // set when deferred
    std::unique_ptr<scfg::checkout::CheckoutController> ctl;

    // fakes[key] (made on first use).
    FakeChain& fake(const std::string& key) { return nodes->evm[key]; }
    // engine.pending[sid], or None.
    scfg::Json pending(const std::optional<std::string>& sid) const;
    // for o in engine.tick(): ctl.on_outcome(o).
    scfg::Status deliver();
};

scfg::Result<Rig> make_rig(RigOptions options);

// for o in engine.tick(): sinks each outcome (the fakes answer at once, so the tick finishes inside the call). An
// Err (or a tick that suspended) is returned.
scfg::Status deliver(scfg::engine::Engine& engine, const std::function<void(const scfg::watcher::StepOutcome&)>& sink);

}  // namespace scfg_test::fakes
