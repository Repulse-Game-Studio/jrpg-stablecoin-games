// No Python twin: Engine behaviour the Tier 2 vectors do not reach (their runner always passes its own RPC factory
// and error handler): the default factory, detached copies in and out, and a null error handler.
using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class EngineTests
    {
        private const string Wallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

        private static ConfigData BaseConfig()
        {
            var config = Config.BuildConfig(new JsonObject
            {
                { "wallet", Wallet }, { "chains", new List<object?> { "base" } }, { "testnet", true },
            });
            Config.AddItem(config, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            return config;
        }

        private static ScriptedEntropy Draws() => new ScriptedEntropy(new List<object?>
        {
            new JsonObject { { "fn", "randbelow" }, { "n", 100L }, { "value", 6L } },
            new JsonObject { { "fn", "token_hex" }, { "n", 8L }, { "value", "ea4702a60e526f21" } },
        });

        [Test]
        public async Task TheDefaultRpcFactoryUsesTheEnginesTransportAndKeepsOneClientPerChain()
        {
            var url = Chains.Testnet["base"].Rpc[0];
            var transport = new FakeTransport(new Dictionary<string, Handler> { { url, (m, p) => Fakes.Ok("0x3e8") } });
            var entropy = Draws();
            var engine = new Engine(BaseConfig(), transport, null, null, () => 1700000000, entropy: entropy);
            var rec = await engine.StartSession("unlock", "base", new JsonObject { { "item", "chapter2" } });
            Assert.That(rec["start_block"], Is.EqualTo(995L));
            Assert.That(rec["created_at"], Is.EqualTo(1700000000L));
            Assert.That(rec["id"], Is.EqualTo("ps_ea4702a60e526f21"));
            Assert.That(transport.Calls, Has.Count.EqualTo(1));
            Assert.That(transport.Calls[0].Method, Is.EqualTo("eth_blockNumber"));
            Assert.That(engine.Rpc("base"), Is.InstanceOf<RpcClient>());
            Assert.That(engine.Rpc("base"), Is.SameAs(engine.Rpc("base")));
            Assert.That(entropy.Leftover(), Is.Empty);
        }

        [Test]
        public async Task StateIsCopiedInAndOut()
        {
            var pending = new JsonObject();
            var url = Chains.Testnet["base"].Rpc[0];
            var transport = new FakeTransport(new Dictionary<string, Handler> { { url, (m, p) => Fakes.Ok("0x3e8") } });
            var engine = new Engine(BaseConfig(), transport, pending, null, () => 1700000000, entropy: Draws());
            var rec = await engine.StartSession("unlock", "base", new JsonObject { { "item", "chapter2" } });
            Assert.That(pending.Count, Is.EqualTo(0), "the caller's store is not the Engine's");
            rec["state"] = "changed";
            var snap = engine.Snapshot();
            Assert.That(((JsonObject)snap.Pending["ps_ea4702a60e526f21"]!)["state"], Is.EqualTo("awaiting"));
            ((JsonObject)snap.Pending["ps_ea4702a60e526f21"]!)["state"] = "changed";
            Assert.That(((JsonObject)engine.Pending["ps_ea4702a60e526f21"]!)["state"], Is.EqualTo("awaiting"));
        }

        [Test]
        public void AnUnknownKeywordIsATypeError()
        {
            var engine = new Engine(BaseConfig(), new FakeTransport(new Dictionary<string, Handler>()), null, null, () => 0);
            Assert.ThrowsAsync<TypeError>(() => engine.StartSession("unlock", "base", new JsonObject { { "colour", "red" } }));
        }

        private static RestoreCandidate Ambiguous(List<string> choices) => new RestoreCandidate(
            "base", "USDC", new TransferLog("0xT", "0xS", Wallet, 5000000, "0x" + new string('a', 64), 3, 10, "0xb"), null, null,
            choices, false);

        [Test]
        public void ARestoreCandidateIsEqualByValueAndKeepsACopyOfItsChoices()
        {
            var choices = new List<string> { "chapter2", "tip" };
            var c = Ambiguous(choices);
            choices.Add("chapter3");
            Assert.That(c.Choices, Is.EqualTo(new[] { "chapter2", "tip" }));
            Assert.That(c, Is.EqualTo(Ambiguous(new List<string> { "chapter2", "tip" })));
            Assert.That(c, Is.Not.EqualTo(Ambiguous(new List<string> { "tip" })));
        }

        [Test]
        public void RestoreApplyRefusesAChoiceTheCandidateDoesNotOffer()
        {
            var engine = new Engine(BaseConfig(), new FakeTransport(new Dictionary<string, Handler>()), null, null, () => 0);
            var candidates = new List<RestoreCandidate> { Ambiguous(new List<string> { "chapter2", "tip" }) };
            // Python: `5 not in c.choices` is a ValueError, as an item id it does not offer is
            Assert.Throws<ValueError>(() => engine.RestoreApply(candidates, new Dictionary<int, object?> { { 0, 5L } }));
            Assert.Throws<ValueError>(() => engine.RestoreApply(candidates, new Dictionary<int, object?> { { 0, "chapter9" } }));
            Assert.That(engine.RestoreApply(candidates), Is.Empty, "no choice: skipped");
            Assert.That(engine.RestoreApply(candidates, new Dictionary<int, object?> { { 0, null } }), Is.Empty);
            var added = engine.RestoreApply(candidates, new Dictionary<int, object?> { { 0, "tip" } });
            Assert.That(added, Has.Count.EqualTo(1));
            Assert.That(added[0]["kind"], Is.EqualTo("tip"));
            Assert.That(added[0]["source"], Is.EqualTo("restore"));
            Assert.That(engine.RestoreApply(candidates, new Dictionary<int, object?> { { 0, "chapter2" } }), Is.Empty, "already stored");
        }

        [Test]
        public async Task ACrashWithNoHandlerIsStillContained()
        {
            var pending = new JsonObject
            {
                { "ps_bad", new JsonObject { { "chain", "base" }, { "state", "awaiting" }, { "ui_status", "cancelled" } } },
            };
            var engine = new Engine(BaseConfig(), new FakeTransport(new Dictionary<string, Handler>()), pending, null, () => 0);
            engine.OnError = null; // prints instead
            var errors = Console.Error;
            var captured = new System.IO.StringWriter();
            Console.SetError(captured);
            List<StepOutcome> outcomes;
            try
            {
                outcomes = await engine.Tick(); // created_at is missing: a KeyError while stepping
            }
            finally
            {
                Console.SetError(errors);
            }
            Assert.That(outcomes, Has.Count.EqualTo(1));
            Assert.That(outcomes[0].Status, Is.EqualTo("waiting"));
            Assert.That(outcomes[0].Error, Is.EqualTo("internal_error"));
            Assert.That(captured.ToString(), Does.Contain("KeyError"));
        }
    }
}
