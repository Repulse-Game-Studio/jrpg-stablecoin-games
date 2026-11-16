// No Python twin: the Engine on one thread, as Unity's main thread runs it. A single-threaded SynchronizationContext
// queues every post; the transport answers with incomplete tasks that only a pump loop on the test thread completes.
// Every transport call, every continuation the context runs and every resume in the runner must be on the test
// thread, and the core must never block (a blocked continuation would leave the pump with nothing to do).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class Tier2SingleThreadTest
    {
        /// <summary>Queues posts for the pump; Send is refused (nothing in the core may wait on the context).</summary>
        private sealed class QueueContext : SynchronizationContext
        {
            public readonly Queue<(SendOrPostCallback Callback, object? State)> Posted = new Queue<(SendOrPostCallback, object?)>();
            private readonly Action<string> _check;

            public QueueContext(Action<string> check) { _check = check; }

            public override void Post(SendOrPostCallback d, object? state)
            {
                _check("Post");
                Posted.Enqueue((d, state));
            }

            public override void Send(SendOrPostCallback d, object? state) =>
                throw new InvalidOperationException("Send would block");

            public override SynchronizationContext CreateCopy() => this;
        }

        [TestCase("smoke/evm_unlock_paid")]
        [TestCase("evm/hint_verified")]
        [TestCase("evm/reorg_during_confirming")]
        [TestCase("rpc/failover_on_transport_faults")] // recorded transport errors fail the deferred tasks
        [TestCase("solana/paid_by_hint")]
        [TestCase("solana/build_payment")]
        public void ReplaysOnTheTestThread(string id)
        {
            Assert.That(Tier2.Runs(id), id + " must be an enabled vector");
            int testThread = Thread.CurrentThread.ManagedThreadId;
            var violations = new List<string>();
            void Check(string where)
            {
                int now = Thread.CurrentThread.ManagedThreadId;
                if (now != testThread)
                    violations.Add(where + " ran on thread " + now.ToString(CultureInfo.InvariantCulture));
            }

            var context = new QueueContext(Check);
            var answers = new Queue<Action>();
            int calls = 0, resumes = 0;
            var transport = new ReplayTransport
            {
                OnCall = (url, payload) =>
                {
                    calls++;
                    Check("transport call " + payload["method"]);
                },
                Deliver = answer =>
                {
                    // RunContinuationsAsynchronously: the awaiting code resumes through the context, never inline
                    var tcs = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
                    answers.Enqueue(() =>
                    {
                        try
                        {
                            tcs.SetResult(answer());
                        }
                        catch (Exception e)
                        {
                            tcs.SetException(e);
                        }
                    });
                    return tcs.Task;
                },
            };

            var previous = SynchronizationContext.Current;
            SynchronizationContext.SetSynchronizationContext(context);
            int posts = 0, delivered = 0;
            Task<string?> run;
            try
            {
                run = Tier2.Replay(Tier2Tests.Load(id), transport, where =>
                {
                    resumes++;
                    Check("resume after " + where);
                });
                while (!run.IsCompleted)
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
                        Assert.Fail("the replay is waiting but nothing is queued: something blocked or left the thread");
                    }
                }
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(previous);
            }

            Assert.That(run.IsCompleted && !run.IsFaulted && !run.IsCanceled, "the replay completed: " + run.Exception);
            Assert.That(run.GetAwaiter().GetResult(), Is.Null); // completed: this does not wait
            Assert.That(violations, Is.Empty);
            Assert.That(calls, Is.GreaterThan(0));
            Assert.That(delivered, Is.EqualTo(calls), "every answer went through the pump");
            Assert.That(posts, Is.GreaterThan(0), "continuations went through the context");
            Assert.That(resumes, Is.GreaterThan(0));
        }

        // The largest vector (about 0.5 MB of recorded answers, signature pages of SIG_PAGE): it must stay fast.
        [Test, MaxTime(10_000)]
        public void ScanFarBehindReplaysOnTheTestThreadInUnderTenSeconds() => ReplaysOnTheTestThread("solana/scan_far_behind");
    }
}
