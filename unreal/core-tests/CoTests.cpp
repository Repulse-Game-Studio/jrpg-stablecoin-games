// Copyright 2026 Repulse
// No Python twin: Co<T>, Future/Promise and Job (Co.h), the cooperative driver that replaces coio.run_sync. These
// pin what no vector reaches: eager start keeps loops of synchronous awaits flat, a deferred answer resumes the
// awaiting chain on the completer's stack, destroying a suspended Co is a clean cancellation, and errors cross
// levels as values. No coroutine here is a lambda (LintTests.NoCoroutineLambdas checks this file too).
#include "support/Test.h"

#include "Scfg/Co.h"
#include "Scfg/Err.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using scfg::Co;
using scfg::Err;
using scfg::Future;
using scfg::Job;
using scfg::Promise;
using scfg::Result;
using scfg::Status;

namespace scfg_test::co_tests {

// Futures for the coroutines below: completed at once, or queued until pump() (a transport whose answers arrive
// on a later tick).
struct Source {
    bool deferred = false;
    std::vector<Promise<int>> queue;
    std::vector<Result<int>> answers;

    Future<int> get(Result<int> answer) {
        if (!deferred) return Future<int>::completed(std::move(answer));
        Promise<int> p;
        Future<int> f = p.get_future();
        queue.push_back(std::move(p));
        answers.push_back(std::move(answer));
        return f;
    }
    // Completes what is queued now (not what the resumed coroutines queue meanwhile); returns how many.
    size_t pump() {
        std::vector<Promise<int>> q = std::move(queue);
        std::vector<Result<int>> a = std::move(answers);
        queue.clear();
        answers.clear();
        for (size_t i = 0; i < q.size(); ++i) q[i].complete(std::move(a[i]));
        return q.size();
    }
    void reset() {
        deferred = false;
        queue.clear();
        answers.clear();
    }
};

Source g_src;
int g_locals = 0;  // Local destructors run

struct Local {
    Local() = default;
    Local(const Local&) = delete;
    Local& operator=(const Local&) = delete;
    ~Local() { ++g_locals; }
};

Co<int> leaf(int x) {
    Local l;
    SCFG_CO_TRY(int v, co_await g_src.get(x));
    co_return v + 1;
}

Co<int64_t> sum_leaves(int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        SCFG_CO_TRY(int v, co_await leaf(i));
        sum += v;
    }
    co_return sum;
}

Co<int> outer() {
    Local l;
    SCFG_CO_TRY(int v, co_await leaf(1));
    co_return v * 10;
}

Co<int> nest(int depth) {
    if (depth == 0) co_return co_await g_src.get(0);
    SCFG_CO_TRY(int v, co_await nest(depth - 1));
    co_return v + 1;
}

Co<int> level3(bool fail) {
    if (fail) co_return Err::value_error("deep");
    SCFG_CO_TRY(int v, co_await g_src.get(3));
    co_return v;
}
Co<int> level2(bool fail) {
    SCFG_CO_TRY(int v, co_await level3(fail));
    co_return v + 20;
}
Co<int> level1(bool fail) {
    SCFG_CO_TRY(int v, co_await level2(fail));
    co_return v + 100;
}

Co<int> deferred_err() {
    SCFG_CO_TRY(int v, co_await g_src.get(Err::make("TransportError", "down")));
    co_return v;
}
Co<std::string> wraps_deferred_err() {
    Result<int> r = co_await deferred_err();
    if (r.ok()) co_return std::string("no error");
    co_return "caught " + r.err().kind;
}

Co<int> pass_through(Result<int> r) { co_return r; }

Co<int> await_future(Future<int> f) { co_return co_await std::move(f); }

Co<int> await_two(Future<int> a, Future<int> b) {
    SCFG_CO_TRY(int x, co_await std::move(a));
    SCFG_CO_TRY(int y, co_await std::move(b));
    co_return x * 10 + y;
}

Co<void> step(bool fail) {
    if (fail) co_return Err::make("RpcUnavailable", "all endpoints failed for m");
    SCFG_CO_TRY_VOID(co_await g_src.get(1));
    co_return scfg::ok();
}
Co<int> steps(bool fail) {
    SCFG_CO_TRY_VOID(co_await step(false));
    SCFG_CO_TRY_VOID(co_await step(fail));
    co_return 2;
}
Co<int> checks(Status s) {
    SCFG_CO_TRY_VOID(s);
    co_return 1;
}

Co<std::string> text(bool long_text) {
    if (long_text) co_return std::string(100, 'x');
    co_return "short";
}

// A finished Co's result (the tests' run_sync).
template <class T>
Result<T> finished(Co<T> c) {
    if (!c.done()) return Err::make("RuntimeError", "the coroutine did not finish");
    return c.take();
}

}  // namespace scfg_test::co_tests

