// Twin of scfg_core's tests/unit/test_overlay.py (the same names), ported from rpgmaker/src/core/overlay.test.ts with
// `ui` a fake page and the Solana transaction builds Jobs the test finishes itself; then the hardening the TS file
// took from the Godot twin, the Godot-only case of a batch cut short by a stop (godot/tests/test_overlay.gd), and the
// C# rulings: which request ids are exact integers, Python == for the status, a reply that stops the overlay.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class OverlayTests
    {
        private static readonly string Tx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
        private const string Sol = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";
        private const double MaxSafe = 9007199254740991.0; // 2**53 - 1

        /// <summary>The fake page: every call recorded as a JSON list (["open", key, title, logo], ["reply", id, status, body]...).</summary>
        private sealed class FakeUi : IOverlayUi
        {
            public readonly List<List<object?>> Calls = new List<List<object?>>();
            public object? Inbox = new List<object?>();
            public Action<long>? OnReply;

            public void Open(string key, JsonObject sessionInfo, string? logoUrl) =>
                Calls.Add(new List<object?> { "open", key, sessionInfo.Get("title"), logoUrl });

            public void Show(string key) => Calls.Add(new List<object?> { "show", key });

            public void Close(string key) => Calls.Add(new List<object?> { "close", key });

            public object? Take(string key)
            {
                var output = Inbox;
                Inbox = new List<object?>();
                return output;
            }

            public void SetStatus(string key, JsonObject status) => Calls.Add(new List<object?> { "status", status.Get("state") });

            public void Reply(string key, long id, int status, JsonObject body)
            {
                Calls.Add(new List<object?> { "reply", id, (long)status, body });
                OnReply?.Invoke(id);
            }

            public List<List<object?>> Of(string kind) => Calls.Where(c => (string)c[0]! == kind).ToList();

            public string Replies() => Json.Dumps(Of("reply").Cast<object?>().ToList());

            public string All() => Json.Dumps(Calls.Cast<object?>().ToList());

            public int Count(string kind) => Of(kind).Count;
        }

        private sealed class Clock
        {
            public double Now;
            public double Call() => Now;
            public void Advance(double s) => Now += s;
        }

        private sealed class Made
        {
            public OverlayCheckout O = null!;
            public FakeUi Ui = null!;
            public readonly List<(string Tx, string? Sender)> Hints = new List<(string, string?)>();
            public readonly JsonObject Status = new JsonObject { { "state", "waiting" } };
            public Clock Clock = null!;
        }

        private static Made Make(Func<string, Job>? buildTx = null, Clock? clock = null)
        {
            var m = new Made { Ui = new FakeUi(), Clock = clock ?? new Clock() };
            m.O = new OverlayCheckout(m.Ui, "o1", new JsonObject { { "title", "Chapter 2" } },
                                      (tx, sender) => m.Hints.Add((tx, sender)),
                                      () => (JsonObject)DeepCopy.Of(m.Status)!, "data:image/png;base64,AA", buildTx, m.Clock.Call);
            return m;
        }

        private static JsonObject Build(object? id, string account = Sol) => new JsonObject
        {
            { "type", "build" }, { "id", id }, { "body", new JsonObject { { "account", account } } },
        };

        private static JsonObject TxMessage(string tx) => new JsonObject
        {
            { "type", "tx" }, { "body", new JsonObject { { "tx_hash", tx } } },
        };

        private static string R(long id, long status, string body) =>
            "[\"reply\", " + id.ToString(System.Globalization.CultureInfo.InvariantCulture) + ", "
            + status.ToString(System.Globalization.CultureInfo.InvariantCulture) + ", " + body + "]";

        private const string Unavailable = "{\"error\": \"unavailable\"}";

        private static Job Succeed(Job job, object? value)
        {
            job.SetRunningOrNotifyCancel();
            job.SetResult(value);
            return job;
        }

        // --- overlay.test.ts ------------------------------------------------------------

        [Test]
        public void StartOpensTheOverlayAndPushesTheStatus()
        {
            var m = Make();
            Assert.That(m.O.Url, Is.EqualTo(OverlayCheckout.OverlayUrl));
            Assert.That(OverlayCheckout.OverlayUrl, Is.EqualTo("overlay:checkout"));
            Assert.That(m.O.Running, Is.False);
            m.O.Start();
            Assert.That(m.O.Url, Is.EqualTo(OverlayCheckout.OverlayUrl));
            Assert.That(m.O.Running, Is.True);
            Assert.That(m.Ui.All(), Is.EqualTo("[[\"open\", \"o1\", \"Chapter 2\", \"data:image/png;base64,AA\"], [\"status\", \"waiting\"]]"));
        }

        [Test]
        public void StatusIsPushedOnlyWhenItChanges()
        {
            var m = Make();
            m.O.Start();
            m.O.Pump();
            m.O.Pump();
            m.Status["state"] = "confirming";
            m.O.Pump();
            Assert.That(Json.Dumps(m.Ui.Of("status").Cast<object?>().ToList()), Is.EqualTo("[[\"status\", \"waiting\"], [\"status\", \"confirming\"]]"));
        }

        [Test]
        public void ATxMessageBecomesAHint()
        {
            var m = Make();
            m.O.Start();
            m.Ui.Inbox = new List<object?> { TxMessage(Tx) };
            m.O.Pump();
            Assert.That(m.Hints, Is.EqualTo(new[] { (Tx, (string?)null) }));
        }

        [Test]
        public void GarbledMessagesAreIgnored()
        {
            var built = 0;
            var m = Make(_ =>
            {
                built++;
                return new Job();
            });
            m.O.Start();
            m.Ui.Inbox = new List<object?>
            {
                "x", null, 5L, new List<object?>(), new List<object?> { TxMessage(Tx) }, new JsonObject { { "type", "tx" } },
                new JsonObject { { "type", "tx" }, { "body", "x" } }, TxMessage("0x12"), new JsonObject { { "type", "nope" } },
                new JsonObject { { "type", 5L }, { "id", 1L }, { "body", new JsonObject { { "account", Sol } } } },
                Build("7"),
                // ids the page could only have made up: a boolean, a fraction, out of the integers JSON keeps exact
                Build(true), Build(7.5), Build(9007199254740992.0), Build(-9007199254740992.0), Build(null),
                Build(double.NaN), Build(double.PositiveInfinity), Build(1e300), Build(new BigInteger(1) << 70),
                new JsonObject { { "type", "build" }, { "body", new JsonObject { { "account", Sol } } } },
            };
            m.O.Pump();
            Assert.That(m.Hints, Is.Empty);
            Assert.That(m.Ui.Replies(), Is.EqualTo("[]"));
            Assert.That(built, Is.EqualTo(0));
            // what Take() hands back that is not a list of messages is no messages
            foreach (var inbox in new object?[] { null, "[]", TxMessage(Tx), 7L, new JsonObject() })
            {
                m.Ui.Inbox = inbox;
                Assert.DoesNotThrow(() => m.O.Pump(), Py.TypeName(inbox));
            }
            Assert.That(m.Hints, Is.Empty);
        }

        [Test]
        public void BuildRepliesWhenTheFutureIsDone()
        {
            var fut = new Job();
            var m = Make(_ => fut);
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L) };
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[]"), "still building");
            Succeed(fut, new JsonObject { { "transaction", "AAA" } });
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 200, "{\"transaction\": \"AAA\"}") + "]"));
        }

        [Test]
        public void BuildFailuresAndBadAccounts()
        {
            var failing = new Job();
            failing.SetRunningOrNotifyCancel();
            failing.SetException(new RuntimeError("rpc down"));
            var accounts = new List<string>();
            var m = Make(account =>
            {
                accounts.Add(account);
                return failing;
            });
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L), Build(2L, "0xabc") };
            m.O.Pump();
            Assert.That(accounts, Is.EqualTo(new[] { Sol }), "only the valid account reaches build_tx");
            Assert.That(m.Ui.Replies(), Does.Contain(R(1, 503, Unavailable)));
            Assert.That(m.Ui.Replies(), Does.Contain(R(2, 400, "{\"error\": \"bad account\"}")));
        }

        [Test]
        public void BuildWithoutSolanaIsNotFound()
        {
            var m = Make();
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(3L) };
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(3, 404, "{\"error\": \"not_found\"}") + "]"));
        }

        [Test]
        public void ABuildThatTakesTooLongIsCancelled()
        {
            var clock = new Clock();
            var never = new Job();
            var m = Make(_ => never, clock);
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L) };
            m.O.Pump();
            clock.Advance(PageMessages.BuildTimeoutS - 0.5);
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[]"), "not before the timeout");
            clock.Advance(1);
            m.O.Pump();
            Assert.That(never.Cancelled(), Is.True);
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 503, Unavailable) + "]"));
            m.O.Pump();
            Assert.That(m.Ui.Count("reply"), Is.EqualTo(1), "answered once");
        }

        [Test]
        public void ReopenShowsTheSameOverlay()
        {
            var m = Make();
            m.O.Reopen();
            Assert.That(m.Ui.Calls, Is.Empty, "nothing before start");
            m.O.Start();
            m.O.Reopen();
            Assert.That(m.Ui.Count("open"), Is.EqualTo(1));
            Assert.That(m.Ui.All(), Does.Contain("[\"show\", \"o1\"]"));
        }

        [Test]
        public void StopClosesOnceAndPumpDoesNothingAfter()
        {
            var m = Make();
            m.O.Start();
            m.O.Stop();
            m.O.Stop();
            m.Ui.Inbox = new List<object?> { TxMessage(Tx) };
            m.O.Pump();
            m.O.Reopen();
            Assert.That(Json.Dumps(m.Ui.Of("close").Cast<object?>().ToList()), Is.EqualTo("[[\"close\", \"o1\"]]"));
            Assert.That(m.Ui.Count("show"), Is.EqualTo(0), "no reopen after stop");
            Assert.That(m.Hints, Is.Empty);
            Assert.That(m.O.Running, Is.False);
        }

        // --- hardening (the Godot twin's decisions, as overlay.test.ts has them) ----------------

        [Test]
        public void AReusedRequestIdCancelsTheOlderBuild()
        {
            var jobs = new List<Job>();
            var m = Make(_ =>
            {
                jobs.Add(new Job());
                return jobs[jobs.Count - 1];
            });
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(4L), Build(4.0) };
            m.O.Pump();
            Assert.That(jobs.Count, Is.EqualTo(2));
            Assert.That(jobs[0].Cancelled(), Is.True, "the older build is moot");
            Assert.That(m.O.PendingBuilds, Is.EqualTo(1));
            Succeed(jobs[1], new JsonObject { { "transaction", "BBB" } });
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(4, 200, "{\"transaction\": \"BBB\"}") + "]"), "one answer, from the newer build");
        }

        [Test]
        public void ABuildTxThatThrowsOrGivesNoJobIsUnavailable()
        {
            int n = 0;
            var m = Make(_ =>
            {
                n++;
                if (n == 1) throw new InvalidOperationException("the controller is gone");
                if (n == 2) throw new RuntimeError("the worker stopped");
                return null!;
            });
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L), Build(2L), Build(3L) };
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 503, Unavailable) + ", " + R(2, 503, Unavailable) + ", " + R(3, 503, Unavailable) + "]"));
            m.O.Pump();
            Assert.That(m.Ui.Count("reply"), Is.EqualTo(3), "nothing kept for them");
            Assert.That(m.O.PendingBuilds, Is.EqualTo(0));
        }

        [Test]
        public void ABuildThatSucceedsWithNoObjectIsUnavailable()
        {
            var values = new object?[] { new List<object?> { "not", "an", "object" }, null, "x", 5L };
            var jobs = values.Select(_ => new Job()).ToList();
            int i = 0;
            var m = Make(_ => jobs[i++]);
            m.O.Start();
            m.Ui.Inbox = Enumerable.Range(0, values.Length).Select(k => (object?)Build((long)k)).ToList();
            m.O.Pump();
            for (int k = 0; k < values.Length; k++) Succeed(jobs[k], values[k]);
            m.O.Pump();
            var expected = Enumerable.Range(0, values.Length).Select(k => R(k, 503, Unavailable));
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + string.Join(", ", expected) + "]"));
        }

        [Test]
        public void ACancelledBuildIsUnavailable()
        {
            var fut = new Job();
            var m = Make(_ => fut);
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L) };
            m.O.Pump();
            fut.Cancel(); // e.g. the worker stopped
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 503, Unavailable) + "]"));
        }

        [Test]
        public void CallbacksAreNotCalledAfterStop()
        {
            // the hint stops the overlay (its controller closed): the rest of that batch is not handled, no status is
            // pushed, and a build finishing later is never answered
            var fut = new Job();
            int statuses = 0;
            var ui = new FakeUi();
            var hints = new List<string>();
            int builds = 0;
            OverlayCheckout o = null!;
            o = new OverlayCheckout(ui, "o1", new JsonObject { { "title", "T" } },
                                    (tx, _) =>
                                    {
                                        hints.Add(tx);
                                        o.Stop();
                                    },
                                    () =>
                                    {
                                        statuses++;
                                        return new JsonObject { { "state", "waiting" } };
                                    },
                                    buildTx: _ =>
                                    {
                                        builds++;
                                        return fut;
                                    });
            o.Start();
            ui.Inbox = new List<object?> { Build(1L) };
            o.Pump();
            Assert.That(builds, Is.EqualTo(1));
            int before = statuses;
            ui.Inbox = new List<object?> { TxMessage(Tx), TxMessage(Tx), Build(2L) };
            o.Pump();
            Assert.That(hints, Is.EqualTo(new[] { Tx }), "the batch stops at the message that stopped it");
            Assert.That(builds, Is.EqualTo(1), "no build after stop");
            Assert.That(statuses, Is.EqualTo(before), "no status asked after stop");
            Assert.That(fut.Cancelled(), Is.True, "Stop() cancelled the build");
            o.Pump();
            Assert.That(ui.Replies(), Is.EqualTo("[]"), "never answered after stop");
            Assert.That(Json.Dumps(ui.Of("close").Cast<object?>().ToList()), Is.EqualTo("[[\"close\", \"o1\"]]"));
        }

        [Test]
        public void ABuildTxThatStopsTheOverlayHasItsJobCancelled()
        {
            // build_tx closed the controller (and so stopped this overlay) before handing its Job back: nobody will
            // answer that build, so it must not run on in the Worker
            var fut = new Job();
            var ui = new FakeUi();
            OverlayCheckout o = null!;
            o = new OverlayCheckout(ui, "o1", new JsonObject { { "title", "T" } }, (_, __) => { },
                                    () => new JsonObject { { "state", "waiting" } },
                                    buildTx: _ =>
                                    {
                                        o.Stop();
                                        return fut;
                                    });
            o.Start();
            ui.Inbox = new List<object?> { Build(1L) };
            o.Pump();
            Assert.That(fut.Cancelled(), Is.True, "the Job just received is cancelled");
            Assert.That(o.PendingBuilds, Is.EqualTo(0), "nothing kept for it");
            o.Pump();
            Assert.That(ui.Replies(), Is.EqualTo("[]"), "never answered");
        }

        [Test]
        public void AStatusThatIsNotAnObjectIsNotPushed()
        {
            var ui = new FakeUi();
            JsonObject? answer = null;
            var o = new OverlayCheckout(ui, "o1", new JsonObject { { "title", "T" } }, (_, __) => { }, () => answer!);
            o.Start();
            o.Pump();
            Assert.That(ui.Count("status"), Is.EqualTo(0), "nothing to push");
            answer = new JsonObject { { "state", "waiting" }, { "nested", new JsonObject { { "a", new List<object?> { 1L, 2L } } } } };
            o.Pump();
            answer = new JsonObject { { "state", "waiting" }, { "nested", new JsonObject { { "a", new List<object?> { 1L, 2L } } } } };
            o.Pump();
            Assert.That(ui.Count("status"), Is.EqualTo(1), "an equal status (deeply) is not pushed again");
            answer = new JsonObject { { "state", "waiting" }, { "nested", new JsonObject { { "a", new List<object?> { 1L, 3L } } } } };
            o.Pump();
            Assert.That(ui.Count("status"), Is.EqualTo(2));
        }

        // --- godot/tests/test_overlay.gd -------------------------------------------------

        [Test]
        public void TheRestOfABatchIsIgnoredOnceACallbackStoppedTheOverlay()
        {
            var ui = new FakeUi();
            var hints = new List<string>();
            var built = new List<string>();
            var status = new JsonObject { { "state", "waiting" } };
            OverlayCheckout o = null!;
            o = new OverlayCheckout(ui, "o1", new JsonObject { { "title", "Chapter 2" } },
                                    (tx, _) =>
                                    {
                                        hints.Add(tx);
                                        o.Stop();
                                    },
                                    () => (JsonObject)DeepCopy.Of(status)!,
                                    buildTx: account =>
                                    {
                                        built.Add(account);
                                        return new Job();
                                    },
                                    clock: new Clock().Call);
            o.Start();
            status["state"] = "confirming";
            ui.Inbox = new List<object?> { TxMessage(Tx), TxMessage("0x" + string.Concat(Enumerable.Repeat("cd", 32))), Build(2L) };
            o.Pump();
            Assert.That(hints, Is.EqualTo(new[] { Tx }), "only the first hint");
            Assert.That(built, Is.Empty, "no build after stop");
            Assert.That(o.PendingBuilds, Is.EqualTo(0), "nothing kept");
            Assert.That(ui.All(), Does.Not.Contain("[\"status\", \"confirming\"]"), "no status pushed to a closed overlay");
        }

        // --- C# rulings ---------------------------------------------------------------------

        [Test]
        public void ARequestIdIsAnExactInteger()
        {
            var m = Make(_ => Succeed(new Job(), new JsonObject { { "transaction", "T" } }));
            m.O.Start();
            m.Ui.Inbox = new List<object?>
            {
                Build(5L), Build(6), Build(7.0), Build(MaxSafe), Build(-MaxSafe), Build(-0.0),
                Build(1L << 60), // a long the bridge parsed is exact whatever its size
            };
            m.O.Pump();
            var ids = m.Ui.Of("reply").Select(c => (long)c[1]!).ToList();
            Assert.That(ids, Is.EqualTo(new[] { 5L, 6L, 7L, (long)MaxSafe, -(long)MaxSafe, 0L, 1L << 60 }));
            Assert.That(m.Ui.Of("reply").Select(c => (long)c[2]!), Is.All.EqualTo(200L));
        }

        [Test]
        public void TheStatusComparesWithPythonEquality()
        {
            var ui = new FakeUi();
            JsonObject answer = new JsonObject { { "state", "waiting" }, { "confirmations", 1L }, { "ok", true } };
            var o = new OverlayCheckout(ui, "o1", new JsonObject(), (_, __) => { }, () => answer);
            o.Start();
            // 1 == 1.0 == True and key order does not matter: the same status
            answer = new JsonObject { { "ok", 1L }, { "confirmations", 1.0 }, { "state", "waiting" } };
            o.Pump();
            answer = new JsonObject { { "ok", true }, { "confirmations", new BigInteger(1) }, { "state", "waiting" } };
            o.Pump();
            Assert.That(ui.Count("status"), Is.EqualTo(1));
            // "1" is not 1, None is not 0, and a key more is a change
            foreach (var changed in new[]
            {
                new JsonObject { { "ok", true }, { "confirmations", "1" }, { "state", "waiting" } },
                new JsonObject { { "ok", true }, { "confirmations", null }, { "state", "waiting" } },
                new JsonObject { { "ok", true }, { "confirmations", 1.5 }, { "state", "waiting" } },
                new JsonObject { { "ok", true }, { "confirmations", 1L }, { "state", "waiting" }, { "x", null } },
            })
            {
                answer = changed;
                o.Pump();
            }
            Assert.That(ui.Count("status"), Is.EqualTo(5));
        }

        [Test]
        public void AStatusChangedInPlaceIsSeen()
        {
            // the overlay keeps a copy: a controller that hands back the same (mutated) object is still pushed
            var ui = new FakeUi();
            var status = new JsonObject { { "state", "waiting" }, { "list", new List<object?> { 1L } } };
            var o = new OverlayCheckout(ui, "o1", new JsonObject(), (_, __) => { }, () => status);
            o.Start();
            ((List<object?>)status["list"]!).Add(2L);
            o.Pump();
            status["state"] = "confirming";
            o.Pump();
            o.Pump();
            Assert.That(ui.Count("status"), Is.EqualTo(3));
        }

        [Test]
        public void AReplyThatStopsTheOverlayEndsTheReplies()
        {
            var m = Make(_ => Succeed(new Job(), new JsonObject { { "transaction", "T" } }));
            m.Ui.OnReply = _ => m.O.Stop();
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L), Build(2L) };
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 200, "{\"transaction\": \"T\"}") + "]"));
        }

        [Test]
        public void TheTxSenderIsPassedOn()
        {
            var m = Make();
            m.O.Start();
            const string from = "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed";
            m.Ui.Inbox = new List<object?>
            {
                new JsonObject { { "type", "tx" }, { "body", new JsonObject { { "tx_hash", Tx.ToUpperInvariant().Replace("0X", "0x") }, { "from", from } } } },
            };
            m.O.Pump();
            Assert.That(m.Hints, Is.EqualTo(new[] { (Tx, (string?)"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed") }));
        }

        [Test]
        public void ItIsBuiltFromTheControllersServerArgs()
        {
            var ui = new FakeUi();
            var hints = new List<string>();
            var fut = new Job();
            var args = new ServerArgs(new JsonObject { { "title", "Chapter 2" } }, (tx, _) => hints.Add(tx),
                                      () => new JsonObject { { "state", "waiting" } }, _ => fut);
            var o = new OverlayCheckout(ui, "o2", args, "data:image/png;base64,AA");
            Assert.That(o.SessionInfo, Is.SameAs(args.SessionInfo));
            o.Start();
            ui.Inbox = new List<object?> { TxMessage(Tx), Build(1L) };
            o.Pump();
            Assert.That(hints, Is.EqualTo(new[] { Tx }));
            Assert.That(o.PendingBuilds, Is.EqualTo(1), "a build waits on the default clock");
            Assert.That(ui.All(), Is.EqualTo("[[\"open\", \"o2\", \"Chapter 2\", \"data:image/png;base64,AA\"], [\"status\", \"waiting\"]]"));
            Assert.Throws<TypeError>(() => new OverlayCheckout(ui, "o3", (ServerArgs)null!));
            Assert.Throws<TypeError>(() => new OverlayCheckout(null!, "o3", args));
        }

        [Test]
        public void PyEqIsPythonsEqualityForJsonValues()
        {
            Assert.That(Py.Eq(1L, 1.0), Is.True);
            Assert.That(Py.Eq(true, 1L), Is.True);
            Assert.That(Py.Eq(false, 0.0), Is.True);
            Assert.That(Py.Eq(true, 2L), Is.False);
            Assert.That(Py.Eq(new BigInteger(1) << 53, 9007199254740992.0), Is.True);
            Assert.That(Py.Eq((new BigInteger(1) << 53) + 1, 9007199254740992.0), Is.False, "int vs float is exact");
            Assert.That(Py.Eq(double.NaN, double.NaN), Is.False);
            Assert.That(Py.Eq(null, null), Is.True);
            Assert.That(Py.Eq(null, 0L), Is.False);
            Assert.That(Py.Eq("a", "a"), Is.True);
            Assert.That(Py.Eq("1", 1L), Is.False);
            Assert.That(Py.Eq(new List<object?> { 1L, "x" }, new List<object?> { 1.0, "x" }), Is.True);
            Assert.That(Py.Eq(new List<object?> { 1L, 2L }, new List<object?> { 2L, 1L }), Is.False);
            Assert.That(Py.Eq(new List<object?> { 1L }, new List<object?> { 1L, 1L }), Is.False);
            Assert.That(Py.Eq(new JsonObject { { "a", 1L }, { "b", 2L } }, new JsonObject { { "b", 2L }, { "a", 1L } }), Is.True);
            Assert.That(Py.Eq(new JsonObject { { "a", 1L } }, new JsonObject { { "b", 1L } }), Is.False);
            Assert.That(Py.Eq(new JsonObject(), new List<object?>()), Is.False);
            Assert.That(Py.Eq(new JsonObject { { "a", null } }, new JsonObject()), Is.False);
        }

        [Test]
        public void PyEqTakesTheIdentityShortcutOnlyForItemsOfContainers()
        {
            object nan = double.NaN;
            Assert.That(Py.Eq(nan, nan), Is.False, "Python: x == x is False for a NaN");
            var list = new List<object?> { nan };
            Assert.That(Py.Eq(new List<object?> { nan }, new List<object?> { nan }), Is.True, "the same item inside lists");
            Assert.That(Py.Eq(list, list), Is.True);
            Assert.That(Py.Eq(new JsonObject { { "x", nan } }, new JsonObject { { "x", nan } }), Is.True, "and inside dicts");
            Assert.That(Py.Eq(new List<object?> { double.NaN }, new List<object?> { double.NaN }), Is.False, "two NaN objects");
        }

        [Test]
        public void PyEqOfTooDeepOrCircularValuesIsAValueError()
        {
            List<object?> Deep(int n)
            {
                var v = new List<object?>();
                for (int i = 0; i < n; i++) v = new List<object?> { v };
                return v;
            }
            Assert.Throws<ValueError>(() => Py.Eq(Deep(Json.MaxDepth + 1), Deep(Json.MaxDepth + 1)));
            var a = new List<object?>();
            a.Add(a);
            var b = new List<object?>();
            b.Add(b);
            Assert.Throws<ValueError>(() => Py.Eq(a, b), "circular lists");
            var oa = new JsonObject();
            oa["self"] = oa;
            var ob = new JsonObject();
            ob["self"] = ob;
            Assert.Throws<ValueError>(() => Py.Eq(oa, ob), "circular dicts");
        }

        [Test]
        public void AReusedIdThatHandsBackTheSameJobKeepsIt()
        {
            var fut = new Job();
            var m = Make(_ => fut);
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L), Build(1L) };
            m.O.Pump();
            Assert.That(fut.Cancelled(), Is.False, "the same Job is not moot");
            Assert.That(m.O.PendingBuilds, Is.EqualTo(1));
            Succeed(fut, new JsonObject { { "transaction", "T" } });
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 200, "{\"transaction\": \"T\"}") + "]"));
        }

        [Test]
        public void AReusedIdGetsAFreshDeadline()
        {
            var clock = new Clock();
            var m = Make(_ => new Job(), clock);
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L) };
            m.O.Pump();
            clock.Advance(PageMessages.BuildTimeoutS - 1);
            m.Ui.Inbox = new List<object?> { Build(1L) };
            m.O.Pump();
            clock.Advance(2); // past the first request's deadline, not the second's
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[]"), "no 503 at the old deadline");
            clock.Advance(PageMessages.BuildTimeoutS);
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 503, Unavailable) + "]"));
        }

        [Test]
        public void AReusedIdKeepsItsPlaceInTheReplyOrder()
        {
            var jobs = new List<Job>();
            var m = Make(_ =>
            {
                jobs.Add(new Job());
                return jobs[jobs.Count - 1];
            });
            m.O.Start();
            m.Ui.Inbox = new List<object?> { Build(1L), Build(2L), Build(1L) };
            m.O.Pump();
            Succeed(jobs[1], new JsonObject { { "transaction", "B" } });
            Succeed(jobs[2], new JsonObject { { "transaction", "C" } });
            m.O.Pump();
            Assert.That(m.Ui.Replies(), Is.EqualTo("[" + R(1, 200, "{\"transaction\": \"C\"}") + ", "
                                                   + R(2, 200, "{\"transaction\": \"B\"}") + "]"), "id 1 was asked first");
        }

        [Test]
        public void ABuildTxThatStopsTheOverlayAndThrowsGetsNoReply()
        {
            var ui = new FakeUi();
            OverlayCheckout o = null!;
            o = new OverlayCheckout(ui, "o1", new JsonObject { { "title", "T" } }, (_, __) => { },
                                    () => new JsonObject { { "state", "waiting" } },
                                    buildTx: _ =>
                                    {
                                        o.Stop();
                                        throw new RuntimeError("the controller is gone");
                                    });
            o.Start();
            ui.Inbox = new List<object?> { Build(1L), Build(2L) };
            o.Pump();
            Assert.That(ui.Replies(), Is.EqualTo("[]"));
            Assert.That(ui.Count("close"), Is.EqualTo(1));
            Assert.That(o.PendingBuilds, Is.EqualTo(0));
        }
    }

    public class JsonEqualTests
    {
        [Test]
        public void JsonEqualIsPythonsEqualityForJsonValues()
        {
            Assert.That(Json.Equal(Json.Loads("{\"a\": [1, 2.0, true]}"), Json.Loads("{\"a\": [1.0, 2, 1]}")), Is.True);
            Assert.That(Json.Equal(Json.Loads("{\"a\": 1, \"b\": 2}"), Json.Loads("{\"b\": 2, \"a\": 1}")), Is.True);
            Assert.That(Json.Equal(Json.Loads("[1, 2]"), Json.Loads("[2, 1]")), Is.False);
            Assert.That(Json.Equal(Json.Loads("{\"a\": null}"), Json.Loads("{}")), Is.False);
            Assert.That(Json.Equal(null, null), Is.True);
            Assert.That(Json.Equal(double.NaN, double.NaN), Is.False);
        }
    }
}
