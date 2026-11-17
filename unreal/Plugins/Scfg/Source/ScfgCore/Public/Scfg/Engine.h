// Twin of scfg_core/engine.py (and of unity/.../Core/Engine.cs): the Engine composes config, sessions, watcher,
// receipts and restore.
//
// It owns all plugin state (deep copies of what the game persists) and is not re-entrant: one caller drives it, one
// command at a time, on one thread. Readers get Snapshots, which are detached copies. Methods that need the network
// are Co<...> (Python's *_co jobs) and only await the injected transport; the others are plain.
//
// One command at a time: a network command runs until its Co finishes or is destroyed, and while it runs (suspended
// on a request) no other command may start, as Python's Worker and PumpWorker queue commands and never interleave
// jobs. Starting a network command, or calling report_hint, set_ui_status, discard_session or restore_apply, while
// one runs is a programming error and aborts (running() tells). The glue queues commands, as the PumpWorker does;
// the readers (pending(), receipts(), snapshot(), config(), chain()) are not commands. The next command may start
// from the finished one's Job on_done, which runs after its coroutine has returned.
//
// Lifetime: the Engine's coroutines refer to it (member coroutines) and to the transport, so the caller keeps both
// alive, and does not move the Engine, while one runs (Co.h, "Teardown order": destroy the Jobs first). Destroying a
// suspended Co is safe before or after the Engine: it only runs its frames' destructors, and the one that writes
// Engine state (the busy flag) writes a flag it shares with the Engine, so an Engine already gone is not touched.
// What must not happen is a resume after the Engine is gone: destroy the Job before the transport answers or drops
// its request. If the transport is torn down first (the Engine alive), its dropped promises resume the waiting
// coroutine with CancelledError: that is a BaseException, which no step contains, so it leaves tick() (and every
// other method) at once, without another request and without touching the session it was stepping.
//
// Errors: a crash while stepping one session (or, later, checking one receipt or classifying one transfer) is
// reported to on_error and counted, never propagated, exactly where Python has `except Exception`; a BaseException
// (CancelledError) is never contained. on_error is called from inside the running command's coroutine, so it must not
// destroy the Job or Co that runs the command (Co.h, Job): that destroys the frame that is executing, which is
// undefined behaviour with no diagnostic. A handler that wants to tear down records the wish and acts after on_done.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::engine {

using watcher::StepOutcome;

// Python's frozen Snapshot: what readers see, detached from the Engine's state.
struct Snapshot {
    Json pending;
    Json receipts;
    std::vector<std::string> owned;  // owned item ids, sorted by code point (Python's frozenset)
    Dec tip_total;
    // The (chain, token) routes the developer cannot receive on, sorted by code point (a frozenset of tuples).
    std::vector<std::pair<std::string, std::string>> unready;
};

// A chain's RPC object (the default: its backend's make_rpc over the Engine's transport).
using RpcFactory = std::function<Result<backends::base::Rpc>(const chains::Chain&)>;

class SCFGCORE_API Engine {
public:
    // pending and receipts are None or dicts (Python's `dict(pending or {})`), deep-copied; anything else is a
    // programming error. clock gives whole seconds: session timestamps and windows run on it. on_error defaults to
    // errors::print_error, entropy (Python's `secrets`) to a SystemEntropy; a given entropy and the transport must
    // outlive the Engine.
    Engine(config::Config config, coio::Transport& transport, Json pending, Json receipts,
           std::function<int64_t()> clock, RpcFactory rpc_factory = {}, std::function<void(const Err&)> handler = {},
           Entropy* entropy = nullptr);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    const config::Config& config() const { return config_; }
    // The Engine's own stores (dicts, shared handles: a change through them is the Engine's state).
    const Json& pending() const { return pending_; }
    const Json& receipts() const { return receipts_; }
    // Where a contained crash goes. Settable, as Python's attribute: it is read when an error is reported, and an
    // empty one prints the error (Python's `on_error or print_error`). It runs inside the command's coroutine: it
    // must not destroy the Job or Co running that command, nor start another command (see the top of this file).
    std::function<void(const Err&)> on_error;

