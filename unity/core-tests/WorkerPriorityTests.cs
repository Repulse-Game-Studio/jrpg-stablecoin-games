// Twin of tests/unit/test_worker_priority.py (testbed U15, review V1): a session start the player waits on comes before the
// background watching of pending payments. With pending sessions whose checks hang (a DNS that never answers), the
// start submitted while a tick runs makes the tick yield at once, its request in flight included, and starts without
// waiting for that request at all; the yielded tick keeps its outcomes and the next tick steps the sessions again. A
// real Engine on a real Worker, its timer and its 0-second callbacks fired by the test (Python's runs on a thread).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class WorkerPriorityTests
    {
        private const string Dev = "0x52908400098527886E0F7030069857D2E4169EE7";

        /// <summary>
        /// Answers eth_blockNumber and eth_getLogs, or holds every request while <see cref="Hang"/> is set. With a
        /// <see cref="Receipt"/>, eth_getTransactionReceipt is always answered with it (Python's RevertingNode).
        /// </summary>
        private sealed class Node : ITransport
        {
            public bool Hang;
            public JsonObject? Receipt;
            public readonly List<TaskCompletionSource<object?>> Held = new List<TaskCompletionSource<object?>>();
            public readonly List<string> Methods = new List<string>();

            public Task<object?> PostJson(string url, JsonObject payload, double timeout)
            {
                var method = (string)payload["method"]!;
                Methods.Add(method);
                if (Receipt != null && method == "eth_getTransactionReceipt")
                    return Task.FromResult<object?>(new JsonObject { { "jsonrpc", "2.0" }, { "id", payload["id"] }, { "result", Receipt } });
                if (Hang)
                {
                    var held = new TaskCompletionSource<object?>();
                    Held.Add(held);
                    return held.Task;
                }
                object? result = method == "eth_blockNumber" ? "0x3e8" : new List<object?>();
                return Task.FromResult<object?>(new JsonObject { { "jsonrpc", "2.0" }, { "id", payload["id"] }, { "result", result } });
            }
        }

        [Test]
        public async Task AStartDoesNotWaitBehindTheWatchingOfPendingPayments()
        {
            var cfg = Config.BuildConfig(new JsonObject
            {
                { "wallet", Dev }, { "chains", new List<object?> { "base", "arbitrum" } }, { "testnet", true },
            });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var node = new Node();
            var engine = new Engine(cfg, node, null, null, () => 1700000000);
            var reported = new List<Exception>();
            engine.OnError = reported.Add;
            for (int i = 0; i < 3; i++) // three pending tips, as in the testbed
                await engine.StartSession("tip", "base", new JsonObject { { "amount", "1.00" }, { "token", "USDC" } });
            Assert.That(engine.Pending, Has.Count.EqualTo(3));
            var errors = new List<Exception>();
            var t = new FakeScheduler();
            var w = new Worker(engine, t, () => 60.0, errors.Add);
            node.Hang = true;
            w.Start();
            t.Fire(); // the loop's first pass: the reverify asks nothing, the tick asks the node and hangs
            Assert.That(node.Held, Has.Count.EqualTo(1), "the tick asked the node");
            node.Hang = false; // the network is back for the start (the tick's request still hangs)
            var start = w.Submit(async e => await e.StartSession("unlock", "arbitrum", new JsonObject { { "item", "chapter2" } }));
            Assert.That(start.Done(), Is.False, "nothing runs inside Submit");
            Assert.That(t.RunDue(), Is.EqualTo(1)); // the next frame: the tick yields, the start runs
            Assert.That(start.Done(), Is.True, "the start did not wait behind the tick");
            Assert.That(node.Held.Single().Task.IsCompleted, Is.False, "the tick's request was never waited for");
            var rec = (JsonObject)start.Result()!;
            Assert.That(rec["chain"], Is.EqualTo("arbitrum"));
            Assert.That(engine.Pending, Has.Count.EqualTo(4), "the pending payments are untouched");
            Assert.That(w.Snapshot.Pending, Has.Count.EqualTo(4));
            Assert.That(t.Pending!.Seconds, Is.EqualTo(60), "the follow-up tick stepped the sessions; the loop waits");
            Assert.That(errors, Is.Empty);
            Assert.That(reported, Is.Empty);
            w.Stop();
        }

        /// <summary>
        /// Review V1 (test_a_hint_verdict_reached_before_the_tick_yields_is_reported): the tick judged the hint
        /// (reverted) and cleared it, then waits on its next request; a command makes it yield. The verdict is still
        /// reported, not lost with the step.
        /// </summary>
        [Test]
        public async Task AHintVerdictReachedBeforeTheTickYieldsIsReported()
        {
            var cfg = Config.BuildConfig(new JsonObject
            {
                { "wallet", Dev }, { "chains", new List<object?> { "base" } }, { "testnet", true },
            });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var node = new Node
            {
                Receipt = new JsonObject { { "status", "0x0" }, { "blockNumber", "0x3e8" }, { "logs", new List<object?>() } },
            };
            var engine = new Engine(cfg, node, null, null, () => 1700000000);
            var reported = new List<Exception>();
            engine.OnError = reported.Add;
            var sid = (string)(await engine.StartSession("unlock", "base", new JsonObject { { "item", "chapter2" } }))["id"]!;
            var tx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
            engine.ReportHint(sid, tx);
            var errors = new List<Exception>();
            var t = new FakeScheduler();
            var w = new Worker(engine, t, () => 60.0, errors.Add);
            node.Hang = true;
            w.Start();
            t.Fire(); // the loop's first pass: the tick judges the hint, then asks the node for the head and hangs
            Assert.That(node.Held, Has.Count.EqualTo(1), "the tick went on to its next request after the verdict");
            Assert.That(((JsonObject)engine.Pending[sid]!)["hint_tx"], Is.Null, "the hint is cleared already");
            var command = w.Submit(e => Task.FromResult<object?>("the player's command"));
            Assert.That(t.RunDue(), Is.EqualTo(1)); // the next frame: the tick yields, the command runs
            Assert.That(command.Done(), Is.True);
            Assert.That(command.Result(), Is.EqualTo("the player's command"));
            var outcomes = w.DrainOutcomes();
            Assert.That(outcomes.Select(o => (o.SessionId, o.Error, o.HintTx)),
                        Is.EqualTo(new[] { (sid, (string?)"tx_reverted", (string?)tx) }));
            Assert.That(errors, Is.Empty);
            Assert.That(reported, Is.Empty);
            w.Stop();
        }
    }
}
