// No Python twin: checks of Backends/Solana.cs that the enabled Tier 2 vectors cannot reach yet. The Engine's
// reverify (task 12) is what calls the backend's Reverify and CheckReady, so the Solana reverify and readiness vectors
// are replayed here straight through the backend, as reverify_receipts and Engine._check_ready would call it for
// well-formed Solana receipts. transfer_to's cases were checked against the Python reference.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class SolanaBackendTests
    {
        // ---- reverify and readiness, against the recorded vectors -------------------------------------------------

        [TestCase("solana/readiness")]
        [TestCase("solana/readiness_crash")]
        [TestCase("solana/reverify_receipts")]
        [TestCase("solana/reverify_outage_and_future_slot")]
        [TestCase("solana/reverify_unreadable_transactions")]
        [TestCase("solana/history_covering_endpoint_drops")]
        [TestCase("solana/history_found_on_second_endpoint")]
        [TestCase("solana/history_pruned_endpoints_keep")]
        public async Task ReverifyAndReadinessAgreeWithTheVector(string id)
        {
            var v = Tier2Tests.Load(id);
            var setup = (JsonObject)v["setup"]!;
            long now = (long)setup["clock"]!;
            int errors = 0;
            var transport = new ReplayTransport();
            var engine = Tier2.BuildEngine(setup, transport, () => now, _ => errors++, new StepEntropy());
            var wallets = engine.Config.AcceptedWalletsFor("solana");
            var steps = (List<object?>)v["steps"]!;
            for (int i = 0; i < steps.Count; i++)
            {
                var step = (JsonObject)steps[i]!;
                var where = id + " step " + i;
                Assert.That(step["action"], Is.EqualTo("reverify"), where);
                now = (long)step["at"]!;
                errors = 0;
                transport.Load((List<object?>)step["calls"]!);

                // reverify_receipts for well-formed receipts on a known chain with wallets: the backend's verdict, and
                // an exception is reported and keeps the receipt
                var removed = new List<object?>();
                foreach (var kv in engine.Receipts.ToList())
                {
                    var receipt = (JsonObject)kv.Value!;
                    Assert.That(receipt["chain"], Is.EqualTo("solana"), where + ": the replay covers Solana receipts only");
                    var chain = engine.Chain("solana");
                    Assert.That(chain.Tokens.ContainsKey((string)receipt["token"]!) && wallets.Count > 0, where);
                    string verdict;
                    try
                    {
                        verdict = await new SolanaBackend().Reverify(receipt, engine.Rpc("solana"), chain, wallets);
                    }
                    catch (Exception)
                    {
                        errors++;
                        continue;
                    }
                    if (verdict == "drop")
                    {
                        engine.Receipts.Remove(kv.Key);
                        removed.Add(kv.Key);
                    }
                }

                // Engine._check_ready
                var unready = new List<(string Chain, string Token)>();
                foreach (var kv in Config.EnabledChains(engine.Config))
                {
                    Dictionary<string, bool> ready;
                    try
                    {
                        ready = await Backends.BackendFor(kv.Value).CheckReady(() => engine.Rpc(kv.Key), kv.Value,
                                                                               engine.Config.WalletFor(kv.Value));
                    }
                    catch (Exception e) when (e is RpcUnavailable || e is RpcError)
                    {
                        continue;
                    }
                    catch (Exception)
                    {
                        errors++;
                        continue;
                    }
                    foreach (var r in ready)
                        if (!r.Value) unready.Add((kv.Key, r.Key));
                }
                unready.Sort((x, y) =>
                {
                    int c = Py.CompareCodePoints(x.Chain, y.Chain);
                    return c != 0 ? c : Py.CompareCodePoints(x.Token, y.Token);
                });

                Assert.That(transport.Problems.Concat(transport.Leftover()), Is.Empty, where);
                Assert.That(JsonEq.FirstDiff(step["returns"], removed), Is.Null, where + ": removed keys");
                Assert.That(errors, Is.EqualTo((long)step["internal_errors"]!), where + ": internal errors");
                var recordedUnready = ((JsonObject)step["snapshot"]!)["unready"];
                var got = unready.Select(p => (object?)new List<object?> { p.Chain, p.Token }).ToList();
                Assert.That(JsonEq.FirstDiff(recordedUnready, got), Is.Null, where + ": unready");
                var recordedReceipts = (JsonObject)((JsonObject)step["snapshot"]!)["receipts"]!;
                Assert.That(engine.Receipts.Keys, Is.EqualTo(recordedReceipts.Keys), where + ": receipts kept");
            }
        }

        [Test]
        public void AnAccountInfoResultWithoutValueIsAKeyErrorNotAnRpcError()
        {
            // tier2/solana/readiness_crash: the Engine contains it (reported, the route stays ready)
            var v = Tier2Tests.Load("solana/readiness_crash");
            var setup = (JsonObject)v["setup"]!;
            var transport = new ReplayTransport();
            var engine = Tier2.BuildEngine(setup, transport, () => 1700000000, _ => { }, new StepEntropy());
            transport.Load((List<object?>)((JsonObject)((List<object?>)v["steps"]!)[0]!)["calls"]!);
            var chain = engine.Chain("solana");
            Assert.ThrowsAsync<KeyError>(() => new SolanaBackend().CheckReady(() => engine.Rpc("solana"), chain, engine.Config.SolanaWallet));
        }

        // ---- transfer_to (results from the Python reference) -------------------------------------------------------

        private const string Mint = "MINT";
        private const string Owner = "OWNER";

        private static JsonObject Bal(long i, object? owner, long amount, string mint = Mint) => new JsonObject
        {
            { "accountIndex", i }, { "mint", mint }, { "owner", owner },
            { "uiTokenAmount", new JsonObject { { "amount", amount.ToString(System.Globalization.CultureInfo.InvariantCulture) } } },
        };

        private static JsonObject Tx(List<object?> pre, List<object?> post, List<object?>? keys = null, object? err = null)
        {
            var tx = new JsonObject
            {
                { "slot", 7L },
                { "meta", new JsonObject { { "err", err }, { "preTokenBalances", pre }, { "postTokenBalances", post } } },
            };
            if (keys != null) tx["transaction"] = new JsonObject { { "message", new JsonObject { { "accountKeys", keys } } } };
            return tx;
        }

        private static List<object?> L(params object?[] xs) => xs.ToList();

        private static Transfer? Read(JsonObject tx) => SolanaBackend.TransferTo(tx, "S", Mint, Owner);

        private static void AssertTransfer(Transfer? t, string? payer, long value, long index)
        {
            Assert.That(t, Is.Not.Null);
            Assert.That(t!.Sender, Is.EqualTo(payer));
            Assert.That(t.Value, Is.EqualTo(new BigInteger(value)));
            Assert.That(t.LogIndex, Is.EqualTo(index));
            Assert.That(t.BlockNumber, Is.EqualTo(7));
            Assert.That(t.Recipient, Is.EqualTo(Owner));
            Assert.That(t.TxHash, Is.EqualTo("S"));
            Assert.That(t.Token, Is.EqualTo(Mint));
        }

        [Test]
        public void ThePayerIsTheFirstOfTheLargestDrops()
        {
            AssertTransfer(Read(Tx(L(Bal(0, "A", 50), Bal(1, "B", 50), Bal(2, Owner, 0), Bal(3, "C", 10)),
                                   L(Bal(0, "A", 20), Bal(1, "B", 20), Bal(2, Owner, 70), Bal(3, "C", 0)))), "A", 70, 2);
            // drops are summed per owner
            AssertTransfer(Read(Tx(L(Bal(0, "A", 10), Bal(1, "B", 15), Bal(3, "A", 10)),
                                   L(Bal(0, "A", 0), Bal(1, "B", 0), Bal(3, "A", 0), Bal(2, Owner, 35)))), "A", 35, 2);
        }

        [Test]
        public void WithoutDropsThePayerIsTheFeePayerOrNone()
        {
            AssertTransfer(Read(Tx(L(Bal(2, Owner, 0)), L(Bal(2, Owner, 5)), keys: L("FEE", "X"))), "FEE", 5, 2);
            AssertTransfer(Read(Tx(L(Bal(2, Owner, 0)), L(Bal(2, Owner, 5)), keys: L(new JsonObject { { "pubkey", "FEEP" } }))), "FEEP", 5, 2);
            AssertTransfer(Read(Tx(L(Bal(2, Owner, 0)), L(Bal(2, Owner, 5)))), null, 5, 2);
        }

        [Test]
        public void TheIndexIsTheFirstAccountThatGained()
        {
            AssertTransfer(Read(Tx(L(Bal(4, Owner, 3)), L(Bal(1, Owner, 0), Bal(4, Owner, 10), Bal(5, Owner, 2)))), null, 9, 4);
        }

        [Test]
        public void NoTransferForAFailedTxAnotherMintOrALoss()
        {
            Assert.That(Read(Tx(L(Bal(2, Owner, 0)), L(Bal(2, Owner, 5)), err: new JsonObject { { "x", 1L } })), Is.Null);
            Assert.That(Read(Tx(L(Bal(2, Owner, 0, "Z")), L(Bal(2, Owner, 5, "Z")))), Is.Null);
            Assert.That(Read(Tx(L(Bal(2, Owner, 9)), L(Bal(2, Owner, 5)))), Is.Null);
        }

        [Test]
        public void MalformedRepliesAreNotRpcErrors()
        {
            var owner = new JsonObject { { "a", 1L } };
            Assert.Throws<TypeError>(() => Read(Tx(L(Bal(0, owner, 5)), L(Bal(0, owner, 1), Bal(2, Owner, 4))))); // unhashable
            Assert.Throws<TypeError>(() => Read(new JsonObject { { "slot", 1L }, { "meta", "x" } })); // AttributeError
            Assert.Throws<TypeError>(() => Read(new JsonObject
            {
                { "slot", 1L }, { "meta", new JsonObject { { "err", null }, { "postTokenBalances", "ab" } } },
            })); // AttributeError
        }
    }
}
