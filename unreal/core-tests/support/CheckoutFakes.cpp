// Copyright 2026 Repulse
// No Python twin: tests/unit/fakes.py's doubles for the controller tests (see CheckoutFakes.h).
#include "support/CheckoutFakes.h"

#include "Scfg/Backends.h"
#include "Scfg/BackendsBase.h"
#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Evm.h"
#include "Scfg/Models.h"
#include "Scfg/Payments.h"
#include "Scfg/Py.h"

#include <string_view>
#include <utility>

using scfg::Err;
using scfg::Json;
using scfg::Result;

namespace scfg_test::fakes {

namespace {

// format(n, "064x").
std::string hex64(const scfg::BigInt& n) {
    std::string digits = scfg::py::hex(n).substr(2);
    return std::string(digits.size() < 64 ? 64 - digits.size() : 0, '0') + digits;
}

Json answer(const Json& payload, Json result) {
    return Json::dict({{"jsonrpc", "2.0"}, {"id", payload.as_dict().get("id")}, {"result", std::move(result)}});
}

Json item(const Json& params, size_t i) {
    if (!params.is_list() || params.as_list().size() <= i) return Json();
    return params.as_list()[i];
}

// The int of a hex quantity, or nullopt.
std::optional<int64_t> quantity(const Json& v) {
    Result<std::optional<int64_t>> n = scfg::py::py_int_hex_int64(v);
    return n.ok() ? n.value() : std::nullopt;
}

template <class T>
Result<T> run_now(scfg::Co<T> co) {
    if (!co.done()) return Err::make("AssertionError", "the command suspended, but the fake nodes answer at once");
    return co.take();
}

template <class T>
void call(const scfg::worker::OnDone<T>& done, Result<T> r) {
    if (done) done(std::move(r));
}

}  // namespace

std::function<int64_t()> FakeClock::fn() const {
    std::shared_ptr<int64_t> at = now;
    return [at] { return *at; };
}

std::string FakeChain::block_hash(int64_t n) const {
    auto it = hashes.find(n);
    return it != hashes.end() ? it->second : "0x" + hex64(n);
}

scfg::Future<Json> FakeNodes::post_json(std::string url, Json payload, double timeout) {
    (void)timeout;
    const std::string_view prefix = "fake://";
    const std::string rest = url.rfind(prefix, 0) == 0 ? url.substr(prefix.size()) : std::string();
    const size_t slash = rest.find('/');
    const std::string family = slash == std::string::npos ? std::string() : rest.substr(0, slash);
    const std::string key = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    const std::string method = payload.as_dict().get("method").as_str();
    const Json params = payload.as_dict().get("params");
    auto refuse = [&](const std::string& why) {
        problems.push_back(url + " " + method + ": " + why);
        return scfg::Future<Json>::completed(Json::dict(
            {{"jsonrpc", "2.0"}, {"id", payload.as_dict().get("id")},
             {"error", Json::dict({{"code", -32601}, {"message", "the fake does not answer " + method}})}}));
    };
    if (family == "evm") {
        FakeChain& chain = evm[key];
        chain.calls.push_back(method);
        if (chain.down) return scfg::Future<Json>::completed(Err::transport_error("connection refused", std::nullopt));
        if (method == "eth_blockNumber") return scfg::Future<Json>::completed(answer(payload, scfg::py::hex(chain.head)));
        if (method == "eth_getBlockByNumber") {
            std::optional<int64_t> n = quantity(item(params, 0));
            if (!n) return refuse("bad block number");
            Json block = *n <= chain.head ? Json::dict({{"hash", chain.block_hash(*n)}}) : Json();
            return scfg::Future<Json>::completed(answer(payload, std::move(block)));
        }
        if (method == "eth_getTransactionReceipt") {
            const Json tx = item(params, 0);
            auto it = tx.is_str() ? chain.receipts.find(tx.as_str()) : chain.receipts.end();
            return scfg::Future<Json>::completed(answer(payload, it != chain.receipts.end() ? it->second : Json()));
        }
        if (method == "eth_getLogs") {
            const Json filter = item(params, 0);
            if (!filter.is_dict()) return refuse("no filter");
            std::optional<int64_t> from = quantity(filter.as_dict().get("fromBlock"));
            std::optional<int64_t> to = quantity(filter.as_dict().get("toBlock"));
            const Json address = filter.as_dict().get("address");
            if (!from || !to || !address.is_str()) return refuse("bad filter");
            Json::List out;
            for (const Json& log : chain.logs) {
                std::optional<int64_t> block = quantity(log.as_dict().get("blockNumber"));
                const bool here = block && *from <= *block && *block <= *to;
                if (here && scfg::py::lower_ascii(log.as_dict().get("address").as_str()) ==
                                scfg::py::lower_ascii(address.as_str())) {
                    out.push_back(log);
                }
            }
            return scfg::Future<Json>::completed(answer(payload, Json(std::move(out))));
        }
        return refuse("unknown method");
    }
    if (family == "solana") {
        FakeSolanaChain& chain = solana[key];
        chain.calls.push_back(method);
        if (chain.down) return scfg::Future<Json>::completed(Err::transport_error("connection refused", std::nullopt));
        if (method == "getSlot") return scfg::Future<Json>::completed(answer(payload, chain.slot));
        if (method == "getSignaturesForAddress") return scfg::Future<Json>::completed(answer(payload, Json::list()));
        return refuse("unknown method");
    }
    return refuse("no fake chain at this url");
}

scfg::engine::RpcFactory FakeNodes::rpc_factory() {
    std::shared_ptr<double> ticks = std::make_shared<double>(0.0);
    return [this, ticks](const scfg::chains::Chain& chain) -> Result<scfg::backends::base::Rpc> {
        SCFG_TRY(const scfg::backends::base::Backend* backend, scfg::backends::backend_for(chain));
        std::vector<std::string> urls{"fake://" + chain.family + "/" + chain.key};
        SCFG_TRY(scfg::backends::base::Rpc rpc, backend->make_rpc(chain.with(std::move(urls)), *this));
        scfg::backends::base::client_of(rpc).clock = [ticks] { return *ticks += 86400.0; };
        return rpc;
    };
}

std::unique_ptr<scfg::engine::Engine> make_engine(scfg::config::Config config, FakeNodes& nodes,
                                                  std::function<int64_t()> clock, std::vector<std::string>* errors) {
    std::function<void(const Err&)> handler = [errors](const Err& e) {
        if (errors != nullptr) errors->push_back(e.str());
    };
    return std::make_unique<scfg::engine::Engine>(std::move(config), nodes, Json(), Json(), std::move(clock),
                                                  nodes.rpc_factory(), std::move(handler), nullptr);
}

Json transfer_log(const std::string& token, const std::string& sender, const std::string& recipient, int64_t value,
                  int64_t block, const std::string& tx_hash, int64_t log_index, std::optional<std::string> block_hash) {
    Json topics = Json::list({std::string(scfg::evm::TRANSFER_TOPIC), scfg::evm::address_topic(sender).value(),
                              scfg::evm::address_topic(recipient).value()});
    return Json::dict({{"address", scfg::py::lower_ascii(token)},
                       {"topics", topics},
                       {"data", "0x" + hex64(value)},
                       {"transactionHash", tx_hash},
                       {"logIndex", scfg::py::hex(log_index)},
                       {"blockNumber", scfg::py::hex(block)},
                       {"blockHash", block_hash ? *block_hash : "0x" + hex64(block)}});
}

// ---- submitters ----

void EngineSubmitter::start_session(Json kind, Json chain, Json opts, scfg::worker::OnDone<Json> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "start_session",
        [e, kind, chain, opts, done] {
            Result<Json> r = run_now(e->start_session(kind, chain, opts));
            call(done, std::move(r));
        },
        [done](Err err) { call<Json>(done, std::move(err)); });
}

