// Twin of scfg_core's tests/unit/test_restore_flow.py (and test_checkout_solana.py::test_restore_accepts_signatures),
// ported from rpgmaker/src/core/restore_flow.test.ts, plus the cases godot/tests/test_restore_flow.gd adds (the
// MESSAGES order and the result shapes, and the job edges: a failure that is not a RestoreError, a RestoreError's
// code, a success of the wrong type). The engine runs over the fake JSON-RPC nodes of CheckoutFakes.cs.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    public class RestoreFlowTests
    {
        private const string Dev = CheckoutFixture.Dev;
        private const string Payer = CheckoutFixture.Payer;
        private static readonly string Tx = CheckoutFixture.Tx;
        private static string UsdcArb => CheckoutFixture.UsdcArb;

        private sealed class Rigged
        {
            public RestoreController Ctl = null!;
            public CheckoutRig Rig = null!;
            public ImmediateSubmitter Submitter = null!;
            public Engine E => Rig.Engine;
        }

        /// <summary>test_restore_flow.py's setup: <paramref name="logs"/> are the raw logs of Tx on Arbitrum.</summary>
        private static Rigged Setup(List<JsonObject>? logs = null, string status = "0x1")
        {
            var cfg = Config.BuildConfig(new JsonObject { { "wallet", Dev } });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var r = new Rigged { Rig = new CheckoutRig(cfg) };
            if ((logs != null && logs.Count > 0) || status == "0x0")
            {
                r.Rig.Evm("arbitrum").Receipts[Tx] = new JsonObject
                {
                    { "status", status }, { "logs", (logs ?? new List<JsonObject>()).Cast<object?>().ToList() },
                };
            }
            r.Submitter = new ImmediateSubmitter(r.E);
            r.Ctl = new RestoreController(r.Submitter.Submit);
            return r;
        }

        private static JsonObject Log(BigInteger value, long logIndex = 0) =>
            FakeEvmNode.TransferLog(UsdcArb, Payer, Dev, value, 5, Tx, logIndex);

        /// <summary>test_restore_flow.py's run.</summary>
        private static async Task Run(RestoreController ctl, ImmediateSubmitter submit, string? text = null)
        {
            ctl.TxInput = text ?? Tx;
            ctl.Check();
            await submit.Idle();
            ctl.Poll();
            if (ctl.State == "saving")
            {
                await submit.Idle();
                ctl.Poll();
            }
        }

        private static List<string> Owned(Engine e) => e.Snapshot().Owned.ToList();

        /// <summary>solana_fakes.ts's sig(n).</summary>
        private static string SigOf(int n)
        {
            var b = new byte[64];
            b[63] = (byte)n;
            return Base58.B58Encode(b);
        }

        // --- restore_flow.test.ts ----------------------------------------------------------

        [Test]
        public async Task AutomaticRestore()
        {
            var r = Setup(new List<JsonObject> { Log(4_993_742) });
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.State, Is.EqualTo("done"));
            var res = r.Ctl.Result();
            Assert.That(res.Status, Is.EqualTo("restored"));
            Assert.That(res.Restored[0]["item"], Is.EqualTo("chapter2"));
            Assert.That(res.Error, Is.Null);
            Assert.That(Owned(r.E), Is.EqualTo(new[] { "chapter2" }));
        }

        [Test]
        public async Task AlreadyRestoredIsNothingFound()
        {
            var r = Setup(new List<JsonObject> { Log(4_993_742) });
            await Run(r.Ctl, r.Submitter);
            var ctl2 = new RestoreController(r.Ctl.Submit);
            await Run(ctl2, r.Submitter);
            Assert.That(ctl2.State, Is.EqualTo("done"));
            Assert.That(ctl2.Result().Status, Is.EqualTo("nothing_found"));
            Assert.That(ctl2.Result().Restored, Is.Empty);
        }

        [Test]
        public async Task AmbiguousAmountNeedsChoice()
        {
            var r = Setup(new List<JsonObject> { Log(5_000_000) });
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.State, Is.EqualTo("choose"));
            Assert.That(r.Ctl.PendingChoices(), Is.EqualTo(new[] { 0 }));
            Assert.That(r.Ctl.Candidates[0].Choices, Is.EqualTo(new[] { "chapter2", Restore.TipChoice }));
            r.Ctl.Choose(0, "chapter2");
            r.Ctl.Confirm();
            await r.Submitter.Idle();
            r.Ctl.Poll();
            Assert.That(r.Ctl.State, Is.EqualTo("done"));
            Assert.That(Owned(r.E), Is.EqualTo(new[] { "chapter2" }));
        }

        [Test]
        public async Task RestoreWaitsUntilEveryChoiceIsAnswered()
        {
            var r = Setup(new List<JsonObject> { Log(5_000_000) });
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.CanConfirm, Is.False);
            int jobs = r.Submitter.Jobs.Count;
            r.Ctl.Confirm(); // e.g. a click that raced the screen: nothing happens
            Assert.That(r.Ctl.State, Is.EqualTo("choose"));
            Assert.That(r.Submitter.Jobs.Count, Is.EqualTo(jobs), "nothing submitted");
            r.Ctl.Choose(0, Restore.TipChoice);
            Assert.That(r.Ctl.CanConfirm, Is.True);
        }

        [Test]
        public async Task ErrorsReturnToInput()
        {
            var r = Setup();
            await Run(r.Ctl, r.Submitter, "nonsense");
            Assert.That(r.Ctl.State, Is.EqualTo("input"));
            Assert.That(r.Ctl.Error, Is.EqualTo("invalid_hash"));
            Assert.That(r.Submitter.Jobs, Is.Empty, "an invalid hash is not looked up");
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.State, Is.EqualTo("input"));
            Assert.That(r.Ctl.Error, Is.EqualTo("tx_not_found"));
            Assert.That(r.Ctl.Message(), Is.Not.Null.And.Not.Empty);
        }

        [Test]
        public async Task Reverted()
        {
            var r = Setup(null, "0x0");
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.Error, Is.EqualTo("tx_reverted"));
            Assert.That(r.Ctl.State, Is.EqualTo("input"));
        }

        [Test]
        public void CancelResult()
        {
            var r = Setup();
            Assert.That(r.Ctl.Result().Status, Is.EqualTo("cancelled"));
        }

        [Test]
        public void ABlockedPasteShowsTheRestoreWording()
        {
            var r = Setup();
            r.Ctl.Error = "paste_blocked";
            Assert.That(r.Ctl.Message(), Does.Contain("Type the hash instead"));
        }

        [Test]
        public void RestoreAcceptsSignatures()
        {
            var submitted = new List<Func<Engine, Task<object?>>>();
            var ctl = new RestoreController(fn =>
            {
                submitted.Add(fn);
                return new Job();
            });
            Assert.That(ctl.Check(SigOf(7)), Is.True);
            Assert.That(submitted, Is.Not.Empty, "the lookup was submitted");
            Assert.That(ctl.State, Is.EqualTo("checking"));
            Assert.That(ctl.Check("nonsense"), Is.False);
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
        }

        [Test]
        public void ACancelledJobLandsInFailed()
        {
            var job = new Job();
            job.Cancel();
            var ctl = new RestoreController(_ => job);
            Assert.That(ctl.Check(Tx), Is.True);
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("input"));
            Assert.That(ctl.Error, Is.EqualTo("failed"));
            Assert.That(ctl.Message(), Is.EqualTo("Something went wrong. Please try again."));
        }

        // --- the extras of godot/tests/test_restore_flow.gd ---------------------------------

        [Test]
        public void MessagesAndResultShapes()
        {
            Assert.That(RestoreFlow.Messages.Keys, Is.EqualTo(new[]
            {
                "invalid_hash", "tx_not_found", "tx_reverted", "tx_not_a_payment", "network_unavailable", "paste_blocked", "failed",
            }));
            var ctl = Setup().Ctl;
            Assert.That(ctl.Message(), Is.Null, "no error, no message");
            ctl.Error = "unheard_of";
            Assert.That(ctl.Message(), Is.Null, "an unknown code has no text (Python MESSAGES.get)");
            ctl.Error = "";
            Assert.That(ctl.Message(), Is.Null, "an empty code is no error");
            ctl.Error = "tx_reverted";
            var res = ctl.Result();
            Assert.That(res.Status, Is.EqualTo("cancelled"));
            Assert.That(res.Error, Is.EqualTo("tx_reverted"));
            Assert.That(res.Restored, Is.Empty);
        }

        /// <summary>A controller whose lookup job is the one <paramref name="make"/> builds, already polled.</summary>
        private static RestoreController WithJob(Func<Job> make)
        {
            var ctl = new RestoreController(_ => make());
            ctl.TxInput = Tx;
            ctl.Check();
            ctl.Poll();
            return ctl;
        }

        private static Job Failed(Exception e)
        {
            var job = new Job();
            job.SetRunningOrNotifyCancel();
            job.SetException(e);
            return job;
        }

        private static Job Succeeded(object? value)
        {
            var job = new Job();
            job.SetRunningOrNotifyCancel();
            job.SetResult(value);
            return job;
        }

        [Test]
        public void AFailureThatIsNotARestoreErrorIsFailed()
        {
            var ctl = WithJob(() => Failed(new ValueError("boom")));
            Assert.That((ctl.State, ctl.Error), Is.EqualTo(("input", "failed")));
        }

        [Test]
        public void ARestoreErrorCodeIsTheError()
        {
            var ctl = WithJob(() => Failed(new RestoreError("network_unavailable")));
            Assert.That((ctl.State, ctl.Error), Is.EqualTo(("input", "network_unavailable")));
            Assert.That(ctl.Message(), Is.EqualTo(RestoreFlow.Messages["network_unavailable"]));
        }

        [Test]
        public async Task ASuccessThatIsNotAListOfCandidatesIsFailed()
        {
            var values = new object?[]
            {
                null, "x", new JsonObject { { "a", 1L } }, 3L, new List<object?> { 1L },
                new List<RestoreCandidate?> { null }, new List<JsonObject>(),
            };
            foreach (var v in values)
            {
                var ctl = WithJob(() => Succeeded(v));
                Assert.That((ctl.State, ctl.Error), Is.EqualTo(("input", "failed")), "lookup result " + Py.TypeName(v));
            }
            // the apply phase too: its job is swapped for one that succeeds with no list of receipts
            foreach (var v in new object?[] { null, new List<object?> { 1L }, new List<JsonObject?> { null } })
            {
                var r = Setup(new List<JsonObject> { Log(4_993_742) });
                Job? swapped = null;
                var ctl2 = new RestoreController(fn =>
                {
                    if (swapped == null) return r.Submitter.Submit(fn); // the real lookup
                    return swapped;
                });
                ctl2.TxInput = Tx;
                ctl2.Check();
                await r.Submitter.Idle();
                swapped = Succeeded(v);
                ctl2.Poll();
                Assert.That(ctl2.State, Is.EqualTo("saving"));
                ctl2.Poll();
                Assert.That((ctl2.State, ctl2.Error), Is.EqualTo(("input", "failed")), "apply result " + Py.TypeName(v));
            }
        }

        // --- C# additions ---------------------------------------------------------------

        [Test]
        public async Task ChoicesAreRenumberedPastTheAlreadyRestoredCandidates()
        {
            // the first transfer of Tx is restored first; then the receipt shows a second, ambiguous one. The player's
            // choice for candidate 1 is candidate 0 of the restore_apply call (Python's index_map).
            var r = Setup(new List<JsonObject> { Log(4_993_742) });
            await Run(r.Ctl, r.Submitter);
            Assert.That(r.Ctl.Result().Status, Is.EqualTo("restored"));
            var logs = (List<object?>)r.Rig.Evm("arbitrum").Receipts[Tx]["logs"]!;
            logs.Add(Log(5_000_000, 1));
            var ctl = new RestoreController(r.Submitter.Submit);
            ctl.TxInput = Tx;
            ctl.Check();
            await r.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("choose"));
            Assert.That(ctl.Candidates.Select(c => c.AlreadyRestored), Is.EqualTo(new[] { true, false }));
            Assert.That(ctl.PendingChoices(), Is.EqualTo(new[] { 1 }));
            ctl.Choose(1, Restore.TipChoice);
            ctl.Confirm();
            Assert.That(ctl.State, Is.EqualTo("saving"));
            await r.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("done"));
            Assert.That(ctl.Added.Count, Is.EqualTo(1));
            Assert.That(ctl.Added[0]["kind"], Is.EqualTo("tip"));
            Assert.That(ctl.Result().Status, Is.EqualTo("restored"));
            var restored = ctl.Result().Restored;
            restored.Clear();
            Assert.That(ctl.Added.Count, Is.EqualTo(1), "Result() hands out a copy of the list");
        }

        [Test]
        public void CheckUsesTheTypedTextUnlessGivenOne()
        {
            var submitted = 0;
            var ctl = new RestoreController(_ =>
            {
                submitted++;
                return new Job();
            });
            ctl.TxInput = "  " + Tx + "  ";
            Assert.That(ctl.Check(), Is.True, "the typed text, normalized");
            Assert.That(ctl.Check(""), Is.False, "an empty text is not the typed one");
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
            Assert.That(submitted, Is.EqualTo(1));
        }

        [Test]
        public void ConfirmOutsideCheckingOrChooseDoesNothing()
        {
            var submitted = 0;
            var ctl = new RestoreController(_ =>
            {
                submitted++;
                return new Job();
            });
            ctl.Confirm();
            Assert.That(ctl.State, Is.EqualTo("input"));
            Assert.That(submitted, Is.EqualTo(0));
        }

        [Test]
        public void ASubmitIsRequired()
        {
            Assert.Throws<TypeError>(() => new RestoreController(null!));
        }

        [Test]
        public void PollWithNothingPendingOrAJobStillRunningDoesNothing()
        {
            var job = new Job();
            var ctl = new RestoreController(_ => job);
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("input"));
            ctl.Check(Tx);
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("checking"));
            job.SetRunningOrNotifyCancel();
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("checking"));
        }

        /// <summary>
        /// The texts equal restore_flow.py's MESSAGES (the reference, read by tools/check_text_ids.py) and
        /// restore_flow.gd's (read by tools/make_pot.py), key for key and in order.
        /// </summary>
        [TestCase("plugin/game/python-packages/scfg_core/restore_flow.py", "MESSAGES = {")]
        [TestCase("godot/addons/scfg/core/restore_flow.gd", "const MESSAGES := {")]
        public void MessagesEqualTheTwins(string rel, string opener)
        {
            var root = Vectors.FindUp(Path.GetDirectoryName(rel)!.Replace('\\', '/'));
            var lines = File.ReadAllLines(Path.Combine(root, rel));
            int start = Array.FindIndex(lines, l => l.StartsWith(opener, StringComparison.Ordinal));
            Assert.That(start, Is.GreaterThanOrEqualTo(0), rel + ": no " + opener);
            var entry = new Regex("^\\s+\"([a-z_]+)\": \"((?:[^\"\\\\]|\\\\.)*)\",$");
            var got = new List<KeyValuePair<string, string>>();
            for (int i = start + 1; i < lines.Length && !lines[i].StartsWith("}", StringComparison.Ordinal); i++)
            {
                var m = entry.Match(lines[i]);
                Assert.That(m.Success, Is.True, rel + ": an entry this test cannot read: " + lines[i]);
                Assert.That(m.Groups[2].Value, Does.Not.Contain("\\"), rel + ": an escape this test does not decode");
                got.Add(new KeyValuePair<string, string>(m.Groups[1].Value, m.Groups[2].Value));
            }
            Assert.That(RestoreFlow.Messages.ToList(), Is.EqualTo(got));
        }
    }
}