using namespace scfg_test::co_tests;

SCFG_TEST(CoTests, SynchronousAwaitsKeepTheStackFlat) {
    g_src.reset();
    // 200 000 sequential awaits, each of a callee that finished without waiting: an eager start means no
    // suspension and no resume, so this finishes inside the call and the stack stays flat (asan's too).
    Co<int64_t> c = sum_leaves(200000);
    if (!SCFG_CHECK(t, c.done())) return;
    Result<int64_t> r = c.take();
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value(), int64_t{200000} * 200001 / 2);
    SCFG_CHECK(t, g_src.queue.empty());
}

SCFG_TEST(CoTests, ADeferredChainCompletesOnlyAfterNPumps) {
    g_src.reset();
    g_src.deferred = true;
    Co<int64_t> c = sum_leaves(1000);
    int pumps = 0;
    while (!c.done()) {
        if (!t.check(g_src.pump() == 1, "one request at a time")) return;
        ++pumps;
    }
    SCFG_CHECK_EQ(t, pumps, 1000);
    Result<int64_t> r = c.take();
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value(), int64_t{1000} * 1001 / 2);
    // 100 000 pumps: every resume starts on the pump's stack and returns to it.
    Co<int64_t> many = sum_leaves(100000);
    pumps = 0;
    while (!many.done() && g_src.pump() == 1) ++pumps;
    SCFG_CHECK_EQ(t, pumps, 100000);
    SCFG_CHECK(t, many.done());
    g_src.reset();
}

SCFG_TEST(CoTests, ACompletionResumesTheWholeChainOnTheCompletersStack) {
    g_src.reset();
    g_src.deferred = true;
    // 1000 nested coroutines waiting on one answer: complete() runs them all to the end before it returns, each
    // finished callee handing over to its caller by symmetric transfer.
    Co<int> c = nest(1000);
    SCFG_CHECK(t, !c.done());
    SCFG_CHECK_EQ(t, g_src.queue.size(), 1);
    g_src.pump();
    if (!SCFG_CHECK(t, c.done())) return;
    Result<int> r = c.take();
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value(), 1000);
    g_src.reset();
}

SCFG_TEST(CoTests, DestroyingASuspendedCoCancelsIt) {
    g_src.reset();
    g_src.deferred = true;
    g_locals = 0;
    {
        Co<int> c = outer();
        SCFG_CHECK(t, !c.done());
        SCFG_CHECK_EQ(t, g_src.queue.size(), 1);
        SCFG_CHECK_EQ(t, g_locals, 0);
    }
    // Both frames are gone, their locals destroyed (outer's and the leaf's it was waiting in).
    SCFG_CHECK_EQ(t, g_locals, 2);
    // The answer arriving afterwards resumes nothing (asan reports any touch of a freed frame).
    SCFG_CHECK_EQ(t, g_src.pump(), 1);
    SCFG_CHECK_EQ(t, g_locals, 2);

    // Mid-loop: the frames of finished leaves are already gone; the waiting one goes with the root.
    g_locals = 0;
    {
        Co<int64_t> c = sum_leaves(5);
        g_src.pump();
        g_src.pump();
        SCFG_CHECK_EQ(t, g_locals, 2);
    }
    SCFG_CHECK_EQ(t, g_locals, 3);
    g_src.pump();
    SCFG_CHECK_EQ(t, g_locals, 3);
    // A finished Co is destroyed the same way.
    g_src.deferred = false;
    g_locals = 0;
    { Co<int> c = outer(); }
    SCFG_CHECK_EQ(t, g_locals, 2);
    g_src.reset();
}

SCFG_TEST(CoTests, AnErrCrossesThreeLevels) {
    g_src.reset();
    Result<int> fine = finished(level1(false));
    if (SCFG_CHECK(t, fine.ok())) SCFG_CHECK_EQ(t, fine.value(), 123);
    Result<int> r = finished(level1(true));
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "ValueError");
        SCFG_CHECK_EQ(t, r.err().message, "deep");
    }
    // The same through a deferred answer: the Err arrives from complete(), three levels down.
    g_src.deferred = true;
    Co<std::string> c = wraps_deferred_err();
    SCFG_CHECK(t, !c.done());
    g_src.pump();
    Result<std::string> w = finished(std::move(c));
    if (SCFG_CHECK(t, w.ok())) SCFG_CHECK_EQ(t, w.value(), "caught TransportError");
    g_src.reset();
}

