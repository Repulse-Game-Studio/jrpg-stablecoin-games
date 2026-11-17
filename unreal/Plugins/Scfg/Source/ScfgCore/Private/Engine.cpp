// Twin of scfg_core/engine.py (see Engine.h).
#include "Scfg/Engine.h"

#include "Scfg/Backends.h"
#include "Scfg/Payments.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"
#include "Scfg/Reverify.h"
#include "Scfg/Sessions.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace scfg::engine::detail {

constexpr std::array<std::string_view, 4> kSessionKeys = {"item", "amount", "token", "title"};

// copy.deepcopy(dict(store or {})): None (or another falsy value) is an empty dict, a dict is copied.
Json own_store(const Json& store) {
    if (!py::truthy(store)) return Json::dict();
    if (!store.is_dict()) ::scfg::detail::programming_error("an Engine's pending and receipts must be dicts or None");
    return deep_copy(store);
}

// make()'s check of a store: what own_store takes without aborting.
Status check_store(const Json& store, std::string_view name) {
    if (!py::truthy(store) || store.is_dict()) return ok();
    return Err::make("TypeError", "an Engine's " + std::string(name) + " must be a dict or None, not " +
                                      py::type_name(store));
}

// One command at a time (Engine.h): a network command holds its Engine's flag for as long as its coroutine is alive
// (a local of the coroutine, so it is released when the coroutine returns or its frame is destroyed). The flag is
// shared, not referenced: a frame destroyed after its Engine releases a flag nobody reads any more, instead of
// writing into the freed Engine.
class Running {
public:
    Running(std::shared_ptr<bool> busy, std::string_view command) : busy_(std::move(busy)) {
        idle_or_abort(*busy_, command);
        *busy_ = true;
    }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    ~Running() { *busy_ = false; }

    // A command started while another runs (re-entry from a callback, a click while a tick waits for HTTP):
    // Python's workers never interleave jobs, and the running one holds handles to the records the new one would
    // change, so it is a programming error.
    static void idle_or_abort(bool busy, std::string_view command) {
        if (!busy) return;
        const std::string what = "Engine::" + std::string(command) +
                                 "() started while another Engine command is running (one command at a time, Engine.h)";
        ::scfg::detail::programming_error(what.c_str());
    }

private:
    std::shared_ptr<bool> busy_;
};

SystemEntropy& system_entropy() {
    static SystemEntropy e;
    return e;
}

// self.pending[sid]: the stored key, or the KeyError (TypeError for an unhashable sid).
Result<std::string> pending_key(const Json& pending, const Json& sid) {
    SCFG_TRY(std::optional<std::string> key, py::str_key(sid));
    if (!key || !pending.as_dict().contains(*key)) return py::key_error_for(sid);
    return *key;
}

}  // namespace scfg::engine::detail