    // A network command's coroutine is alive (it is running or suspended): no other command may start.
    bool running() const { return *running_; }

    // --- chains & RPC ---

    // The full registry chain (all tokens, so old receipts stay verifiable) with the developer's RPC urls first and
    // the confirmation override applied. A KeyError for a chain the registry does not know.
    Result<chains::Chain> chain(std::string_view key) const;
    // The chain's RPC object, built on first use and kept.
    Result<backends::base::Rpc> rpc(const std::string& key);

    // --- payments ---

    // start_session(kind, chain, *, item=None, amount=None, token=None, title=None): opts holds the keyword
    // arguments (a dict; an unknown one is a TypeError). kind and chain are any value, as Python takes them (a str
    // converts). Returns a copy of the new session record.
    Co<Json> start_session(Json kind, Json chain_name, Json opts);
    // The unsigned transaction a browser wallet signs for a Solana session, account paying: {"transaction"
    // (base64), "token_balance", "sol_balance"} (decimal strings of base units). ValueError for another family.
    Co<Json> build_solana_payment(Json sid, Json account);
    // Python report_hint(sid, tx_hash): the session's family checks and normalizes the hint.
    Status report_hint(const Json& sid, const Json& tx_hash);
    // A no-op for a sid that is not pending; otherwise sessions::set_ui_status.
    Status set_ui_status(const Json& sid, const Json& status);
    // Drop a session the player abandoned before paying (still awaiting, no hint); a no-op otherwise.
    Status discard_session(const Json& sid);
    // Every session, one after another in pending order (never concurrently), then the expiries.
    Co<std::vector<StepOutcome>> tick();

    // --- receipts ---

    // Launch checks: re-verify stored receipts against the full registry, then check that the developer can
    // receive each enabled token. Returns the removed receipt keys (a list, in store order).
    Co<Json> reverify();
    // Restore candidates for a pasted transaction id, from the first chain that knows it: the game's chains in its
    // order, then the rest of the registry.
    Co<std::vector<restore::RestoreCandidate>> restore_lookup(Json tx_hash);
    // Store the candidates as receipts; returns the receipts added (a list of dicts). choices maps a candidate's
    // index to an item id or "tip" for the candidates the player must choose for.
    Result<Json> restore_apply(const std::vector<restore::RestoreCandidate>& candidates,
                               const std::optional<std::map<int64_t, Json>>& choices = std::nullopt);

    // Detached copies of the state, ownership recomputed from the receipts.
    Result<Snapshot> snapshot() const;

private:
    // Python _advance: one session's step, or nullopt for a session kept unwatched until its horizon.
    Co<std::optional<StepOutcome>> advance_one(std::string sid, Json rec, Json now);
    Co<std::optional<StepOutcome>> step_session(std::string sid, Json rec, Json now);
    // Python _expire: the 30-minute payment window closing (scanning continues up to 24 h).
    Result<std::vector<StepOutcome>> expire(const Json& now);
    // Python _check_ready.
    Co<void> check_ready();
    // self.chain(rec["chain"]) for a stored value: chain() of a str, the KeyError of any other hashable value
    // (registry[key]), the TypeError of an unhashable one.
    Result<chains::Chain> chain_for(const Json& key) const;
    // Python _report: on_error as it is when the error is reported, or print_error.
    void report(const Err& e) const;
    backends::base::RpcFor rpc_for();

    config::Config config_;
    coio::Transport* transport_;
    Json pending_;
    Json receipts_;
    std::function<int64_t()> clock_;
    RpcFactory rpc_factory_;
    Entropy* entropy_;
    OrderedMap<backends::base::Rpc> rpcs_;
    std::set<std::pair<std::string, std::string>> unready_;
    // Shared with the running command's guard, which may outlive the Engine (see Lifetime above).
    std::shared_ptr<bool> running_ = std::make_shared<bool>(false);
};

}  // namespace scfg::engine
