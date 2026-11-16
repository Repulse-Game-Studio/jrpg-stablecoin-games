// No Python twin: checks of Backends/Solana.cs below what the Tier 2 vectors pin (they replay its reverify,
// readiness and restore through the Engine). transfer_to's cases were checked against the Python reference.
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
        // ---- reverify ----------------------------------------------------------------------------------------------

        private const string BigSlotTx = "5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn";
        private const string InfiniteAmountTx = "4yo3LtPZ3x5oz5XTid5pbHMdERC9JTsrFGfpruW3YG5m9a1ox5PQYV663q3BfHExK2shap9pKt31TYzho5GCCJXQ";

        private static void Tamper(JsonObject call, string from, string to)
        {
            var text = (string)call["response_text"]!;
            Assert.That(text.Split(new[] { from }, StringSplitOptions.None).Length, Is.EqualTo(2), from);
            call["response_text"] = text.Replace(from, to);
        }

        /// <summary>
        /// solana/reverify_receipts with two node answers changed, and the outcome the Python reference gives them
        /// (checked with replay.run_action): a transaction slot of 2**64 is a slot the receipt's is not, so its receipt
        /// is dropped; an infinite token amount is an OverflowError, which Python's reverify contains as one internal
        /// error, keeping the receipt.
        /// </summary>
        [Test]
        public async Task NodeDataBeyondSixtyFourBitsGetsPythonsVerdict()
        {
            var v = Tier2Tests.Load("solana/reverify_receipts");
            var step = (JsonObject)((List<object?>)v["steps"]!)[0]!;
            foreach (var c in (List<object?>)step["calls"]!)
            {
                var call = (JsonObject)c!;
                if ((string)call["method"]! != "getTransaction") continue;
                var sig = ((List<object?>)call["params"]!)[0];
                if (Equals(sig, BigSlotTx)) Tamper(call, "\"slot\": 950", "\"slot\": 18446744073709551616");
                if (Equals(sig, InfiniteAmountTx)) Tamper(call, "\"amount\": \"995006258\"", "\"amount\": Infinity");
            }
            var returns = (List<object?>)step["returns"]!;
            var key = "solana:" + BigSlotTx + ":2";
            returns.Insert(3, key); // store order
            step["internal_errors"] = 1L;
            var receipts = (JsonObject)((JsonObject)step["snapshot"]!)["receipts"]!;
            Assert.That(receipts.Remove(key));
            Assert.That(receipts.ContainsKey("solana:" + InfiniteAmountTx + ":2"));
            Assert.That(receipts.Count, Is.EqualTo(1));
            ((JsonObject)step["snapshot"]!)["tip_total"] = "4.993742"; // the one tip left
            Assert.That(await Tier2.Replay(v), Is.Null);
        }

        // ---- readiness ---------------------------------------------------------------------------------------------

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
