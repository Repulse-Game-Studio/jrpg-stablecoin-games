// No Python twin: replays every Tier 2 vector through Tier2 (conformance/js/tier2.test.ts's checks), plus checks that
// the runner itself catches a vector that does not match.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class Tier2Tests
    {
        private static IEnumerable<string> Ids() => Vectors.Tier2Ids();

        internal static JsonObject Load(string id) => Tier2.Parse(Vectors.ReadText("tier2/" + id + ".json"));

        [TestCaseSource(nameof(Ids))]
        public async Task Vector(string id)
        {
            var diff = await Tier2.Replay(Load(id));
            Assert.That(diff, Is.Null, id + ": " + diff);
        }

        [Test]
        public void EveryGroupHasVectors()
        {
            var ids = Vectors.Tier2Ids().ToList();
            foreach (var prefix in new[] { "smoke/", "evm/", "config/", "custom_chains/", "rpc/", "solana/", "reverify/", "restore/" })
                Assert.That(ids.Any(id => id.StartsWith(prefix, StringComparison.Ordinal)), prefix);
        }

        /// <summary>
        /// Both runners, over every Tier 1 topic and every Tier 2 vector: none is ignored or inconclusive. A vector
        /// that fails also fails this test (NUnit records the assertion even though it is thrown here).
        /// </summary>
        [Test]
        public async Task NoVectorIgnored()
        {
            var skipped = new List<string>();
            int ran = 0;
            foreach (var topic in Vectors.Tier1Topics())
            {
                try
                {
                    new Tier1Tests().Topic(topic);
                    ran++;
                }
                catch (IgnoreException e)
                {
                    skipped.Add("tier1/" + topic + " ignored: " + e.Message);
                }
                catch (InconclusiveException e)
                {
                    skipped.Add("tier1/" + topic + " inconclusive: " + e.Message);
                }
            }
            foreach (var id in Vectors.Tier2Ids())
            {
                try
                {
                    await new Tier2Tests().Vector(id);
                    ran++;
                }
                catch (IgnoreException e)
                {
                    skipped.Add("tier2/" + id + " ignored: " + e.Message);
                }
                catch (InconclusiveException e)
                {
                    skipped.Add("tier2/" + id + " inconclusive: " + e.Message);
                }
            }
            Assert.That(skipped, Is.Empty, skipped.Count + " skipped:\n" + string.Join("\n", skipped));
            Assert.That(ran, Is.EqualTo(Vectors.Tier1Topics().Count() + Vectors.Tier2Ids().Count()));
        }

        // ---- the runner catches a vector that does not match ------------------------------------------------------

        private static JsonObject Step(JsonObject v, int i) => (JsonObject)((List<object?>)v["steps"]!)[i]!;

        private static List<object?> Calls(JsonObject v, int i) => (List<object?>)Step(v, i)["calls"]!;

        private static async Task<string?> Tampered(Action<JsonObject> change)
        {
            var v = Load("smoke/evm_unlock_paid");
            Assert.That(await Tier2.Replay(v), Is.Null, "the untouched vector passes");
            v = Load("smoke/evm_unlock_paid");
            change(v);
            return await Tier2.Replay(v);
        }

        [Test]
        public async Task AMissingRecordedCallIsAnUnexpectedCall()
        {
            var diff = await Tampered(v => Calls(v, 1).RemoveAt(2));
            Assert.That(diff, Does.StartWith("step 1 (tick): unexpected call https://base-sepolia-rpc.publicnode.com eth_getBlockByNumber"));
        }

        [Test]
        public async Task AnUnusedRecordedCallFailsTheStep()
        {
            var diff = await Tampered(v => Calls(v, 0).Add(Calls(v, 0)[0]));
            Assert.That(diff, Does.StartWith("step 0 (start_session): expected call not made: https://base-sepolia-rpc.publicnode.com eth_blockNumber"));
        }

        [Test]
        public async Task ParamsMustMatch()
        {
            var diff = await Tampered(v => ((List<object?>)((JsonObject)Calls(v, 1)[2]!)["params"]!)[1] = 0L); // false is not 0
            Assert.That(diff, Does.Contain("unexpected call").And.Contain("expected call not made"));
        }

        [Test]
        public async Task EntropyMustMatchInOrder()
        {
            var diff = await Tampered(v => ((JsonObject)((List<object?>)Step(v, 0)["entropy"]!)[0]!)["n"] = 99L);
            Assert.That(diff, Does.Contain("entropy mismatch: drew randbelow(100), recorded randbelow(99)"));
            diff = await Tampered(v => ((List<object?>)Step(v, 0)["entropy"]!).Add(new JsonObject { { "fn", "token_hex" }, { "n", 8L }, { "value", "00" } }));
            Assert.That(diff, Does.Contain("unconsumed entropy"));
            diff = await Tampered(v => ((List<object?>)Step(v, 0)["entropy"]!).RemoveAt(1));
            Assert.That(diff, Does.Contain("entropy exhausted at token_hex(8)"));
        }

        [Test]
        public async Task RaisesReturnsErrorsAndSnapshotAreCompared()
        {
            Assert.That(await Tampered(v => Step(v, 0)["raises"] = new JsonObject { { "type", "other" } }),
                        Does.StartWith("step 0 (start_session): raises"));
            Assert.That(await Tampered(v => ((JsonObject)Step(v, 0)["returns"]!)["nonce"] = 7L),
                        Does.StartWith("step 0 (start_session): returns: nonce"));
            Assert.That(await Tampered(v => Step(v, 1)["internal_errors"] = 1L),
                        Does.StartWith("step 1 (tick): internal_errors: recorded 1, got 0"));
            Assert.That(await Tampered(v => ((JsonObject)Step(v, 2)["snapshot"]!)["tip_total"] = "0.0"),
                        Does.StartWith("step 2 (tick): snapshot: tip_total"));
        }

        [Test]
        public async Task IdenticalCallsTakeTheirCopiesInRecordedOrder()
        {
            JsonObject Call(string result) => new JsonObject
            {
                { "url", "u" }, { "method", "m" }, { "params", new List<object?> { new JsonObject { { "a", 1L }, { "b", true } } } },
                { "response_text", "{\"result\": \"" + result + "\"}" },
            };
            var t = new ReplayTransport();
            t.Load(new List<object?> { Call("first"), Call("second") });
            // key order is ignored and 1 equals 1.0
            var payload = new JsonObject { { "id", 9L }, { "method", "m" }, { "params", new List<object?> { new JsonObject { { "b", true }, { "a", 1.0 } } } } };
            Assert.That(((JsonObject)(await t.PostJson("u", payload, 1))!)["result"], Is.EqualTo("first"));
            Assert.That(((JsonObject)(await t.PostJson("u", payload, 1))!)["result"], Is.EqualTo("second"));
            Assert.That(t.Leftover(), Is.Empty);
            Assert.That(t.Problems, Is.Empty);
            // true is not 1
            t.Load(new List<object?> { Call("x") });
            var wrong = new JsonObject { { "method", "m" }, { "params", new List<object?> { new JsonObject { { "a", 1L }, { "b", 1L } } } } };
            Assert.ThrowsAsync<ReplayError>(() => t.PostJson("u", wrong, 1));
            Assert.That(t.Problems, Has.Count.EqualTo(1));
        }

        [Test]
        public void ARecordedTransportErrorIsTheTransportsFailure()
        {
            var t = new ReplayTransport();
            t.Load(new List<object?>
            {
                new JsonObject
                {
                    { "url", "u" }, { "method", "m" }, { "params", new List<object?>() },
                    { "transport_error", new JsonObject { { "message", "HTTP 503" }, { "status", 503L } } },
                },
            });
            var e = Assert.ThrowsAsync<TransportError>(() => t.PostJson("u", new JsonObject { { "method", "m" }, { "params", new List<object?>() } }, 1));
            Assert.That(e!.Message, Is.EqualTo("HTTP 503"));
            Assert.That(e.Status, Is.EqualTo(503));
        }

        // ---- restore candidates (R4) ------------------------------------------------------------------------------

        [Test]
        public void RecordedRestoreCandidatesRoundTrip()
        {
            int seen = 0;
            foreach (var id in Vectors.Tier2Ids())
            {
                foreach (var s in (List<object?>)Load(id)["steps"]!)
                {
                    var step = (JsonObject)s!;
                    if ((string)step["action"]! != "restore_lookup" || !(step["returns"] is List<object?> returns)) continue;
                    foreach (var c in returns)
                    {
                        var back = Tier2.CandidateToJson(Tier2.CandidateFromJson((JsonObject)c!));
                        Assert.That(JsonEq.FirstDiff(c, back), Is.Null, id);
                        seen++;
                    }
                }
            }
            Assert.That(seen, Is.GreaterThan(0));
        }

        [Test]
        public void CandidateFromJsonRefusesTheWrongClassOrFields()
        {
            JsonObject Candidate(JsonObject transfer) => new JsonObject
            {
                { "chain", "base" }, { "token", "USDC" }, { "transfer", transfer }, { "kind", null }, { "item", null },
                { "choices", new List<object?> { "tip" } }, { "already_restored", false },
            };
            var transfer = new JsonObject
            {
                { "token", "t" }, { "sender", "s" }, { "recipient", "r" }, { "value", 5L }, { "tx_hash", "h" },
                { "log_index", 0L }, { "block_number", 1L }, { "class", "Transfer" },
            };
            Assert.That(Tier2.CandidateFromJson(Candidate(transfer)).Transfer, Is.InstanceOf<Transfer>());
            var wrongClass = new JsonObject();
            foreach (var kv in transfer) wrongClass[kv.Key] = kv.Value;
            wrongClass["class"] = "Nope";
            Assert.Throws<KeyError>(() => Tier2.CandidateFromJson(Candidate(wrongClass)));
            var log = new JsonObject();
            foreach (var kv in transfer) log[kv.Key] = kv.Value;
            log["class"] = "TransferLog"; // without block_hash
            Assert.Throws<TypeError>(() => Tier2.CandidateFromJson(Candidate(log)));
        }
    }
}
