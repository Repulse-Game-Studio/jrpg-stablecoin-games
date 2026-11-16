// Twin of tests/unit/test_worker.py through rpgmaker/src/core/worker.test.ts (every test, under its name in
// PascalCase), plus the cases of godot/tests/test_worker.gd the TypeScript file lacks (a failing reverify, the
// done callbacks as the `finished` signal and `wait()`, a joining RunOnce that sees the chain's failure), the C#
// specifics (Job's Future API, Commands, a command's synchronous throw, the real Engine behind the default hooks)
// and the worker on one thread under a queueing SynchronizationContext.
//
// The fakes complete synchronously, so a run that awaits nothing incomplete is over when RunOnce returns; a Gate
// keeps a tick or a command in flight until the test opens it (its continuations then run inline, on this thread).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class WorkerTests
    {
        private static Func<Engine, Task<object?>> Value(object? v) => _ => Task.FromResult(v);

        private static void Completed(Task t)
        {
            Assert.That(t.IsCompleted, Is.True, "the run is over");
            Assert.That(t.IsFaulted, Is.False, "the run succeeded: " + t.Exception);
        }

        private static Exception Faulted(Task t)
        {
            Assert.That(t.IsFaulted, Is.True, "the run failed");
            return t.Exception!.InnerExceptions.Single();
        }

        // --- test_worker.py ---------------------------------------------------------------------------------

        [Test]
        public void InitialSnapshotTakenAtConstruction()
        {
            var w = new FakeEngine().Worker();
            Assert.That(w.Snapshot.Owned, Is.EqualTo(new[] { "tick0" }));
        }

        [Test]
        public void RunOnceReverifiesOnlyFirstTimeAndPublishesSnapshot()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            Completed(w.RunOnce());
            Completed(w.RunOnce());
            Assert.That(e.Reverified, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(2));
            Assert.That(w.Snapshot.Owned, Is.EqualTo(new[] { "tick2" }));
        }

        [Test]
        public void WaitingOutcomesWithoutErrorAreNotPublished()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            e.Statuses = new List<string> { "waiting", "confirming", "paid" };
            Completed(w.RunOnce());
            Assert.That(w.DrainOutcomes().Select(o => o.Status), Is.EqualTo(new[] { "confirming", "paid" }));
            Assert.That(w.DrainOutcomes(), Is.Empty);
        }

        [Test]
        public void WaitingOutcomeWithErrorIsPublished()
        {
            var e = new FakeEngine
            {
                TickImpl = () => Task.FromResult(new List<StepOutcome>
                {
                    new StepOutcome("ps_1", "waiting", error: "network_unavailable"),
                    new StepOutcome("ps_2", "waiting", error: ""), // Python's `o.error` is falsy for ""
                }),
            };
            var w = e.Worker();
            Completed(w.RunOnce());
            var got = w.DrainOutcomes();
            Assert.That(got.Select(o => o.SessionId), Is.EqualTo(new[] { "ps_1" }));
            Assert.That(got[0].Error, Is.EqualTo("network_unavailable"));
        }

        [Test]
        public void SubmittedCommandsRunBeforeTickAndResolveJobs()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            var ok = w.Submit(Commands.Sync(_ => e.Ticks));
            var bad = w.Submit(_ => throw new ArithmeticError("division by zero")); // Python's 1 / 0
            Assert.That(ok.Done(), Is.False);
            Completed(w.RunOnce());
            Assert.That(ok.Result(), Is.EqualTo(0)); // ran before the tick
            Assert.That(bad.Done(), Is.True);
            Assert.Throws<ArithmeticError>(() => bad.Result());
        }

        [Test]
        public void ACommandSubmittedByACommandRunsBeforeTheSameTick()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            var inner = new List<Job>();
            w.Submit(Commands.Sync(_ => { inner.Add(w.Submit(Commands.Sync(_ => e.Ticks))); }));
            Completed(w.RunOnce());
            Assert.That(inner, Has.Count.EqualTo(1));
            Assert.That(inner[0].Result(), Is.EqualTo(0)); // drained in the same pass, as in Python
            Assert.That(e.Ticks, Is.EqualTo(2), "the inner submit's wake still adds its one follow-up pass");
        }

        [Test]
        public void CancelledJobIsSkipped()
        {
            var w = new FakeEngine().Worker();
            var ran = new List<int>();
            var job = w.Submit(Commands.Sync(_ => ran.Add(1)));
            Assert.That(job.Cancel(), Is.True);
            Completed(w.RunOnce());
            Assert.That(ran, Is.Empty);
            Assert.Throws<CancelledError>(() => job.Result());
        }

        [Test]
        public void TimerLoopWakeAndStop()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 60);
            w.Start();
            Assert.That(w.Running, Is.True);
            Assert.That(t.Pending!.Seconds, Is.EqualTo(0)); // first run at once
            t.Fire();
            Assert.That(e.Ticks, Is.EqualTo(1));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(60)); // long interval: only Wake() can trigger tick 2
            var job = w.Submit(Value("done")); // submit wakes the loop
            Assert.That(t.Pending!.Seconds, Is.EqualTo(0));
            t.Fire();
            Assert.That(job.Result(), Is.EqualTo("done"));
            Assert.That(e.Ticks, Is.EqualTo(2));
            Assert.That(w.Running, Is.True);
            w.Stop();
            Assert.That(w.Running, Is.False);
            Assert.That(t.Pending, Is.Null);
        }

        [Test]
        public void LoopSurvivesExceptions()
        {
            var e = new FakeEngine();
            var errors = new List<Exception>();
            int n = 0;
            e.TickImpl = () =>
            {
                n++;
                if (n == 1) throw new RuntimeError("boom");
                return Task.FromResult(new List<StepOutcome>());
            };
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 0.01, errors.Add);
            w.Start();
            t.Fire();
            Assert.That(errors, Has.Count.EqualTo(1));
            Assert.That(errors[0], Is.InstanceOf<RuntimeError>());
            Assert.That(errors[0].Message, Does.Contain("boom"));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(0.01));
            t.Fire(); // the loop rescheduled itself
            Assert.That(n, Is.EqualTo(2));
            Assert.That(errors, Has.Count.EqualTo(1));
            w.Stop();
        }

        [Test]
        public void AsyncCommandsAreAwaited()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            var gate = new Gate();
            var job = w.Submit(async _ =>
            {
                await gate.Wait();
                return 42;
            });
            var run = w.RunOnce();
            Assert.That(run.IsCompleted, Is.False);
            Assert.That(job.Running(), Is.True);
            Assert.That(e.Ticks, Is.EqualTo(0), "the tick waits for the command");
            gate.Open();
            Completed(run);
            Assert.That(job.Result(), Is.EqualTo(42));
            Assert.That(e.Ticks, Is.EqualTo(1));
        }

        // --- test_pump_worker.py: the semantics that are not frame pumping ------------------------------------

        [Test]
        public void PumpACommandSubmittedMidTickWaitsForTheTick()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            e.TickImpl = async () =>
            {
                await gate.Wait();
                return new List<StepOutcome>();
            };
            var w = e.Worker();
            var run = w.RunOnce();
            var job = w.Submit(Commands.Sync(_ =>
            {
                e.Log.Add("choose");
                return "chosen";
            }));
            Assert.That(job.Done(), Is.False);
            e.TickImpl = null;
            gate.Open();
            Completed(run);
            Assert.That(job.Result(), Is.EqualTo("chosen"));
            Assert.That(e.Log, Is.EqualTo(new[] { "reverify", "tick", "choose", "tick" }));
        }

        [Test]
        public void PumpCommandsRunBeforeADueTickAndPlainResultsResolve()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            var plain = w.Submit(Value("now"));
            w.Submit(Commands.Sync(_ => e.Log.Add("first")));
            Completed(w.RunOnce());
            Assert.That(plain.Result(), Is.EqualTo("now"));
            Assert.That(e.Log, Is.EqualTo(new[] { "first", "reverify", "tick" }));
        }

        [Test]
        public void PumpASubmitLandingMidRunGetsItsFollowUpRunAtOnceNotAnIntervalLater()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            e.TickImpl = async () =>
            {
                await gate.Wait();
                return new List<StepOutcome>();
            };
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 10);
            w.Start();
            t.Fire();
            var job = w.Submit(Value("hint"));
            Assert.That(t.Pending, Is.Null, "a submit mid-run marks a follow-up; it does not arm the timer");
            e.TickImpl = null;
            gate.Open();
            Assert.That(job.Result(), Is.EqualTo("hint"));
            Assert.That(e.Ticks, Is.EqualTo(2)); // the follow-up tick it asked for ran immediately
            Assert.That(t.Pending!.Seconds, Is.EqualTo(10)); // then the loop waits the interval
            w.Stop();
        }

        [Test]
        public void PumpARunWithoutAMidRunWakeWaitsTheInterval()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 10);
            w.Wake(); // not started: nothing to wake
            Assert.That(t.Pending, Is.Null);
            w.Start();
            t.Fire();
            Assert.That(e.Ticks, Is.EqualTo(1));
            Assert.That(t.Scheduled, Is.EqualTo(new[] { 0.0, 10.0 }));
            w.Stop();
        }

        [Test]
        public void PumpAFailingTickReportsAndSchedulesTheNextReverifyIsNotRetried()
        {
            var e = new FakeEngine();
            var errors = new List<Exception>();
            bool first = true;
            e.TickImpl = () =>
            {
                if (first)
                {
                    first = false;
                    throw new RuntimeError("down");
                }
                return Task.FromResult(new List<StepOutcome>());
            };
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 10, errors.Add);
            w.Start();
            t.Fire();
            Assert.That(errors, Has.Count.EqualTo(1));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(10));
            t.Fire();
            Assert.That(e.Reverified, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(2));
            w.Stop();
        }

        [Test]
        public void PumpStopCancelsQueuedCommandsAndRunsNothingMore()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t);
            w.Start();
            var queued = w.Submit(Value("later"));
            w.Stop();
            Assert.That(queued.Cancelled(), Is.True);
            Assert.That(w.Running, Is.False);
            Assert.That(t.Pending, Is.Null);
            Assert.That(t.RunDue(), Is.EqualTo(0));
            Assert.That(e.Ticks, Is.EqualTo(0));
        }

        // --- ordering and follow-ups --------------------------------------------------------------------------

        [Test]
        public void CommandsNeverInterleaveWithATick()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            bool inTick = false;
            int ticks = 0;
            e.TickImpl = async () =>
            {
                ticks++;
                if (ticks == 1)
                {
                    inTick = true;
                    await gate.Wait();
                    inTick = false;
                }
                return new List<StepOutcome>();
            };
            var w = e.Worker();
            var run = w.RunOnce();
            Assert.That(inTick, Is.True);
            bool? ranDuringTick = null;
            var job = w.Submit(Commands.Sync(_ =>
            {
                ranDuringTick = inTick;
                e.Log.Add("cmd");
                return 1;
            }));
            Assert.That(job.Done(), Is.False); // not executed while the tick awaits the network
            gate.Open();
            Completed(run);
            Assert.That(job.Result(), Is.EqualTo(1));
            Assert.That(ranDuringTick, Is.False);
            // the command ran in the NEXT pass: tick (1st pass) ... cmd, then tick (2nd pass)
            Assert.That(e.Log, Is.EqualTo(new[] { "reverify", "tick", "cmd", "tick" }));
            Assert.That(e.Reverified, Is.EqualTo(1));
        }

        [Test]
        public void WakeDuringARunSchedulesExactlyOneFollowUp()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            e.TickImpl = async () =>
            {
                if (e.Ticks == 1) await gate.Wait();
                return new List<StepOutcome>();
            };
            var w = e.Worker();
            var run = w.RunOnce();
            w.Wake();
            w.Wake();
            var joined = w.RunOnce();
            Assert.That(joined, Is.SameAs(run), "a RunOnce during a run joins its chain");
            gate.Open();
            Completed(run);
            Assert.That(e.Ticks, Is.EqualTo(2));
        }

        [Test]
        public void FirstRunReverifiesOnce()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            Completed(w.RunOnce());
            Assert.That(e.Log, Is.EqualTo(new[] { "reverify", "tick" })); // reverify before the first tick
            Completed(w.RunOnce());
            Completed(w.RunOnce());
            Assert.That(e.Reverified, Is.EqualTo(1));
        }

        [Test]
        public void JobReportsException()
        {
            var w = new FakeEngine().Worker();
            var err = new TypeError("bad");
            var bad = w.Submit(_ => throw err);
            var good = w.Submit(Value(1));
            var pending = Assert.Throws<RuntimeError>(() => bad.Result());
            Assert.That(pending!.Message, Is.EqualTo("job is not done"));
            Completed(w.RunOnce());
            Assert.That(bad.Exception(), Is.SameAs(err));
            Assert.That(good.Exception(), Is.Null);
            Assert.That(Assert.Throws<TypeError>(() => bad.Result()), Is.SameAs(err));
            Assert.That(Assert.Throws<TypeError>(() => bad.Result()), Is.SameAs(err), "rethrown on every call");
        }

        [Test]
        public void JobUnobservedFailureRaisesNoUnobservedTaskException()
        {
            var seen = new List<Exception>();
            void On(object? sender, UnobservedTaskExceptionEventArgs args)
            {
                foreach (var x in args.Exception!.Flatten().InnerExceptions)
                    if (x.Message == "never observed" || x is CancelledError) seen.Add(x);
            }
            TaskScheduler.UnobservedTaskException += On;
            try
            {
                RunAndForget();
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
            }
            finally
            {
                TaskScheduler.UnobservedTaskException -= On;
            }
            Assert.That(seen, Is.Empty);

            static void RunAndForget()
            {
                var w = new FakeEngine().Worker();
                w.Submit(_ => throw new RuntimeError("never observed"));
                w.Submit(async _ =>
                {
                    await Task.FromResult(0);
                    throw new RuntimeError("never observed");
                });
                var c = w.Submit(Value(1));
                c.Cancel();
                Completed(w.RunOnce());
            }
        }

        [Test]
        public void StopCancelsTheTimer()
        {
            var t = new FakeScheduler();
            var w = new FakeEngine().Worker(t);
            w.Start();
            Assert.That(t.Pending, Is.Not.Null);
            w.Stop();
            Assert.That(t.CancelCount, Is.EqualTo(1));
            Assert.That(t.Pending, Is.Null);
            Assert.That(w.Running, Is.False);
        }

        [Test]
        public void NoRunAfterStopEvenIfARunIsInFlight()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            e.TickImpl = async () =>
            {
                await gate.Wait();
                return new List<StepOutcome>();
            };
            var t = new FakeScheduler();
            var w = e.Worker(t);
            w.Start();
            t.Fire();
            w.Wake(); // would normally add a follow-up
            w.Stop();
            gate.Open();
            Assert.That(e.Ticks, Is.EqualTo(1));
            Assert.That(t.Pending, Is.Null);
        }

        [Test]
        public void JobResultThrowsWhilePending()
        {
            var j = new Job();
            Assert.That(Assert.Throws<RuntimeError>(() => j.Result())!.Message, Is.EqualTo("job is not done"));
            Assert.That(j.Exception(), Is.Null);
            Assert.That(j.Done(), Is.False);
        }

        /// <summary>A tick that stays in flight until the test opens its gate; counts the ticks running at once.</summary>
        private sealed class YieldingTicks
        {
            public int Live, MaxLive;
            public readonly List<Gate> Gates = new List<Gate>();

            public async Task<List<StepOutcome>> Tick()
            {
                Live++;
                MaxLive = Math.Max(MaxLive, Live);
                var g = new Gate();
                Gates.Add(g);
                await g.Wait();
                Live--;
                return new List<StepOutcome>();
            }

            /// <summary>Opens the gates one at a time until <paramref name="run"/> is over.</summary>
            public void Drive(Task run)
            {
                int opened = 0;
                while (!run.IsCompleted)
                {
                    Assert.That(Gates.Count, Is.GreaterThan(opened), "the run waits on nothing the test can open");
                    Gates[opened++].Open();
                }
            }
        }

        [Test]
        public void ReentrantRunOnceFromACommandNeverOverlapsTicks()
        {
            var e = new FakeEngine();
            var y = new YieldingTicks();
            e.TickImpl = y.Tick;
            var w = e.Worker();
            Task? inner = null;
            w.Submit(Commands.Sync(_ => { inner = w.RunOnce(); }));
            var run = w.RunOnce();
            y.Drive(run);
            Completed(run);
            Assert.That(inner, Is.SameAs(run), "the in-flight state was set before the command ran");
            Assert.That(y.MaxLive, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(2)); // exactly one follow-up
        }

        [Test]
        public void WakeAndSubmitFromACommandGiveOneFollowUp()
        {
            var e = new FakeEngine();
            var y = new YieldingTicks();
            e.TickImpl = y.Tick;
            var w = e.Worker();
            Job? inner = null;
            w.Submit(Commands.Sync(_ =>
            {
                w.Wake();
                inner = w.Submit(Value("inner"));
            }));
            var run = w.RunOnce();
            y.Drive(run);
            Completed(run);
            Assert.That(y.MaxLive, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(2));
            Assert.That(inner!.Result(), Is.EqualTo("inner"));
            Assert.That(e.Log.Count(x => x == "tick"), Is.EqualTo(2));
        }

        [Test]
        public void CancelledJobExceptionIsTheCancelledError()
        {
            var j = new Job();
            j.Cancel();
            Assert.That(j.Exception(), Is.InstanceOf<CancelledError>());
            Assert.That(j.Exception()!.Message, Is.EqualTo("job cancelled"));
            Assert.That(Assert.Throws<CancelledError>(() => j.Result()), Is.SameAs(j.Exception()));
            Assert.That(j.Cancelled(), Is.True);
        }

        [Test]
        public void SubmitOnAStoppedWorkerIsCancelledButBeforeStartItQueues()
        {
            var t = new FakeScheduler();
            var w = new FakeEngine().Worker(t);
            var early = w.Submit(Value("queued")); // never started: queues
            Assert.That(early.Done(), Is.False);
            Assert.That(t.Pending, Is.Null, "before Start a submit arms no timer");
            w.Stop();
            Assert.That(early.Cancelled(), Is.True);
            var late = w.Submit(Value("never"));
            Assert.That(late.Cancelled(), Is.True);
            Assert.That(late.Exception(), Is.InstanceOf<CancelledError>());
            w.Start(); // a restart accepts work again
            var again = w.Submit(Value("ok"));
            Assert.That(again.Done(), Is.False);
            w.Stop();
        }

        [Test]
        public void TimerFiringDuringAWakeTriggeredRunYieldsOneFollowUp()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            int live = 0, maxLive = 0;
            e.TickImpl = async () =>
            {
                live++;
                maxLive = Math.Max(maxLive, live);
                if (e.Ticks == 1) await gate.Wait();
                live--;
                return new List<StepOutcome>();
            };
            var t = new FakeScheduler();
            var w = e.Worker(t, () => 10);
            w.Start(); // timer armed at 0
            var run = w.RunOnce(); // a run in flight, started outside the timer
            w.Wake(); // asks for a follow-up
            t.Fire(); // the timer fires mid-run: joins the same chain
            gate.Open();
            Completed(run);
            Assert.That(maxLive, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(2));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(10), "the timer's run re-armed once the chain was over");
            w.Stop();
        }

        // --- test_worker.gd extras --------------------------------------------------------------------------

        [Test]
        public void AFailingReverifyEndsThePassAndIsNotRetried()
        {
            var e = new FakeEngine { ReverifyImpl = () => throw new RuntimeError("no chain") };
            var w = e.Worker();
            Assert.That(Faulted(w.RunOnce()), Is.InstanceOf<RuntimeError>());
            Assert.That(e.Ticks, Is.EqualTo(0));
            Assert.That(w.Snapshot.Owned, Is.EqualTo(new[] { "tick0" }));
            Completed(w.RunOnce());
            Assert.That(e.Reverified, Is.EqualTo(1));
            Assert.That(e.Ticks, Is.EqualTo(1));
        }

        [Test]
        public void JobDoneCallbacksFireOncePerJob()
        {
            var w = new FakeEngine().Worker();
            var seen = new List<string>();
            var a = w.Submit(Value(1));
            var b = w.Submit(Value(2));
            a.AddDoneCallback(_ => seen.Add("a"));
            b.AddDoneCallback(_ => seen.Add("b"));
            b.Cancel();
            Assert.That(seen, Is.EqualTo(new[] { "b" }));
            Completed(w.RunOnce());
            Assert.That(seen, Is.EqualTo(new[] { "b", "a" }));
            Assert.That(a.Cancel(), Is.False); // finished: too late
            Assert.That(b.Cancel(), Is.True); // already cancelled
            Assert.That(seen, Is.EqualTo(new[] { "b", "a" }), "a second Cancel fires nothing");
        }

        [Test]
        public void AJoiningRunOnceSeesTheChainsFirstFailure()
        {
            var e = new FakeEngine();
            var gate = new Gate();
            e.TickImpl = async () =>
            {
                await gate.Wait();
                throw new RuntimeError("first");
            };
            var w = e.Worker();
            var run = w.RunOnce();
            e.TickImpl = () => throw new RuntimeError("second");
            var joined = w.RunOnce();
            gate.Open();
            Assert.That(Faulted(joined).Message, Is.EqualTo("first"));
            Assert.That(Faulted(run).Message, Is.EqualTo("first"));
            Assert.That(e.Ticks, Is.EqualTo(2));
        }

        [Test]
        public void ADoneCallbackOnADoneJobRunsAtOnce()
        {
            var w = new FakeEngine().Worker();
            var err = new TypeError("bad");
            var good = w.Submit(Value(7));
            var bad = w.Submit(_ => throw err);
            var gone = w.Submit(Value(1));
            gone.Cancel();
            Completed(w.RunOnce());
            var seen = new List<Job>();
            foreach (var j in new[] { good, bad, gone }) j.AddDoneCallback(seen.Add);
            Assert.That(seen, Is.EqualTo(new[] { good, bad, gone }));
            Assert.That(good.Result(), Is.EqualTo(7));
            Assert.That(bad.Exception(), Is.SameAs(err));
            Assert.That(gone.Exception(), Is.InstanceOf<CancelledError>());
        }

        [Test]
        public void ADoneCallbackOnAPendingJobRunsWhenItSettles()
        {
            var w = new FakeEngine().Worker();
            var job = w.Submit(Value(5));
            var seen = new List<object?>();
            job.AddDoneCallback(j => seen.Add(j.Result()));
            Assert.That(seen, Is.Empty, "pending: still waiting");
            Completed(w.RunOnce());
            Assert.That(seen, Is.EqualTo(new object?[] { 5 }));
            var cancelled = w.Submit(Value(1));
            var seen2 = new List<Exception?>();
            cancelled.AddDoneCallback(j => seen2.Add(j.Exception()));
            cancelled.Cancel();
            Assert.That(seen2, Has.Count.EqualTo(1), "a cancel runs the callback");
            Assert.That(seen2[0], Is.InstanceOf<CancelledError>());
        }

        // --- C# specifics: Job ---------------------------------------------------------------------------------

        [Test]
        public void TheFutureApiFollowsPython()
        {
            var j = new Job();
            Assert.That(j.Running(), Is.False);
            Assert.That(j.SetRunningOrNotifyCancel(), Is.True);
            Assert.That(j.Running(), Is.True);
            Assert.That(j.Done(), Is.False);
            Assert.That(j.Cancel(), Is.False, "a running job cannot be cancelled");
            Assert.That(Assert.Throws<RuntimeError>(() => j.SetRunningOrNotifyCancel())!.Message, Is.EqualTo("job in unexpected state"));
            j.SetResult("v");
            Assert.That(j.Running(), Is.False);
            Assert.That(j.Done(), Is.True);
            Assert.That(j.Result(), Is.EqualTo("v"));
            Assert.Throws<InvalidStateError>(() => j.SetResult("w"));
            Assert.Throws<InvalidStateError>(() => j.SetException(new ValueError("x")));
            Assert.That(j.Result(), Is.EqualTo("v"));
            Assert.Throws<RuntimeError>(() => j.SetRunningOrNotifyCancel());

            var c = new Job();
            c.Cancel();
            Assert.That(c.SetRunningOrNotifyCancel(), Is.False, "a cancelled job is not started");
            Assert.That(c.SetRunningOrNotifyCancel(), Is.False);
            Assert.Throws<InvalidStateError>(() => c.SetResult(1));
            Assert.That(c.Cancelled(), Is.True);

            var p = new Job(); // Python lets a pending future be completed without set_running_or_notify_cancel
            var boom = new ValueError("boom");
            p.SetException(boom);
            Assert.That(p.Exception(), Is.SameAs(boom));
            Assert.That(p.Cancel(), Is.False);
            Assert.Throws<TypeError>(() => new Job().SetException(null!));
        }

        [Test]
        public void ResultRethrowsWithTheCommandsStackTrace()
        {
            var w = new FakeEngine().Worker();
            var bad = w.Submit(_ => ThrowFromHere());
            Completed(w.RunOnce());
            var first = Assert.Throws<ValueError>(() => bad.Result())!;
            Assert.That(first.StackTrace, Does.Contain(nameof(ThrowFromHere)));
            var again = Assert.Throws<ValueError>(() => bad.Result())!;
            Assert.That(again.StackTrace, Does.Contain(nameof(ThrowFromHere)));
        }

        private static Task<object?> ThrowFromHere() => throw new ValueError("from here");

        [Test]
        public void AThrowingDoneCallbackBreaksNeitherTheJobNorTheOtherCallbacks()
        {
            var sink = new List<Exception>();
            var j = new Job(sink.Add);
            var seen = new List<string>();
            j.AddDoneCallback(_ => throw new ValueError("cb1"));
            j.AddDoneCallback(_ => seen.Add("cb2"));
            j.SetResult(3);
            Assert.That(j.Result(), Is.EqualTo(3));
            Assert.That(seen, Is.EqualTo(new[] { "cb2" }));
            Assert.That(sink.Select(x => x.Message), Is.EqualTo(new[] { "cb1" }));
            j.AddDoneCallback(_ => throw new ValueError("late"));
            Assert.That(sink.Select(x => x.Message), Is.EqualTo(new[] { "cb1", "late" }));

            var throwingSink = new Job(_ => throw new InvalidOperationException("sink"));
            throwingSink.AddDoneCallback(_ => throw new ValueError("cb"));
            throwingSink.AddDoneCallback(_ => seen.Add("after a throwing sink"));
            Assert.DoesNotThrow(() => throwingSink.Cancel());
            Assert.That(seen, Is.EqualTo(new[] { "cb2", "after a throwing sink" }));
        }

        [Test]
        public void AWorkersJobsReportCallbackErrorsToTheEnginesOnError()
        {
            var e = new FakeEngine();
            var reported = new List<Exception>();
            e.Engine.OnError = reported.Add;
            var w = e.Worker();
            var a = w.Submit(Value(1));
            var b = w.Submit(Value(2));
            a.AddDoneCallback(_ => throw new ValueError("callback"));
            Completed(w.RunOnce());
            Assert.That(b.Result(), Is.EqualTo(2), "the next command still ran");
            Assert.That(reported.Select(x => x.Message), Is.EqualTo(new[] { "callback" }));
            Assert.That(e.Ticks, Is.EqualTo(1));
        }

        [Test]
        public void AJobsCallbacksRunOnceAndAreThenReleased()
        {
            var j = new Job();
            int n = 0;
            j.AddDoneCallback(x =>
            {
                n++;
                x.AddDoneCallback(_ => n += 10); // added while settling: done, so it runs at once
            });
            j.Cancel();
            j.Cancel();
            Assert.That(n, Is.EqualTo(11));
        }

        // --- C# specifics: commands -----------------------------------------------------------------------------

        [Test]
        public void ACommandsSynchronousThrowAndAsyncFaultBothFailItsJob()
        {
            var w = new FakeEngine().Worker();
            var gate = new Gate();
            var sync = w.Submit(_ => throw new ValueError("sync"));
            var faulted = w.Submit(_ => Task.FromException<object?>(new ValueError("faulted")));
            var later = w.Submit(async _ =>
            {
                await gate.Wait();
                throw new ValueError("later");
            });
            var none = w.Submit(_ => null!);
            var after = w.Submit(Value("after"));
            var run = w.RunOnce();
            Assert.That(sync.Exception()!.Message, Is.EqualTo("sync"));
            Assert.That(faulted.Exception()!.Message, Is.EqualTo("faulted"));
            Assert.That(later.Running(), Is.True);
            gate.Open();
            Completed(run);
            Assert.That(later.Exception()!.Message, Is.EqualTo("later"));
            Assert.That(none.Exception(), Is.InstanceOf<TypeError>());
            Assert.That(after.Result(), Is.EqualTo("after"));
        }

        [Test]
        public void AJobCompletedOutsideTheWorkerIsSkipped()
        {
            var w = new FakeEngine().Worker();
            int ran = 0;
            var done = w.Submit(Commands.Sync(_ => { ran++; }));
            done.SetResult("elsewhere");
            var started = w.Submit(Commands.Sync(_ => { ran++; }));
            Assert.That(started.SetRunningOrNotifyCancel(), Is.True);
            Completed(w.RunOnce());
            Assert.That(ran, Is.EqualTo(0));
            Assert.That(done.Result(), Is.EqualTo("elsewhere"));
            Assert.That(started.Running(), Is.True, "a job started elsewhere is left to whoever started it");
        }

        [Test]
        public void CommandsWrapsSyncAndAsyncCommands()
        {
            var e = new FakeEngine();
            var w = e.Worker();
            Engine? got = null;
            var action = w.Submit(Commands.Sync(eng => { got = eng; }));
            var func = w.Submit(Commands.Sync(eng => eng.Config.Chains.Count));
            var of = w.Submit(Commands.Of(eng => eng.Reverify()));
            var gate = new Gate();
            var ofTask = w.Submit(Commands.Of(_ => gate.Wait()));
            var aTask = w.Submit(Commands.Sync(eng => Task.FromResult(1)));
            var run = w.RunOnce();
            gate.Open();
            Completed(run);
            Assert.That(action.Result(), Is.Null);
            Assert.That(got, Is.SameAs(e.Engine));
            Assert.That(func.Result(), Is.EqualTo(1));
            Assert.That(of.Result(), Is.EqualTo(new List<string>()));
            Assert.That(ofTask.Done() && ofTask.Exception() == null && ofTask.Result() == null, Is.True);
            Assert.That(aTask.Exception(), Is.InstanceOf<TypeError>(), "Sync of a Task would hand back the Task: use Of");
            Assert.That(aTask.Exception()!.Message, Does.Contain("Commands.Of"));
        }

        [Test]
        public void SubmitIsASubmitFn()
        {
            var w = new FakeEngine().Worker();
            SubmitFn submit = w.Submit;
            var job = submit(Value("x"));
            Completed(w.RunOnce());
            Assert.That(job.Result(), Is.EqualTo("x"));
        }

        // --- C# specifics: the loop -------------------------------------------------------------------------------

        [Test]
        public void AnExplicitRunOnceAfterStopStillRunsOnePass()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t);
            w.Start();
            w.Stop();
            Completed(w.RunOnce());
            Assert.That(e.Ticks, Is.EqualTo(1));
            Assert.That(t.Pending, Is.Null, "and arms nothing");
        }

        [Test]
        public void StartTwiceKeepsOneTimer()
        {
            var t = new FakeScheduler();
            var w = new FakeEngine().Worker(t);
            w.Start();
            w.Start();
            Assert.That(t.Scheduled, Is.EqualTo(new[] { 0.0 }));
            w.Stop();
        }

        [Test]
        public void TheDefaultIntervalIsFifteenSeconds()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t);
            Assert.That(w.Interval(), Is.EqualTo(15));
            w.Start();
            t.Fire();
            Assert.That(t.Pending!.Seconds, Is.EqualTo(15));
            w.Stop();
        }

        [Test]
        public void ANegativeIntervalArmsAtZero()
        {
            var e = new FakeEngine();
            var t = new FakeScheduler();
            var w = e.Worker(t, () => -5);
            w.Start();
            t.Fire();
            Assert.That(t.Scheduled, Is.EqualTo(new[] { 0.0, 0.0 }));
            w.Stop();
        }

        [Test]
        public void ACommandQueuedWhenTheLoopReArmsRunsAtOnce()
        {
            // OnError submits once the chain is over: the loop runs that command at once, not an interval later
            var e = new FakeEngine { TickImpl = () => throw new RuntimeError("down") };
            var t = new FakeScheduler();
            Worker? w = null;
            Job? retry = null;
            w = e.Worker(t, () => 10, _ => { retry ??= w!.Submit(Value("retry")); });
            w.Start();
            t.Fire();
            Assert.That(t.Pending!.Seconds, Is.EqualTo(0));
            e.TickImpl = null;
            t.Fire();
            Assert.That(retry!.Result(), Is.EqualTo("retry"));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(10));
            w.Stop();
        }

        [Test]
        public void AThrowingOnErrorOrIntervalIsReportedAndTheLoopGoesOn()
        {
            var e = new FakeEngine { TickImpl = () => throw new RuntimeError("down") };
            var reported = new List<Exception>();
            e.Engine.OnError = reported.Add;
            var t = new FakeScheduler();
            int calls = 0;
            var errors = new List<Exception>();
            void OnError(Exception x)
            {
                errors.Add(x);
                if (x is RuntimeError) throw new TypeError("on_error");
            }
            var w = e.Worker(t, () => ++calls == 1 ? throw new ValueError("interval") : 10, OnError);
            w.Start();
            t.Fire();
            Assert.That(reported.Select(x => x.Message), Is.EqualTo(new[] { "on_error" }), "a throwing OnError goes to Engine.OnError");
            Assert.That(errors.Select(x => x.Message), Is.EqualTo(new[] { "down", "interval" }), "a throwing Interval goes to OnError");
            Assert.That(t.Pending, Is.Null, "an interval that throws leaves no timer: the loop waits for a wake");
            w.Wake();
            Assert.That(t.Pending!.Seconds, Is.EqualTo(0));
            t.Fire();
            Assert.That(t.Pending!.Seconds, Is.EqualTo(10));
            w.Stop();
        }

        [Test]
        public void StopCancelsQueuedJobsWhoseCallbacksMaySubmit()
        {
            var t = new FakeScheduler();
            var w = new FakeEngine().Worker(t);
            w.Start();
            var queued = w.Submit(Value(1));
            Job? resubmitted = null;
            queued.AddDoneCallback(_ => resubmitted = w.Submit(Value(2)));
            w.Stop();
            Assert.That(queued.Cancelled(), Is.True);
            Assert.That(resubmitted!.Cancelled(), Is.True);
        }

        [Test]
        public void TheConstructorRefusesMissingParts()
        {
            Assert.Throws<TypeError>(() => new Worker(null!, new FakeScheduler()));
            Assert.Throws<TypeError>(() => new Worker(WorkerFixtures.IdleEngine(), null!));
        }

        // --- the real Engine behind the default hooks -----------------------------------------------------------------

        [Test]
        public void TheDefaultHooksDriveTheRealEngine()
        {
            var url = Chains.Testnet["base"].Rpc[0];
            var transport = new FakeTransport(new Dictionary<string, Handler> { { url, WorkerFixtures.IdleNode } });
            var engine = new Engine(WorkerFixtures.BaseConfig(), transport, null, null, () => 1700000000, entropy: WorkerFixtures.Draws());
            var reported = new List<Exception>();
            engine.OnError = reported.Add;
            var w = new Worker(engine, new FakeScheduler());
            Assert.That(w.Engine, Is.SameAs(engine));
            Assert.That(w.Snapshot.Pending, Is.Empty);
            var start = w.Submit(async e => await e.StartSession("unlock", "base", new JsonObject { { "item", "chapter2" } }));
            Completed(w.RunOnce());
            var rec = (JsonObject)start.Result()!;
            Assert.That(rec["id"], Is.EqualTo("ps_ea4702a60e526f21"));
            // the command's eth_blockNumber first, then the tick's scan of the new session
            Assert.That(transport.Calls.Select(c => c.Method), Is.EqualTo(new[] { "eth_blockNumber", "eth_blockNumber", "eth_getLogs" }));
            Assert.That(w.Snapshot.Pending.Keys, Is.EqualTo(new[] { "ps_ea4702a60e526f21" }));
            Assert.That(w.DrainOutcomes(), Is.Empty, "an idle session waits without an error");
            Assert.That(reported, Is.Empty);
        }

        // --- one thread ----------------------------------------------------------------------------------------------

        /// <summary>
        /// The worker as Unity's main thread runs it: a queueing SynchronizationContext, a transport whose answers are
        /// incomplete tasks only the pump completes, the timer fired by the test. Every transport call, posted
        /// continuation, command resume and done callback is on the test thread, and the pump never runs dry while the
        /// worker waits (a blocked continuation would leave it with nothing to do).
        /// </summary>
        [Test]
        public void TheWorkerRunsOnOneThreadAndNeverBlocks()
        {
            int testThread = Thread.CurrentThread.ManagedThreadId;
            var violations = new List<string>();
            void Check(string where)
            {
                int now = Thread.CurrentThread.ManagedThreadId;
                if (now != testThread) violations.Add(where + " ran on thread " + now.ToString(CultureInfo.InvariantCulture));
            }

            var context = new Tier2SingleThreadTest.QueueContext(Check);
            var answers = new Queue<Action>();
            var transport = new DeferredAnswers(answers, payload => Check("transport call " + payload["method"]),
                                                payload => WorkerFixtures.IdleNode((string)payload["method"]!, (List<object?>)payload["params"]!));
            var engine = new Engine(WorkerFixtures.BaseConfig(), transport, null, null, () => 1700000000, entropy: WorkerFixtures.Draws());
            var reported = new List<Exception>();
            engine.OnError = reported.Add;
            var t = new FakeScheduler();
            var errors = new List<Exception>();

            var previous = SynchronizationContext.Current;
            SynchronizationContext.SetSynchronizationContext(context);
            int posts = 0, delivered = 0;
            Job start;
            var callbacks = new List<string>();
            try
            {
                var w = new Worker(engine, t, () => 30, errors.Add);
                start = w.Submit(async e =>
                {
                    var rec = await e.StartSession("unlock", "base", new JsonObject { { "item", "chapter2" } });
                    Check("command resume");
                    return rec;
                });
                start.AddDoneCallback(_ =>
                {
                    Check("done callback");
                    callbacks.Add("start");
                });
                w.Start();
                t.Fire(); // the loop's first run: the command, reverify, tick
                Assert.That(start.Running(), Is.True, "the command waits for its answer");
                // the run is over once the loop re-armed with its interval
                while (!(t.Pending is { Seconds: 30 }))
                {
                    if (context.Posted.Count > 0)
                    {
                        var (callback, state) = context.Posted.Dequeue();
                        callback(state);
                        posts++;
                    }
                    else if (answers.Count > 0)
                    {
                        answers.Dequeue()();
                        delivered++;
                    }
                    else
                    {
                        Assert.Fail("the worker is waiting but nothing is queued: something blocked or left the thread");
                    }
                }
                Assert.That(w.Snapshot.Pending.Keys, Is.EqualTo(new[] { "ps_ea4702a60e526f21" }));
                w.Stop();
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(previous);
            }

            Assert.That(violations, Is.Empty);
            Assert.That(errors, Is.Empty);
            Assert.That(reported, Is.Empty);
            Assert.That(((JsonObject)start.Result()!)["id"], Is.EqualTo("ps_ea4702a60e526f21"));
            Assert.That(callbacks, Is.EqualTo(new[] { "start" }));
            Assert.That(delivered, Is.EqualTo(3), "every answer went through the pump");
            Assert.That(posts, Is.GreaterThanOrEqualTo(3), "the continuations went through the context");
        }

        /// <summary>A transport whose answers are incomplete tasks a pump completes (Tier2SingleThreadTest's Deliver).</summary>
        private sealed class DeferredAnswers : ITransport
        {
            private readonly Queue<Action> _answers;
            private readonly Action<JsonObject> _onCall;
            private readonly Func<JsonObject, object?> _answer;

            public DeferredAnswers(Queue<Action> answers, Action<JsonObject> onCall, Func<JsonObject, object?> answer)
            {
                _answers = answers;
                _onCall = onCall;
                _answer = answer;
            }

            public Task<object?> PostJson(string url, JsonObject payload, double timeout)
            {
                _onCall(payload);
                // RunContinuationsAsynchronously: the awaiting code resumes through the context, never inline
                var tcs = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
                _answers.Enqueue(() =>
                {
                    try
                    {
                        tcs.SetResult(_answer(payload));
                    }
                    catch (Exception e)
                    {
                        tcs.SetException(e);
                    }
                });
                return tcs.Task;
            }
        }
    }
}