SCFG_TEST(CoTests, CoReturnTakesAValueAnErrOrAResult) {
    Result<int> e = finished(pass_through(Result<int>(Err::key_error("'x'"))));
    if (SCFG_CHECK(t, !e.ok())) SCFG_CHECK_EQ(t, e.err().kind, "KeyError");
    Result<int> v = finished(pass_through(Result<int>(7)));
    if (SCFG_CHECK(t, v.ok())) SCFG_CHECK_EQ(t, v.value(), 7);
    Result<std::string> s = finished(text(false));
    if (SCFG_CHECK(t, s.ok())) SCFG_CHECK_EQ(t, s.value(), "short");
    Result<std::string> l = finished(text(true));
    if (SCFG_CHECK(t, l.ok())) SCFG_CHECK_EQ(t, l.value().size(), 100);
}

SCFG_TEST(CoTests, CoTryVoidAndCoVoid) {
    g_src.reset();
    Result<int> fine = finished(steps(false));
    if (SCFG_CHECK(t, fine.ok())) SCFG_CHECK_EQ(t, fine.value(), 2);
    Result<int> bad = finished(steps(true));
    if (SCFG_CHECK(t, !bad.ok())) SCFG_CHECK_EQ(t, bad.err().kind, "RpcUnavailable");
    Status s = finished(step(false));
    SCFG_CHECK(t, s.ok());
    Result<int> passed = finished(checks(scfg::ok()));
    if (SCFG_CHECK(t, passed.ok())) SCFG_CHECK_EQ(t, passed.value(), 1);
    Result<int> failed = finished(checks(Err::type_error("t")));
    if (SCFG_CHECK(t, !failed.ok())) SCFG_CHECK_EQ(t, failed.err().message, "t");
    // Co<void> through deferred answers.
    g_src.deferred = true;
    Co<int> c = steps(false);
    SCFG_CHECK_EQ(t, g_src.pump(), 1);
    SCFG_CHECK(t, !c.done());
    SCFG_CHECK_EQ(t, g_src.pump(), 1);
    SCFG_CHECK(t, c.done());
    g_src.reset();
}

SCFG_TEST(CoTests, APromiseDroppedIncompleteCancelsItsFuture) {
    // A waiter resumes with CancelledError, a BaseException that `except Exception` does not catch.
    std::optional<Promise<int>> p(std::in_place);
    Co<int> c = await_future(p->get_future());
    SCFG_CHECK(t, !c.done());
    p.reset();
    if (!SCFG_CHECK(t, c.done())) return;
    Result<int> r = c.take();
    if (SCFG_CHECK(t, !r.ok())) {
        SCFG_CHECK_EQ(t, r.err().kind, "CancelledError");
        SCFG_CHECK(t, r.err().is_a("BaseException"));
        SCFG_CHECK(t, !r.err().is_a("Exception"));
    }
    // Dropped before anyone waits: the future is done, with the same error.
    Future<int> f;
    {
        Promise<int> q;
        f = q.get_future();
        SCFG_CHECK(t, !f.done());
    }
    SCFG_CHECK(t, f.done());
    Result<int> later = finished(await_future(std::move(f)));
    if (SCFG_CHECK(t, !later.ok())) SCFG_CHECK_EQ(t, later.err().kind, "CancelledError");
    // A moved-from Promise cancels nothing; the moved-to one still completes.
    Promise<int> a;
    Co<int> w = await_future(a.get_future());
    Promise<int> b = std::move(a);
    SCFG_CHECK(t, !w.done());
    b.complete(5);
    Result<int> got = finished(std::move(w));
    if (SCFG_CHECK(t, got.ok())) SCFG_CHECK_EQ(t, got.value(), 5);
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

// orphaned(): nobody can see the answer any more (the Future and its waiter are gone), which a transport checks to
// cancel such a request; release(): let go without completing, so the waiter is not resumed (it stays suspended).
SCFG_TEST(CoTests, AnOrphanedPromiseAndARelease) {
    Promise<int> p;
    SCFG_CHECK(t, !p.orphaned());  // no Future handed out yet
    std::optional<Co<int>> waiter(std::in_place, await_future(p.get_future()));
    SCFG_CHECK(t, !p.orphaned());  // awaited
    waiter.reset();                // the waiting frame is destroyed: it unregisters
    SCFG_CHECK(t, p.orphaned());
    p.complete(1);  // resumes nothing
    SCFG_CHECK(t, !p.orphaned() && p.completed());
    {
        Promise<int> q;
        Future<int> held = q.get_future();
        SCFG_CHECK(t, !q.orphaned());  // somebody still holds the Future
    }
    Promise<int> r;
    Co<int> stays = await_future(r.get_future());
    r.release();
    SCFG_CHECK(t, r.completed() && !r.orphaned());
    {
        Promise<int> dropped = std::move(r);  // a released Promise cancels nothing when dropped
    }
    SCFG_CHECK(t, !stays.done());  // still suspended; destroying it is the cancellation
}

SCFG_TEST(CoTests, CompletingBeforeTheAwaitIsFine) {
    Promise<int> p;
    Future<int> f = p.get_future();
    p.complete(41);
    SCFG_CHECK(t, f.done());
    Co<int> c = await_future(std::move(f));
    if (!SCFG_CHECK(t, c.done())) return;
    Result<int> r = c.take();
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value(), 41);
    // Two futures completed in the other order: the second's answer waits for its await.
    Promise<int> a, b;
    Co<int> both = await_two(a.get_future(), b.get_future());
    b.complete(2);
    SCFG_CHECK(t, !both.done());
    a.complete(1);
    Result<int> ab = finished(std::move(both));
    if (SCFG_CHECK(t, ab.ok())) SCFG_CHECK_EQ(t, ab.value(), 12);
    Result<int> ready = finished(await_future(Future<int>::completed(Err::value_error("v"))));
    if (SCFG_CHECK(t, !ready.ok())) SCFG_CHECK_EQ(t, ready.err().message, "v");
}

