// No Python twin: the cooperative driver half of coio.py (run_sync, Sync, synced) as C++20 coroutines.
//
// Python's core is generators: a function that may need the network yields a Request and is resumed with the
// response, or has a TransportError thrown in, and callers write `result = yield from fn(...)`. Here such a
// function returns Co<T> and its callers write `SCFG_CO_TRY(T result, co_await fn(...))`: co_await of a Co<T>
// gives the Result<T> it co_returned, so a Python exception crosses coroutine levels as an Err value. The
// transport hands out a Future<Json> that it completes later through its Promise (an HTTP callback, a later
// tick); single-threaded, nothing here blocks, schedules or uses a clock.
//
//   Co<Json> RpcClient::call(std::string method, Json params) {   // parameters by value (the lint checks)
//       SCFG_CO_TRY(Json resp, co_await transport_->post_json(url, payload, timeout));
//       co_return resp;                                          // a T, an Err or a Result<T>
//   }
//   Job<Json> job;                                               // driven from outside any coroutine
//   job.start(client.call("eth_blockNumber", Json::list()), [](Result<Json> r) { ... });
//
// The rules:
// - Co<T> starts eagerly: the body runs inside the call until its first real suspension, so a callee that
//   finishes without waiting costs no suspension, and loops of thousands of such awaits keep the stack flat.
//   A finished callee resumes its awaiting caller by symmetric transfer (final_suspend), on the stack of whoever
//   completed the last Promise.
// - Co<T> is move-only and owns its frame: destroying an unfinished Co destroys the frame (its locals, and the
//   Co it is awaiting, recursively). That is cancellation; a Promise completed afterwards resumes nothing.
// - Parameters are taken by value: a reference, string_view, span or pointer dangles after the first suspension
//   (LintTests.CoroutineParamsByValue). A member coroutine (RpcClient::call) relies on its object outliving it,
//   which its caller guarantees, as for any member function; do not move the object while one runs.
// - No coroutine lambdas (ruling R8, LintTests.NoCoroutineLambdas): Clang miscompiles them in cases that
//   involve a promise constructor taking the coroutine's arguments (llvm/llvm-project#91983), so the promises here
//   are default-constructible and the coroutines are named functions.
// - Co<void> ends with `co_return scfg::ok();` (or an Err / a Status): a promise cannot have both return_void and
//   return_value, and flowing off the end of a coroutine is undefined. Reaching final_suspend without a result
//   is caught there and aborts (g++ 13 does not warn; Clang does, -Wreturn-type).
// - Build a braced argument before the co_await (`Json params = Json::list({a, b}); co_await call(m, params)`):
//   g++ 13 has an internal compiler error on an initializer list inside a co_await's operand.
// - Exceptions are off, so unhandled_exception() is unreachable; it aborts.
//
// Promise<T> rules (programming errors abort through detail::programming_error):
// - complete() resumes the awaiting coroutine, if any, before it returns; completing before anyone awaits is
//   fine (the await then does not suspend). Completing twice is a programming error.
// - Destroying a Promise that was never completed completes its Future with Err CancelledError (a BaseException,
//   so an `except Exception` in the core does not swallow it), resuming a waiter inside the destructor. A
//   transport cancels its outstanding requests on shutdown by dropping their promises: move them out of its own
//   containers first, since the resumed coroutines may call the transport again.
//
// Teardown order: destroy every Job (and every Co outside a Job) before the objects its coroutines use: the Engine,
// the RPC clients, the transport. Destroying a suspended Co only runs its frames' destructors and resumes nothing,
// so it is always safe while those objects still exist. The other way round is not: a transport destroyed first
// drops its promises, and each dropped Promise resumes its waiter with CancelledError inside the destructor, so the
// resumed coroutine runs on objects that are being torn down. An owner (the glue, the test runner) therefore holds
// the transport and the Engine, and destroys its Jobs first.
#pragma once

#include "Scfg/Err.h"
#include "Scfg/Result.h"

#include <coroutine>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace scfg {

template <class T>
class Co;

namespace co_detail {

// Hands control to the coroutine awaiting this one (symmetric transfer), or back to whoever resumed it.
struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
        P& p = h.promise();
        if (!p.result) detail::programming_error("a coroutine reached its end without co_return");
        std::coroutine_handle<> next = p.continuation;
        return next ? next : std::noop_coroutine();
    }
    void await_resume() const noexcept {}
};