namespace scfg::engine {

namespace base = backends::base;

Result<std::unique_ptr<Engine>> Engine::make(config::Config config, coio::Transport& transport, Json pending,
                                             Json receipts, std::function<int64_t()> clock, RpcFactory rpc_factory,
                                             std::function<void(const Err&)> handler, Entropy* entropy) {
    SCFG_TRY_VOID(detail::check_store(pending, "pending"));
    SCFG_TRY_VOID(detail::check_store(receipts, "receipts"));
    return std::make_unique<Engine>(std::move(config), transport, std::move(pending), std::move(receipts),
                                    std::move(clock), std::move(rpc_factory), std::move(handler), entropy);
}

Engine::Engine(config::Config config, coio::Transport& transport, Json pending, Json receipts,
               std::function<int64_t()> clock, RpcFactory rpc_factory, std::function<void(const Err&)> handler,
               Entropy* entropy)
    : on_error(handler ? std::move(handler) : std::function<void(const Err&)>(errors::print_error)),
      config_(std::move(config)),
      transport_(&transport),
      pending_(detail::own_store(pending)),
      receipts_(detail::own_store(receipts)),
      clock_(std::move(clock)),
      rpc_factory_(std::move(rpc_factory)),
      entropy_(entropy != nullptr ? entropy : &detail::system_entropy()) {
    if (!clock_) ::scfg::detail::programming_error("an Engine needs a clock");
    if (!rpc_factory_) {
        rpc_factory_ = [this](const chains::Chain& c) -> Result<base::Rpc> {
            SCFG_TRY(const base::Backend* backend, backends::backend_for(c));
            return backend->make_rpc(c, *transport_);
        };
    }
}

// --- chains & RPC ---

Result<chains::Chain> Engine::chain(std::string_view key) const {
    OrderedMap<chains::Chain> registry = config_.registry();
    const chains::Chain* found = registry.find(key);
    if (found == nullptr) return py::key_error(key);
    std::vector<std::string> urls;
    if (const std::vector<std::string>* extra = config_.rpc_urls.find(key)) urls = *extra;
    urls.insert(urls.end(), found->rpc.begin(), found->rpc.end());
    const BigInt* confirmations = config_.confirmations.find(key);
    return found->with(std::move(urls), confirmations != nullptr ? *confirmations : found->confirmations);
}

Result<chains::Chain> Engine::chain_for(const Json& key) const {
    SCFG_TRY(std::optional<std::string> name, py::str_key(key));
    if (!name) return py::key_error_for(key);
    return chain(*name);
}

Result<base::Rpc> Engine::rpc(const std::string& key) {
    if (const base::Rpc* known = rpcs_.find(key)) return *known;
    SCFG_TRY(chains::Chain c, chain(key));
    SCFG_TRY(base::Rpc made, rpc_factory_(c));
    if (!base::is_set(made)) ::scfg::detail::programming_error("an RPC factory returned no client");
    rpcs_.set(key, made);
    return made;
}

base::RpcFor Engine::rpc_for() {
    return [this](std::string key) { return rpc(key); };
}

void Engine::report(const Err& e) const {
    if (on_error) {
        on_error(e);
    } else {
        errors::print_error(e);
    }
}

// --- payments ---

Co<Json> Engine::start_session(Json kind, Json chain_name, Json opts) {
    const detail::Running running(running_, "start_session");
    if (!opts.is_dict()) {
        co_return Err::type_error("start_session() argument after ** must be a mapping, not '" +
                                  std::string(opts.type_name()) + "'");
    }
    for (const auto& [key, value] : opts.as_dict()) {
        (void)value;
        if (std::find(detail::kSessionKeys.begin(), detail::kSessionKeys.end(), key) == detail::kSessionKeys.end()) {
            co_return Err::type_error("start_session() got an unexpected keyword argument " + py::repr_str(key));
        }
    }
    SCFG_CO_TRY(payments::PaymentSpec spec, payments::prepare_payment(config_, kind, opts));
    const bool available = chain_name.is_str() && std::find(spec.chains.begin(), spec.chains.end(),
                                                            chain_name.as_str()) != spec.chains.end();
    if (!available) {
        SCFG_CO_TRY(std::string shown, py::repr(chain_name));
        co_return Err::config_error("chain.token_unavailable", spec.token + " is not available on chain " + shown);
    }
    const std::string key = chain_name.as_str();
    SCFG_CO_TRY(chains::Chain chain_obj, chain(key));
    SCFG_CO_TRY(const base::Backend* backend, backends::backend_for(chain_obj));
    SCFG_CO_TRY(base::Rpc client, rpc(key));
    SCFG_CO_TRY(Json fields, co_await backend->start(client, chain_obj, config_.wallet_for(chain_obj), spec.token,
                                                     std::ref(*entropy_)));
    if (!fields.is_dict()) co_return py::no_attribute(fields, "pop");
    const Json* start = fields.as_dict().find("start_block");
    if (start == nullptr) co_return py::key_error("start_block");
    sessions::NewSession args;
    args.start_block = *start;
    fields.as_dict().erase("start_block");  // fields.pop("start_block")
    args.kind = Json(spec.kind);
    args.item = spec.item;
    args.title = spec.title;
    args.token = Json(spec.token);
    args.chain = Json(key);
    args.price = spec.price;
    args.code = Json(spec.code);
    args.now = Json(clock_());
    args.extra = fields;
    SCFG_CO_TRY(Json rec, sessions::create_session(pending_.as_dict(), args, *entropy_));
    co_return deep_copy(rec);
}

Co<Json> Engine::build_solana_payment(Json sid, Json account) {
    const detail::Running running(running_, "build_solana_payment");
    SCFG_CO_TRY(std::string key, detail::pending_key(pending_, sid));
    Json rec = pending_.as_dict().get(key);
    SCFG_CO_TRY(Json chain_key, py::get(rec, "chain"));
    SCFG_CO_TRY(chains::Chain c, chain_for(chain_key));
    if (c.family != "solana") co_return Err::value_error("not a Solana session");
    SCFG_CO_TRY(const base::Backend* backend, backends::backend_for(c));
    SCFG_CO_TRY(base::Rpc client, rpc(c.key));
    co_return co_await backend->build_payment(rec, client, c, account);
}

Status Engine::report_hint(const Json& sid, const Json& tx_hash) {
    detail::Running::idle_or_abort(*running_, "report_hint");
    SCFG_TRY(std::string key, detail::pending_key(pending_, sid));
    Json rec = pending_.as_dict().get(key);
    SCFG_TRY(Json chain_key, py::get(rec, "chain"));
    SCFG_TRY(chains::Chain c, chain_for(chain_key));
    SCFG_TRY(const base::Backend* backend, backends::backend_for(c));
    sessions::Normalize normalize = [backend](const Json& text) { return backend->normalize_tx(text); };
    return sessions::set_hint(pending_.as_dict(), key, tx_hash, Json(clock_()), normalize);
}

Status Engine::set_ui_status(const Json& sid, const Json& status) {
    detail::Running::idle_or_abort(*running_, "set_ui_status");
    SCFG_TRY(std::optional<std::string> key, py::str_key(sid));  // `sid in self.pending`
    if (!key || !pending_.as_dict().contains(*key)) return ok();
    return sessions::set_ui_status(pending_.as_dict(), *key, status);
}

Status Engine::discard_session(const Json& sid) {
    detail::Running::idle_or_abort(*running_, "discard_session");
    SCFG_TRY(std::optional<std::string> key, py::str_key(sid));  // self.pending.get(sid)
    if (!key) return ok();
    Json rec = pending_.as_dict().get(*key);
    if (rec.is_null()) return ok();
    SCFG_TRY(Json state, py::get(rec, "state"));
    if (!py::eq(state, Json("awaiting"))) return ok();
    SCFG_TRY(Json hint, py::get(rec, "hint_tx"));
    if (py::truthy(hint)) return ok();
    pending_.as_dict().erase(*key);
    return ok();
}

Co<std::vector<StepOutcome>> Engine::tick() {
    const detail::Running running(running_, "tick");
    const Json now(clock_());
    std::vector<StepOutcome> outcomes;
    // list(self.pending.items()): a step may remove sessions (its own, when it is paid or closed).
    const std::vector<Json::Dict::Entry> stored(pending_.as_dict().begin(), pending_.as_dict().end());
    for (const Json::Dict::Entry& entry : stored) {
        SCFG_CO_TRY(std::optional<StepOutcome> outcome, co_await advance_one(entry.first, entry.second, now));
        if (outcome) outcomes.push_back(std::move(*outcome));
    }
    SCFG_CO_TRY(std::vector<StepOutcome> expired, expire(now));
    for (StepOutcome& o : expired) outcomes.push_back(std::move(o));
    co_return outcomes;
}

Co<std::optional<StepOutcome>> Engine::advance_one(std::string sid, Json rec, Json now) {
    Result<std::optional<StepOutcome>> stepped = co_await step_session(sid, rec, now);
    if (stepped.ok()) co_return std::move(stepped);
    // except Exception: a crash in one session never stops the others. A BaseException (CancelledError) is not
    // an Exception: it leaves tick() at once.
    if (!stepped.err().is_a("Exception")) co_return std::move(stepped).err();
    report(stepped.err());
    StepOutcome failed;
    failed.session_id = std::move(sid);
    failed.status = "waiting";
    failed.error = "internal_error";
    co_return std::optional<StepOutcome>(std::move(failed));
}

// A session this build can no longer watch (its chain left the registry, or the game stopped accepting that family)
// is kept until its scan horizon, then closed.
Co<std::optional<StepOutcome>> Engine::step_session(std::string sid, Json rec, Json now) {
    using Maybe = std::optional<StepOutcome>;
    SCFG_CO_TRY(Json chain_key, py::get(rec, "chain"));
    SCFG_CO_TRY(std::optional<std::string> name, py::str_key(chain_key));  // `key not in self.config.registry`
    bool watchable = false;
    if (name && config_.registry().contains(*name)) {
        SCFG_CO_TRY(chains::Chain c, chain(*name));
        watchable = config_.wallet_for(c).has_value();
    }
    if (!watchable) {
        SCFG_CO_TRY(bool past, sessions::is_past_horizon(rec, now));
        if (!past) co_return Maybe();
        sessions::remove_session(pending_.as_dict(), sid);
        StepOutcome closed;
        closed.session_id = std::move(sid);
        closed.status = "closed";
        co_return Maybe(std::move(closed));
    }
    SCFG_CO_TRY(chains::Chain c, chain(*name));
    SCFG_CO_TRY(const base::Backend* backend, backends::backend_for(c));
    SCFG_CO_TRY(base::Rpc client, rpc(*name));
    std::string wallet = *config_.wallet_for(c);
    SCFG_CO_TRY(StepOutcome outcome,
                co_await backend->advance(pending_, receipts_, sid, client, c, wallet, config_.items, now));
    co_return Maybe(std::move(outcome));
}

Result<std::vector<StepOutcome>> Engine::expire(const Json& now) {
    std::vector<StepOutcome> out;
    for (const auto& [sid, rec] : pending_.as_dict()) {
        SCFG_TRY(Json ui, py::get(rec, "ui_status"));
        if (!py::eq(ui, Json("open")) && !py::eq(ui, Json("pending"))) continue;
        SCFG_TRY(bool past, sessions::is_past_window(rec, now));
        if (!past) continue;
        Json record = rec;  // the stored dict itself
        record.as_dict().set("ui_status", "expired");
        models::PaymentResult result;
        result.status = "expired";
        SCFG_TRY(result.kind, py::get(rec, "kind"));
        SCFG_TRY(result.item, py::get(rec, "item"));
        SCFG_TRY(result.token, py::get(rec, "token"));
        SCFG_TRY(result.chain, py::get(rec, "chain"));
        result.session_id = Json(sid);
        StepOutcome o;
        o.session_id = sid;
        o.status = "expired";
        o.result = std::move(result);
        out.push_back(std::move(o));
    }
    return out;
}

// --- receipts ---

Co<Json> Engine::reverify() {
    const detail::Running running(running_, "reverify");
    OrderedMap<chains::Chain> by_key;
    for (const std::string& key : config_.registry().keys()) {
        SCFG_CO_TRY(chains::Chain c, chain(key));
        by_key.set(key, std::move(c));
    }
    OrderedMap<std::vector<std::string>> wallets;
    for (std::string_view family : {std::string_view("evm"), std::string_view("solana")}) {
        wallets.set(std::string(family), config_.accepted_wallets_for(family));
    }
    base::ErrorSink sink = [this](const Err& e) { report(e); };
    SCFG_CO_TRY(Json removed, co_await reverify::reverify_receipts(receipts_, rpc_for(), std::move(by_key),
                                                                   std::move(wallets), std::move(sink)));
    SCFG_CO_TRY_VOID(co_await check_ready());
    co_return removed;
}

Co<void> Engine::check_ready() {
    std::set<std::pair<std::string, std::string>> unready;
    SCFG_CO_TRY(OrderedMap<chains::Chain> enabled, config::enabled_chains(config_));
    for (const auto& [key, c] : enabled) {
        std::optional<std::string> wallet = config_.wallet_for(c);
        std::optional<Err> failed;
        OrderedMap<bool> ready;
        Result<const base::Backend*> backend = backends::backend_for(c);
        if (!backend.ok()) {
            failed = backend.err();
        } else {
            std::string name = key;
            base::LazyRpc lazy = [this, name]() { return rpc(name); };
            Result<OrderedMap<bool>> got = co_await backend.value()->check_ready(std::move(lazy), c, wallet);
            if (got.ok()) {
                ready = got.take();
            } else {
                failed = std::move(got).err();
            }
        }
        if (failed) {
            if (rpc::network_failure(*failed)) continue;  // never hide an option on doubt
            if (!failed->is_a("Exception")) co_return *failed;
            report(*failed);
            continue;
        }
        for (const auto& [token, receivable] : ready) {
            if (!receivable) unready.emplace(key, token);
        }
    }
    unready_ = std::move(unready);
    co_return ok();
}

Co<std::vector<restore::RestoreCandidate>> Engine::restore_lookup(Json tx_hash) {
    const detail::Running running(running_, "restore_lookup");
    std::vector<std::string> ordered = config_.chains;
    for (const std::string& key : config_.registry().keys()) {
        if (std::find(config_.chains.begin(), config_.chains.end(), key) == config_.chains.end()) ordered.push_back(key);
    }
    OrderedMap<chains::Chain> by_key;
    for (const std::string& key : ordered) {
        SCFG_CO_TRY(chains::Chain c, chain(key));
        by_key.set(key, std::move(c));
    }
    base::ErrorSink sink = [this](const Err& e) { report(e); };
    co_return co_await restore::lookup(std::move(tx_hash), config_, std::move(by_key), rpc_for(), receipts_,
                                       std::move(sink));
}

Result<Json> Engine::restore_apply(const std::vector<restore::RestoreCandidate>& candidates,
                                   const std::optional<std::map<int64_t, Json>>& choices) {
    detail::Running::idle_or_abort(*running_, "restore_apply");
    return restore::apply(receipts_.as_dict(), candidates, choices);
}

Result<models::Item> Engine::add_item(const Json& item_id, const Json& kwargs) {
    detail::Running::idle_or_abort(*running_, "add_item");
    return config::add_item(config_, item_id, kwargs);
}

Result<Snapshot> Engine::snapshot() const {
    SCFG_TRY(receipts::Ownership own, receipts::compute_ownership(receipts_.as_dict(), config_.items));
    Snapshot s;
    s.pending = deep_copy(pending_);
    s.receipts = deep_copy(receipts_);
    s.owned = std::move(own.owned);
    s.tip_total = own.tip_total;
    s.unready.assign(unready_.begin(), unready_.end());
    return s;
}

}  // namespace scfg::engine