void EngineSubmitter::build_solana_payment(Json sid, Json account, scfg::worker::OnDone<Json> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "build_solana_payment",
        [e, sid, account, done] {
            Result<Json> r = run_now(e->build_solana_payment(sid, account));
            call(done, std::move(r));
        },
        [done](Err err) { call<Json>(done, std::move(err)); });
}

void EngineSubmitter::report_hint(Json sid, Json tx_hash, scfg::worker::OnDone<void> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "report_hint", [e, sid, tx_hash, done] { call(done, e->report_hint(sid, tx_hash)); },
        [done](Err err) { call<void>(done, std::move(err)); });
}

void EngineSubmitter::set_ui_status(Json sid, Json status, scfg::worker::OnDone<void> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "set_ui_status", [e, sid, status, done] { call(done, e->set_ui_status(sid, status)); },
        [done](Err err) { call<void>(done, std::move(err)); });
}

void EngineSubmitter::discard_session(Json sid, scfg::worker::OnDone<void> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "discard_session", [e, sid, done] { call(done, e->discard_session(sid)); },
        [done](Err err) { call<void>(done, std::move(err)); });
}

void EngineSubmitter::restore_lookup(Json tx_hash,
                                     scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>> done) {
    scfg::engine::Engine* e = engine_;
    using Found = std::vector<scfg::restore::RestoreCandidate>;
    dispatch(
        "restore_lookup",
        [e, tx_hash, done] {
            Result<Found> r = run_now(e->restore_lookup(tx_hash));
            call(done, std::move(r));
        },
        [done](Err err) { call<Found>(done, std::move(err)); });
}

void EngineSubmitter::restore_apply(std::vector<scfg::restore::RestoreCandidate> candidates,
                                    std::optional<std::map<int64_t, Json>> choices, scfg::worker::OnDone<Json> done) {
    scfg::engine::Engine* e = engine_;
    dispatch(
        "restore_apply", [e, candidates, choices, done] { call(done, e->restore_apply(candidates, choices)); },
        [done](Err err) { call<Json>(done, std::move(err)); });
}