// Default-constructible on purpose: no constructor that takes the coroutine's arguments (llvm#91983).
template <class T>
struct PromiseBase {
    std::optional<Result<T>> result;
    std::coroutine_handle<> continuation;

    std::suspend_never initial_suspend() const noexcept { return {}; }
    FinalAwaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() const noexcept { std::abort(); }
    // co_return value / co_return err / co_return result (Co<void>: co_return ok() / err / status).
    void return_value(Result<T> r) { result.emplace(std::move(r)); }

    Result<T> take_result() {
        if (!result) detail::programming_error("the result of a coroutine was taken twice");
        Result<T> r = std::move(*result);
        result.reset();
        return r;
    }
};

template <class T>
struct CoPromise : PromiseBase<T> {
    Co<T> get_return_object() noexcept;
};

}  // namespace co_detail

// A coroutine giving Result<T> (see the top of this file). [[nodiscard]]: dropping a Co cancels it.
template <class T>
class [[nodiscard]] Co {
public:
    using promise_type = co_detail::CoPromise<T>;

    // An empty Co (no frame).
    Co() = default;
    Co(const Co&) = delete;
    Co& operator=(const Co&) = delete;
    Co(Co&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    // Destroys this Co's own frame first (cancelling it).
    Co& operator=(Co&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }
    ~Co() { reset(); }

    // Holds a frame (false once moved from).
    bool valid() const { return static_cast<bool>(h_); }
    // Finished: its result is ready.
    bool done() const { return h_ && h_.done(); }
    // The result of a finished Co, for a caller outside coroutines; once. Not done() is a programming error.
    Result<T> take() {
        if (!done()) detail::programming_error("Co::take() of a coroutine that has not finished");
        return h_.promise().take_result();
    }

    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return h.done(); }
        void await_suspend(std::coroutine_handle<> awaiting) const noexcept { h.promise().continuation = awaiting; }
        Result<T> await_resume() const { return h.promise().take_result(); }
    };
    // `co_await f()` or `co_await std::move(c)`: the Co (a temporary, or c) still owns the frame while it is awaited.
    Awaiter operator co_await() && {
        if (!h_) detail::programming_error("co_await of an empty Co");
        return Awaiter{h_};
    }

private:
    friend struct co_detail::CoPromise<T>;
    explicit Co(std::coroutine_handle<promise_type> h) : h_(h) {}
    void reset() {
        if (h_) std::exchange(h_, {}).destroy();
    }

    std::coroutine_handle<promise_type> h_;
};

template <class T>
Co<T> co_detail::CoPromise<T>::get_return_object() noexcept {
    return Co<T>(std::coroutine_handle<CoPromise>::from_promise(*this));
}

namespace co_detail {

// What a Future and its Promise share: the result, and the one coroutine waiting for it.
template <class T>
struct FutureState {
    std::optional<Result<T>> result;
    std::coroutine_handle<> waiter;
};

}  // namespace co_detail

template <class T>
class Promise;

// The one-shot answer a Promise<T> completes, awaited once: `co_await std::move(f)` or `co_await post(...)`.
template <class T>
class [[nodiscard]] Future {
public:
    // An empty Future (awaiting it is a programming error).
    Future() = default;
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;

    // A Future that is already complete (a transport that answers at once, a test).
    static Future completed(Result<T> r) {
        auto s = std::make_shared<co_detail::FutureState<T>>();
        s->result.emplace(std::move(r));
        return Future(std::move(s));
    }

    // Its result has arrived (awaiting it will not suspend).
    bool done() const { return s_ && s_->result.has_value(); }

    struct Awaiter {
        std::shared_ptr<co_detail::FutureState<T>> s;
        Awaiter(std::shared_ptr<co_detail::FutureState<T>> state) : s(std::move(state)) {}
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&&) noexcept = default;
        Awaiter& operator=(Awaiter&&) noexcept = default;
        // A frame destroyed while it waits unregisters, so a later complete() resumes nothing.
        ~Awaiter() {
            if (s && !s->result) s->waiter = {};
        }
        bool await_ready() const noexcept { return s->result.has_value(); }
        void await_suspend(std::coroutine_handle<> h) const noexcept { s->waiter = h; }
        Result<T> await_resume() const { return std::move(*s->result); }
    };
    Awaiter operator co_await() && {
        if (!s_) detail::programming_error("co_await of an empty or already awaited Future");
        return Awaiter(std::move(s_));
    }

