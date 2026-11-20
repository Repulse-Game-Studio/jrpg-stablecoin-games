// Twin of tests/unit/test_watcher_awaiting.py::test_hint_verdict_survives_a_network_failure_later_in_the_step,
// for Watcher.Advance (EVM) and SolanaBackend.Advance: a verdict on the hint reached earlier in a step is
// reported when the rest of the step then fails, with a network failure or with Abandoned (the tick yielding to a
// command, WorkerPriorityTests). The hint is already cleared, so a later step would never report it. The rest of the
// watcher is pinned by the Tier 2 vectors.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    public class WatcherTests
    {
        private const string Dev = "0x52908400098527886E0F7030069857D2E4169EE7";
        private const string SolDev = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";
        private const string Url = "https://a.rpc";
        private static readonly string Tx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
        private const string Sig = "5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn";
        private static readonly IReadOnlyDictionary<string, Item> NoItems = new Dictionary<string, Item>();

        /// <summary>The failure of the step's next request: the network, or the worker refusing it (Abandoned).</summary>
        private static Exception Failure(string how) =>
            how == "network" ? new TransportError("fake outage") : (Exception)new Abandoned("a command waits");

        private static Handler Node(string verdictMethod, object? verdictResult, string how) => (m, p) =>
            m == verdictMethod ? Fakes.Ok(verdictResult) : throw Failure(how);

        [TestCase("network")]
        [TestCase("abandoned")]
        public async Task AnEvmHintVerdictSurvivesAFailureLaterInTheStep(string how)
        {
            var chain = Chains.Mainnet["arbitrum"];
            var pending = new JsonObject();
            var rec = Sessions.CreateSession(pending, "unlock", "chapter2", "Chapter 2", "USDC", "arbitrum", "4.99", 37, 995, 0L,
                                             42, null, SystemEntropy.Instance);
            var sid = (string)rec["id"]!;
            Sessions.SetHint(pending, sid, Tx, 0L);
            var receipt = new JsonObject { { "status", "0x0" }, { "blockNumber", "0x3e8" }, { "logs", new List<object?>() } };
            var t = new FakeTransport(new Dictionary<string, Handler> { { Url, Node("eth_getTransactionReceipt", receipt, how) } });
            var rpc = new RpcClient(new[] { Url }, t, clock: () => 1000);
            var output = await Watcher.Advance(pending, new JsonObject(), sid, rpc, chain, Dev, NoItems, 10);
            Assert.That(t.Calls.Select(c => c.Method), Is.EqualTo(new[] { "eth_getTransactionReceipt", "eth_blockNumber" }),
                        "the scan asked for the head after the verdict");
            Assert.That((output.Status, output.Error, output.HintTx), Is.EqualTo(("waiting", (string?)"tx_reverted", (string?)Tx)));
            Assert.That(rec["hint_tx"], Is.Null);
        }

        [TestCase("network")]
        [TestCase("abandoned")]
        public async Task ASolanaHintVerdictSurvivesAFailureLaterInTheStep(string how)
        {
            var chain = Chains.Mainnet["solana"];
            var pending = new JsonObject();
            var extra = new JsonObject
            {
                { "reference", SolDev }, { "dest", SolDev }, { "scan_cursor", null }, { "scan_before", null },
            };
            var rec = Sessions.CreateSession(pending, "unlock", "chapter2", "Chapter 2", "USDC", "solana", "4.99", 37, 995, 0L,
                                             42, extra, SystemEntropy.Instance);
            var sid = (string)rec["id"]!;
            var backend = new SolanaBackend();
            Sessions.SetHint(pending, sid, Sig, 0L, backend.NormalizeTx);
            var tx = new JsonObject { { "slot", 1000L }, { "meta", new JsonObject { { "err", new JsonObject { { "InstructionError", 0L } } } } } };
            var t = new FakeTransport(new Dictionary<string, Handler> { { Url, Node("getTransaction", tx, how) } });
            var rpc = new SolanaRpc(new RpcClient(new[] { Url }, t, clock: () => 1000));
            var output = await backend.Advance(pending, new JsonObject(), sid, rpc, chain, SolDev, NoItems, 10);
            Assert.That(t.Calls.Select(c => c.Method), Is.EqualTo(new[] { "getTransaction", "getSignaturesForAddress" }),
                        "the reference lookup ran after the verdict");
            Assert.That((output.Status, output.Error, output.HintTx), Is.EqualTo(("waiting", (string?)"tx_reverted", (string?)Sig)));
            Assert.That(rec["hint_tx"], Is.Null);
        }

        [Test]
        public void WithoutAVerdictAbandonedGoesUpToTheTick()
        {
            var chain = Chains.Mainnet["arbitrum"];
            var pending = new JsonObject();
            var rec = Sessions.CreateSession(pending, "unlock", "chapter2", "Chapter 2", "USDC", "arbitrum", "4.99", 37, 995, 0L,
                                             42, null, SystemEntropy.Instance);
            var t = new FakeTransport(new Dictionary<string, Handler> { { Url, Node("none", null, "abandoned") } });
            var rpc = new RpcClient(new[] { Url }, t, clock: () => 1000);
            Assert.ThrowsAsync<Abandoned>(() => Watcher.Advance(pending, new JsonObject(), (string)rec["id"]!, rpc, chain, Dev, NoItems, 10));
        }
    }
}