void ImmediateSubmitter::dispatch(std::string name, std::function<void()> run, std::function<void(Err)> fail) {
    (void)name, (void)fail;
    run();
}

void DeferredSubmitter::dispatch(std::string name, std::function<void()> run, std::function<void(Err)> fail) {
    queue.push_back(Queued{std::move(name), std::move(run), std::move(fail)});
}

void DeferredSubmitter::run() {
    while (!queue.empty()) {
        Queued next = std::move(queue.front());
        queue.pop_front();
        next.run();
    }
}

void RecordingSubmitter::start_session(Json, Json, Json, scfg::worker::OnDone<Json>) {
    submitted.push_back("start_session");
}
void RecordingSubmitter::build_solana_payment(Json, Json, scfg::worker::OnDone<Json>) {
    submitted.push_back("build_solana_payment");
}
void RecordingSubmitter::report_hint(Json, Json, scfg::worker::OnDone<void>) { submitted.push_back("report_hint"); }
void RecordingSubmitter::set_ui_status(Json, Json, scfg::worker::OnDone<void>) { submitted.push_back("set_ui_status"); }
void RecordingSubmitter::discard_session(Json, scfg::worker::OnDone<void>) { submitted.push_back("discard_session"); }
void RecordingSubmitter::restore_lookup(Json, scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>>) {
    submitted.push_back("restore_lookup");
}
void RecordingSubmitter::restore_apply(std::vector<scfg::restore::RestoreCandidate>,
                                       std::optional<std::map<int64_t, Json>>, scfg::worker::OnDone<Json>) {
    submitted.push_back("restore_apply");
}

scfg::Status FakeServer::start() {
    log_->started = true;
    return scfg::ok();
}

scfg::checkout::ServerFactory server_factory(std::shared_ptr<Servers> servers) {
    return [servers](scfg::checkout::ServerArgs args) -> Result<std::unique_ptr<scfg::checkout::Server>> {
        std::shared_ptr<ServerLog> log = std::make_shared<ServerLog>();
        log->args = std::move(args);
        servers->push_back(log);
        return std::unique_ptr<scfg::checkout::Server>(std::make_unique<FakeServer>(log));
    };
}

scfg::checkout::ServerFactory broken_factory() {
    return [](scfg::checkout::ServerArgs) -> Result<std::unique_ptr<scfg::checkout::Server>> {
        return Err::make("RuntimeError", "no sockets");
    };
}

Json Rig::pending(const std::optional<std::string>& sid) const {
    const Json* rec = sid ? engine->pending().as_dict().find(*sid) : nullptr;
    return rec != nullptr ? *rec : Json();
}

scfg::Status Rig::deliver() {
    scfg::checkout::CheckoutController* c = ctl.get();
    return fakes::deliver(*engine, [c](const scfg::watcher::StepOutcome& o) { c->on_outcome(o); });
}

Result<Rig> make_rig(RigOptions options) {
    Rig rig;
    rig.clock = FakeClock(options.now);
    SCFG_TRY(scfg::config::Config cfg, scfg::config::build_config(options.config.is_null() ? Json::dict() : options.config));
    SCFG_TRY_VOID(scfg::config::add_item(
        cfg, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}})));
    SCFG_TRY(scfg::payments::PaymentSpec spec,
             options.kind == "unlock"
                 ? scfg::payments::prepare_payment(cfg, "unlock", Json::dict({{"item", "chapter2"}}))
                 : scfg::payments::prepare_payment(cfg, "tip", Json::dict({{"amount", "3"}, {"token", "USDC"}})));
    rig.engine = make_engine(cfg, *rig.nodes, rig.clock.fn(), rig.errors.get());
    scfg::checkout::CheckoutOptions o;
    o.config = std::move(cfg);
    o.spec = std::move(spec);
    if (options.deferred) {
        rig.deferred = std::make_shared<DeferredSubmitter>(*rig.engine);
        o.submit = rig.deferred;
    } else {
        rig.immediate = std::make_shared<ImmediateSubmitter>(*rig.engine);
        o.submit = rig.immediate;
    }
    o.mobile = options.mobile;
    if (options.server) o.server_factory = server_factory(rig.servers);
    o.browser_wallet = std::move(options.browser_wallet);
    o.unready = std::move(options.unready);
    o.game_name = std::move(options.game_name);
    SCFG_TRY(rig.ctl, scfg::checkout::CheckoutController::make(std::move(o)));
    return rig;
}

scfg::Status deliver(scfg::engine::Engine& engine, const std::function<void(const scfg::watcher::StepOutcome&)>& sink) {
    Result<std::vector<scfg::watcher::StepOutcome>> outcomes = run_now(engine.tick());
    if (!outcomes.ok()) return std::move(outcomes).err();
    for (const scfg::watcher::StepOutcome& o : outcomes.value()) sink(o);
    return scfg::ok();
}

}  // namespace scfg_test::fakes