private:
    friend class Promise<T>;
    explicit Future(std::shared_ptr<co_detail::FutureState<T>> s) : s_(std::move(s)) {}

    std::shared_ptr<co_detail::FutureState<T>> s_;
};

// The completing side of a Future (held by a transport until the answer arrives).
template <class T>
class Promise {
public:
    Promise() : s_(std::make_shared<co_detail::FutureState<T>>()) {}
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&& other) noexcept : s_(std::move(other.s_)), given_(other.given_) {}
    // Cancels the Future this Promise held, if it was not completed.
    Promise& operator=(Promise&& other) noexcept {
        if (this != &other) {
            cancel();
            s_ = std::move(other.s_);
            given_ = other.given_;
        }
        return *this;
    }
    ~Promise() { cancel(); }

    // The Future this Promise completes; once.
    Future<T> get_future() {
        if (!s_ || given_) detail::programming_error("Promise::get_future() called twice or after a move");
        given_ = true;
        return Future<T>(s_);
    }

    // Stores the result and resumes the waiting coroutine, if any, before returning. Once.
    void complete(Result<T> r) {
        if (!s_) detail::programming_error("Promise::complete() of a moved-from Promise");
        if (s_->result) detail::programming_error("Promise::complete() called twice");
        // The resumed coroutine may destroy this Promise (or its owner): touch only the local copy afterwards.
        std::shared_ptr<co_detail::FutureState<T>> s = s_;
        s->result.emplace(std::move(r));
        if (std::coroutine_handle<> w = std::exchange(s->waiter, {})) w.resume();
    }

    // complete() was called (or the Promise was moved from).
    bool completed() const { return !s_ || s_->result.has_value(); }

private:
    void cancel() {
        if (s_ && !s_->result) complete(Err::make("CancelledError", "the request was dropped before it was answered"));
    }

    std::shared_ptr<co_detail::FutureState<T>> s_;
    bool given_ = false;
};

namespace co_detail {

struct JobFlags {
    bool in_callback = false;
    bool finished = false;
};

// Job's root: awaits the work, then reports it. A named function, not a lambda (ruling R8).
template <class T>
Co<void> job_root(Co<T> work, std::function<void(Result<T>)> on_done, std::shared_ptr<JobFlags> flags) {
    Result<T> r = co_await std::move(work);
    flags->in_callback = true;
    if (on_done) on_done(std::move(r));
    flags->in_callback = false;
    flags->finished = true;
    co_return ok();
}

}  // namespace co_detail

// Keeps a root coroutine alive for a caller outside coroutines (the driver, the tests, phase 2's worker), as
// Python's run_sync and the PumpWorker drive a generator. Destroying the Job cancels an unfinished root.
template <class T>
class Job {
public:
    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    Job(Job&&) noexcept = default;
    Job& operator=(Job&&) = delete;
    ~Job() {
        if (flags_ && flags_->in_callback) detail::programming_error("a Job destroyed by its own on_done");
    }

    // Runs work; on_done gets its result when it finishes, inside start() when it finishes without waiting,
    // otherwise inside the complete() that let it finish. on_done must not destroy or restart this Job (it may
    // start others); destroying it there is caught and aborts. Starting a Job that is still running is a
    // programming error; a finished one starts again.
    //
    // A callback that work calls while it runs (an Engine's on_error, the ErrorSink of reverify and restore, a
    // transport's own callbacks) runs inside one of work's frames. Destroying this Job there destroys the frame that
    // is executing: undefined behaviour, and nothing detects it. Such a callback records what it wants and the
    // owner acts on it after on_done (or from its own loop).
    void start(Co<T> work, std::function<void(Result<T>)> on_done) {
        if (flags_ && (flags_->in_callback || !flags_->finished))
            detail::programming_error("Job::start() of a Job that is still running");
        if (!work.valid()) detail::programming_error("Job::start() of an empty Co");
        flags_ = std::make_shared<co_detail::JobFlags>();
        root_ = Co<void>();
        root_ = co_detail::job_root<T>(std::move(work), std::move(on_done), flags_);
    }

    // on_done has run.
    bool done() const { return flags_ && flags_->finished; }

private:
    std::shared_ptr<co_detail::JobFlags> flags_;
    Co<void> root_;
};

}  // namespace scfg
