// Copyright 2026 Repulse
// Twin of scfg_core/worker.py's submit(): the hand-off through which the checkout and restore controllers
// (Checkout.h, RestoreFlow.h) run Engine commands, and how a command's result comes back to them.
//
// Python's controllers call `submit(lambda e: e.<command>(...))` and poll the concurrent.futures.Future it returns
// (done(), exception(), result(), add_done_callback). Without generators or lambdas over the Engine, a command here
// is one method per Engine command the controllers use, taking that command's arguments and an OnDone callback for
// its Result: the glue's FScfgWorker has exactly these commands (StartSession, ReportHint, ...), and a test's fake
// runs them on an Engine at once or when the test says. What the controller polls is a Delivery<T>, the slot the
// callback fills.
//
// A Submitter:
// - runs the commands one at a time, in the order they were submitted (the Engine is not re-entrant);
// - calls each done at most once, with the command's Result: inside the call (a test's immediate submitter) or later
//   (the worker, from its ticker). A cancelled command is an Err CancelledError (a BaseException, so not a Python
//   exception the controllers could mistake for a network failure). It may also drop done without calling it (a
//   destroyed worker): the controller then never sees that command finish, as with a Python future that never
//   completes;
// - accepts an empty done: nobody waits for that command (Python drops the Future; the worker still reports its Err);
// - lets done submit another command (the abandoned session start's discard does).
// A done holds no reference to its controller, so it may run after the controller is gone.
#pragma once

#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace scfg::worker {

// Where a command's Result goes (an empty one: nobody waits).
template <class T>
using OnDone = std::function<void(Result<T>)>;

// The worker's commands the controllers submit, each named and typed as its Engine method (Engine.h).
class Submitter {
public:
    virtual ~Submitter() = default;
    // Engine::start_session(kind, chain, opts): the new session record.
    virtual void start_session(Json kind, Json chain, Json opts, OnDone<Json> done) = 0;
    // Engine::build_solana_payment(sid, account): {"transaction", "token_balance", "sol_balance"}.
    virtual void build_solana_payment(Json sid, Json account, OnDone<Json> done) = 0;
    // Engine::report_hint(sid, tx_hash).
    virtual void report_hint(Json sid, Json tx_hash, OnDone<void> done) = 0;
    // Engine::set_ui_status(sid, status).
    virtual void set_ui_status(Json sid, Json status, OnDone<void> done) = 0;
    // Engine::discard_session(sid).
    virtual void discard_session(Json sid, OnDone<void> done) = 0;
    // Engine::restore_lookup(tx_hash): the restore candidates.
    virtual void restore_lookup(Json tx_hash, OnDone<std::vector<restore::RestoreCandidate>> done) = 0;
    // Engine::restore_apply(candidates, choices): the receipts added (a list of dicts).
    virtual void restore_apply(std::vector<restore::RestoreCandidate> candidates,
                               std::optional<std::map<int64_t, Json>> choices, OnDone<Json> done) = 0;
};

// A submitted command's result as the controller polls it (the part of Python's Future the controllers use). Shared
// between the controller and the OnDone a Submitter holds, so either may go first.
template <class T>
class Delivery {
public:
    // The command finished (with a value or an Err).
    bool done() const { return result_.has_value(); }
    // Its Result; only once done().
    const Result<T>& result() const { return *result_; }
    // Python's add_done_callback: fn runs at once when the command is done, else when its Result arrives.
    void add_done_callback(std::function<void(const Result<T>&)> fn) {
        if (done()) {
            fn(*result_);
            return;
        }
        callbacks_.push_back(std::move(fn));
    }
    // Stores the Result and runs the callbacks added so far, in order. A second Result is ignored.
    void deliver(Result<T> r) {
        if (done()) return;
        result_.emplace(std::move(r));
        std::vector<std::function<void(const Result<T>&)>> callbacks = std::move(callbacks_);
        callbacks_.clear();
        for (const auto& fn : callbacks) fn(*result_);
    }
    // The OnDone that delivers into d (it keeps d alive until it runs or is dropped).
    static OnDone<T> receiver(std::shared_ptr<Delivery> d) {
        return [d = std::move(d)](Result<T> r) { d->deliver(std::move(r)); };
    }

private:
    std::optional<Result<T>> result_;
    std::vector<std::function<void(const Result<T>&)>> callbacks_;
};

}  // namespace scfg::worker
