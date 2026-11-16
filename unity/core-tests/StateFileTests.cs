// No Python twin: tests of StateFile (the state schema checks of conformance/python/scfg_conformance/state_schema.py
// and the load rules of godot/addons/scfg/store.gd). The problem texts were checked against state_schema.check.
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class StateFileTests
    {
        // Two vectors store malformed receipts on purpose (reverify removes them); every other store conforms.
        private static readonly string[] MalformedSetups =
            { "reverify/invalid_receipts_are_removed", "reverify/malformed_entries_are_removed" };

        private static JsonObject Store(JsonObject? holder, string key) =>
            holder != null && holder.TryGetValue(key, out var v) && v is JsonObject o ? o : new JsonObject();

        [Test]
        public void TheTableIsTheFrozenSchema()
        {
            var root = Vectors.FindUp("conformance/schema");
            var text = File.ReadAllText(Path.Combine(root, "conformance", "schema", "state.json"), new UTF8Encoding(false));
            var file = Json.Loads(text);
            Assert.That(JsonEq.FirstDiff(StateSchema.ToJson(), file), Is.Null);
            Assert.That(Json.Dumps(StateSchema.ToJson()), Is.EqualTo(Json.Dumps(file)), "same sections and fields, in the same order");
        }

        [Test]
        public void EveryTier2StoreConforms()
        {
            int stores = 0;
            foreach (var id in Vectors.Tier2Ids())
            {
                var v = Tier2Tests.Load(id);
                var setup = v["setup"] as JsonObject;
                var problems = StateFile.Check(Store(setup, "pending"), Store(setup, "receipts"));
                if (MalformedSetups.Contains(id)) Assert.That(problems, Is.Not.Empty, id);
                else Assert.That(problems, Is.Empty, id + " setup");
                stores++;
                foreach (var s in (List<object?>)v["steps"]!)
                {
                    if (!(((JsonObject)s!).TryGetValue("snapshot", out var snap) && snap is JsonObject snapshot)) continue;
                    Assert.That(StateFile.Check(Store(snapshot, "pending"), Store(snapshot, "receipts")), Is.Empty, id + " snapshot");
                    stores++;
                }
            }
            Assert.That(stores, Is.GreaterThan(500));
        }

        [Test]
        public void TheMalformedVectorStoresGetPythonsProblems()
        {
            var v = Tier2Tests.Load("reverify/malformed_entries_are_removed");
            var setup = (JsonObject)v["setup"]!;
            var want = new List<string>();
            foreach (var f in new[] { "amount", "block", "chain", "item", "kind", "log_index", "payer", "recipient", "source", "token", "tx_hash" })
                want.Add("receipts[junk0]: missing '" + f + "'");
            foreach (var f in new[] { "amount", "block", "item", "kind", "log_index", "payer", "recipient", "source", "token", "tx_hash" })
                want.Add("receipts[junk1]: missing '" + f + "'");
            foreach (var f in new[] { "item", "kind", "payer", "recipient", "source" })
                want.Add("receipts[junk2]: missing '" + f + "'");
            want.Add("receipts[junk2].log_index: expected int, got 'x'");
            Assert.That(StateFile.Check(Store(setup, "pending"), Store(setup, "receipts")), Is.EqualTo(want));

            v = Tier2Tests.Load("reverify/invalid_receipts_are_removed");
            setup = (JsonObject)v["setup"]!;
            var got = StateFile.Check(Store(setup, "pending"), Store(setup, "receipts"));
            Assert.That(got.Count, Is.EqualTo(4));
            Assert.That(got[0], Is.EqualTo("receipts[arbitrum:0x018db0cb849bfd12b23c580daedc0e936fc2200c3d8607c1dbbd2fcba1502ae4:2]: key does not match chain:tx_hash:log_index"));
            Assert.That(got[2], Is.EqualTo("receipts[arbitrum:0x1f322d55230dbfc91798571fea8b86a53bd681c2cb80d54fcce89fb72e670311:2].token: expected str, got ['USDC']"));
            Assert.That(got[3], Does.StartWith("receipts[arbitrum:0x9560d5633ebe59a9064130e03812205c30173fd59d5e663ee9beb32470ed2564:2].log_index: expected int, got '9999"));
        }

        // ---- per-rule problems -------------------------------------------------------------------------------------

        private const string Tx = "0x00000000000000000000000000000000000000000000000000000000000000ab";

        private static JsonObject PendingRec(string id = "ps_1") => new JsonObject
        {
            { "id", id }, { "kind", "unlock" }, { "item", "sword" }, { "title", "Sword" }, { "price", "4.99" },
            { "token", "USDC" }, { "chain", "base" }, { "code", 1L }, { "nonce", 7L }, { "units", 4990107L },
            { "created_at", 1700000000L }, { "start_block", 100L }, { "cursor", 100L }, { "state", "awaiting" },
            { "match", null }, { "hint_tx", null }, { "hint_at", null }, { "ui_status", "open" },
        };

        private static JsonObject MatchRec() => new JsonObject
        {
            { "tx_hash", Tx }, { "log_index", 2L }, { "block", 120L }, { "payer", null }, { "recipient", "0xabc" },
            { "value", 4990107L }, { "block_hash", "0x" + new string('c', 64) },
        };

        private static JsonObject ReceiptRec(string tx = Tx, long logIndex = 2) => new JsonObject
        {
            { "chain", "base" }, { "tx_hash", tx }, { "log_index", logIndex }, { "block", 120L }, { "kind", "unlock" },
            { "item", "sword" }, { "amount", "4.990107" }, { "token", "USDC" }, { "payer", "0xdef" },
            { "recipient", "0xabc" }, { "source", "payment" },
        };

        private static List<string> CheckPending(JsonObject rec, string key = "ps_1") =>
            StateFile.Check(new JsonObject { { key, rec } }, new JsonObject());

        private static List<string> CheckReceipt(JsonObject rec, string? key = null) =>
            StateFile.Check(new JsonObject(), new JsonObject { { key ?? "base:" + Tx + ":2", rec } });

        [Test]
        public void GoodRecordsConform()
        {
            Assert.That(CheckPending(PendingRec()), Is.Empty);
            var withMatch = PendingRec();
            withMatch["match"] = MatchRec();
            withMatch["state"] = "confirming";
            withMatch["created_at"] = 1700000000.25; // a number: int or float
            withMatch["hint_at"] = 1700000001L;
            Assert.That(CheckPending(withMatch), Is.Empty);
            var solana = PendingRec();
            solana["dest"] = "D";
            solana["reference"] = "R";
            solana["scan_before"] = null;
            solana["scan_cursor"] = "sig";
            Assert.That(CheckPending(solana), Is.Empty);
            Assert.That(CheckReceipt(ReceiptRec()), Is.Empty);
            Assert.That(CheckReceipt(ReceiptRec("0xAB"), "base:0xab:2"), Is.Empty, "a 0x hash is keyed lowercased");
            Assert.That(CheckReceipt(ReceiptRec("SigAB"), "base:SigAB:2"), Is.Empty, "a Solana signature keeps its case");
            var bigIndex = ReceiptRec();
            bigIndex["log_index"] = BigInteger.Pow(2, 70);
            Assert.That(CheckReceipt(bigIndex, "base:" + Tx + ":1180591620717411303424"), Is.Empty, "a Python int is unbounded");
        }

        [Test]
        public void EachRuleHasPythonsText()
        {
            var r = PendingRec();
            r.Remove("nonce");
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1]: missing 'nonce'" }));

            r = PendingRec();
            r["colour"] = "red";
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1]: unknown 'colour'" }));

            r = PendingRec();
            r["kind"] = "gift";
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].kind: expected one of ['unlock', 'tip'], got 'gift'" }));

            r = PendingRec();
            r["units"] = true; // booleans are never ints
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].units: expected int, got True" }));

            r = PendingRec();
            r["units"] = "5";
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].units: expected int, got '5'" }));

            r = PendingRec();
            r["units"] = 5.0;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].units: expected int, got 5.0" }));

            r = PendingRec();
            r["price"] = 4.99;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].price: expected str, got 4.99" }));

            r = PendingRec();
            r["created_at"] = false;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].created_at: expected number, got False" }));

            r = PendingRec();
            r["hint_at"] = "soon";
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].hint_at: expected number|null, got 'soon'" }));

            r = PendingRec();
            r["title"] = null;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].title: expected str, got None" }));

            r = PendingRec();
            var m = MatchRec();
            m.Remove("block");
            r["match"] = m;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].match: missing 'block'" }));

            r = PendingRec();
            m = MatchRec();
            m["extra"] = 1L;
            m["value"] = "1";
            r["match"] = m;
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].match.value: expected int, got '1'", "pending[ps_1].match: unknown 'extra'" }));

            r = PendingRec();
            r["match"] = new List<object?> { 1L };
            Assert.That(CheckPending(r), Is.EqualTo(new[] { "pending[ps_1].match: expected object or null, got [1]" }));

            Assert.That(CheckPending(PendingRec("ps_2")), Is.EqualTo(new[] { "pending[ps_1]: id does not match its key" }));
            Assert.That(StateFile.Check(new JsonObject { { "ps_1", "x" } }, new JsonObject()), Is.EqualTo(new[] { "pending[ps_1]: expected object" }));

            Assert.That(CheckReceipt(ReceiptRec(), "base:" + Tx + ":3"),
                        Is.EqualTo(new[] { "receipts[base:" + Tx + ":3]: key does not match chain:tx_hash:log_index" }));
            Assert.That(CheckReceipt(ReceiptRec("0xAB"), "base:0xAB:2"),
                        Is.EqualTo(new[] { "receipts[base:0xAB:2]: key does not match chain:tx_hash:log_index" }));
            var rc = ReceiptRec();
            rc["source"] = "gift";
            rc["block_hash"] = "0x"; // optional in a match, unknown in a receipt
            Assert.That(CheckReceipt(rc), Is.EqualTo(new[]
            {
                "receipts[base:" + Tx + ":2].source: expected one of ['payment', 'restore'], got 'gift'",
                "receipts[base:" + Tx + ":2]: unknown 'block_hash'",
            }));
            rc = ReceiptRec();
            rc.Remove("chain");
            Assert.That(CheckReceipt(rc), Is.EqualTo(new[] { "receipts[base:" + Tx + ":2]: missing 'chain'" }), "no key check when a field is wrong");
        }

        // ---- Parse and Serialize -----------------------------------------------------------------------------------

        private static string Doc(object? version, object? pending, object? receipts)
        {
            var o = new JsonObject { { "version", version }, { "pending", pending }, { "receipts", receipts } };
            return Json.Dumps(o);
        }

        [TestCase("", "is not valid JSON")]
        [TestCase("{\"version\": 1,", "is not valid JSON")]
        [TestCase("﻿{\"version\": 1, \"pending\": {}, \"receipts\": {}}", "is not valid JSON")]
        [TestCase("[]", "is not a JSON object")]
        [TestCase("null", "is not a JSON object")]
        [TestCase("{\"pending\": {}, \"receipts\": {}}", "has an unknown version None")]
        [TestCase("{\"version\": true, \"pending\": {}, \"receipts\": {}}", "has an unknown version True")]
        [TestCase("{\"version\": 1.0, \"pending\": {}, \"receipts\": {}}", "has an unknown version 1.0")]
        [TestCase("{\"version\": \"1\", \"pending\": {}, \"receipts\": {}}", "has an unknown version '1'")]
        [TestCase("{\"version\": 2, \"pending\": {}, \"receipts\": {}}", "has an unknown version 2")]
        [TestCase("{\"version\": 1, \"receipts\": {}}", "has no pending/receipts objects")]
        [TestCase("{\"version\": 1, \"pending\": {}, \"receipts\": []}", "has no pending/receipts objects")]
        [TestCase("{\"version\": 1, \"pending\": null, \"receipts\": {}}", "has no pending/receipts objects")]
        public void ABadFileHasAWhyAndNoStores(string text, string why)
        {
            var load = StateFile.Parse(text);
            Assert.That(load.Why, Is.EqualTo(why));
            Assert.That(load.Pending, Is.Null);
            Assert.That(load.Receipts, Is.Null);
        }

        [Test]
        public void AFileWithABadRecordIsRefusedWhole()
        {
            var bad = PendingRec();
            bad["units"] = "5";
            bad["kind"] = "gift";
            var load = StateFile.Parse(Doc(1L, new JsonObject { { "ps_1", bad } }, new JsonObject { { "base:" + Tx + ":2", ReceiptRec() } }));
            Assert.That(load.Why, Is.EqualTo("does not match the state schema (pending[ps_1].kind: expected one of ['unlock', 'tip'], got 'gift', and 1 more)"));
            Assert.That(load.Pending, Is.Null);
            Assert.That(load.Receipts, Is.Null);

            // the "non-string stored values" divergence (README) cannot come from a file
            var nonString = PendingRec();
            nonString["token"] = 5L;
            Assert.That(StateFile.Parse(Doc(1L, new JsonObject { { "ps_1", nonString } }, new JsonObject())).Why,
                        Is.EqualTo("does not match the state schema (pending[ps_1].token: expected str, got 5)"));
        }

        [Test]
        public void AGoodFileGivesItsStores()
        {
            var load = StateFile.Parse("{\"version\": 1, \"pending\": {}, \"receipts\": {}, \"later\": true}");
            Assert.That(load.Why, Is.Null);
            Assert.That(load.Pending, Is.Empty);
            Assert.That(load.Receipts, Is.Empty);
        }

        [Test]
        public void SerializeRoundTripsInStoreOrder()
        {
            var withMatch = PendingRec("ps_b");
            withMatch["match"] = MatchRec();
            withMatch["created_at"] = 1700000000.123456789;
            var pending = new JsonObject { { "ps_b", withMatch }, { "ps_a", PendingRec("ps_a") } };
            var receipts = new JsonObject { { "base:0xff:9", ReceiptRec("0xFF", 9) }, { "base:" + Tx + ":2", ReceiptRec() } };
            var text = StateFile.Serialize(pending, receipts);
            Assert.That(text, Does.StartWith("{\"version\": 1, \"pending\": {\"ps_b\": {\"id\": \"ps_b\", "));
            var load = StateFile.Parse(text);
            Assert.That(load.Why, Is.Null);
            Assert.That(JsonEq.FirstDiff(load.Pending, pending), Is.Null);
            Assert.That(JsonEq.FirstDiff(load.Receipts, receipts), Is.Null);
            Assert.That(load.Pending!.Keys, Is.EqualTo(new[] { "ps_b", "ps_a" }));
            Assert.That(load.Receipts!.Keys, Is.EqualTo(new[] { "base:0xff:9", "base:" + Tx + ":2" }));
            Assert.That(StateFile.Serialize(new JsonObject(), new JsonObject()), Is.EqualTo("{\"version\": 1, \"pending\": {}, \"receipts\": {}}"));
        }

        [Test]
        public void EveryTier2SnapshotRoundTrips()
        {
            foreach (var id in Vectors.Tier2Ids())
                foreach (var s in (List<object?>)Tier2Tests.Load(id)["steps"]!)
                {
                    if (!(((JsonObject)s!).TryGetValue("snapshot", out var snap) && snap is JsonObject snapshot)) continue;
                    var load = StateFile.Parse(StateFile.Serialize(Store(snapshot, "pending"), Store(snapshot, "receipts")));
                    Assert.That(load.Why, Is.Null, id);
                    Assert.That(JsonEq.FirstDiff(load.Pending, Store(snapshot, "pending")), Is.Null, id);
                    Assert.That(JsonEq.FirstDiff(load.Receipts, Store(snapshot, "receipts")), Is.Null, id);
                }
        }
    }
}