SCFG_TEST(CoTests, JobCallsBackWhenTheRootFinishes) {
    g_src.reset();
    // Synchronously, inside start(), when the root finishes without waiting.
    Job<int> job;
    SCFG_CHECK(t, !job.done());
    int calls = 0;
    std::optional<Result<int>> got;
    job.start(level1(false), [&calls, &got](Result<int> r) {
        ++calls;
        got.emplace(std::move(r));
    });
    SCFG_CHECK_EQ(t, calls, 1);
    SCFG_CHECK(t, job.done());
    if (SCFG_CHECK(t, got && got->ok())) SCFG_CHECK_EQ(t, got->value(), 123);

    // Later, from the pump that completes the last answer; an error arrives the same way.
    g_src.deferred = true;
    Job<int64_t> later;
    std::optional<Result<int64_t>> sum;
    later.start(sum_leaves(3), [&sum](Result<int64_t> r) { sum.emplace(std::move(r)); });
    SCFG_CHECK(t, !later.done());
    g_src.pump();
    g_src.pump();
    SCFG_CHECK(t, !sum.has_value());
    g_src.pump();
    SCFG_CHECK(t, later.done());
    if (SCFG_CHECK(t, sum && sum->ok())) SCFG_CHECK_EQ(t, sum->value(), 6);

    Job<void> failing;
    std::string kind;
    failing.start(step(true), [&kind](Status s) { kind = s.ok() ? "ok" : s.err().kind; });
    SCFG_CHECK_EQ(t, kind, "RpcUnavailable");

    // A finished Job starts again.
    calls = 0;
    job.start(level1(true), [&calls, &got](Result<int> r) {
        ++calls;
        got.emplace(std::move(r));
    });
    g_src.pump();
    SCFG_CHECK_EQ(t, calls, 1);
    if (SCFG_CHECK(t, got && !got->ok())) SCFG_CHECK_EQ(t, got->err().message, "deep");
    g_src.reset();
}

SCFG_TEST(CoTests, DestroyingAJobCancelsItsRoot) {
    g_src.reset();
    g_src.deferred = true;
    g_locals = 0;
    int calls = 0;
    {
        Job<int> job;
        job.start(outer(), [&calls](Result<int>) { ++calls; });
        SCFG_CHECK(t, !job.done());
    }
    SCFG_CHECK_EQ(t, g_locals, 2);
    g_src.pump();
    SCFG_CHECK_EQ(t, calls, 0);
    g_src.reset();
}

SCFG_TEST(CoTests, CoIsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<Co<int>> && !std::is_copy_assignable_v<Co<int>>);
    static_assert(std::is_nothrow_move_constructible_v<Co<int>> && std::is_nothrow_move_assignable_v<Co<int>>);
    static_assert(!std::is_copy_constructible_v<Promise<int>> && !std::is_copy_constructible_v<Future<int>>);
    static_assert(!std::is_copy_constructible_v<Job<int>>);
    g_src.reset();
    g_src.deferred = true;
    g_locals = 0;
    Co<int> a = outer();
    Co<int> b = std::move(a);
    SCFG_CHECK(t, !a.valid());
    SCFG_CHECK(t, b.valid() && !b.done());
    b = leaf(5);  // the waiting outer() is destroyed by the assignment
    SCFG_CHECK_EQ(t, g_locals, 2);
    SCFG_CHECK_EQ(t, g_src.pump(), 2);
    Result<int> r = finished(std::move(b));
    if (SCFG_CHECK(t, r.ok())) SCFG_CHECK_EQ(t, r.value(), 6);
    g_src.reset();
}
