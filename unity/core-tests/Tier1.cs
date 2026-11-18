// No Python twin: the Tier 1 runner, replaying conformance/vectors/tier1/*.json against the C# core.
// Modeled on conformance/js/tier1.ts. Funcs maps the Python function name of a case (its "fn") to an adapter
// from the JSON-friendly input to a JSON-friendly output, registered in one block per topic group under RegisterAll.
// Every topic runs: a case whose fn has no adapter is reported as a failure.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public static class Tier1
    {
        /// <summary>Adapters keyed by Python <c>fn</c>. Outputs are JSON values (string, long, bool, JsonObject, List).</summary>
        public static readonly Dictionary<string, Func<JsonObject, object?>> Funcs =
            new Dictionary<string, Func<JsonObject, object?>>(StringComparer.Ordinal);

        static Tier1()
        {
            RegisterAll();
        }

        // One block per topic group, in conformance/js/tier1.ts's order.
        private static void RegisterAll()
        {
            RegisterAmounts();
            RegisterKeccak();
            RegisterEvm();
            RegisterBase58();
            RegisterSolanaTx();
            RegisterConfig();
            RegisterReceipts();
            RegisterPaylinks();
            RegisterPageMessages();
            RegisterRpcErrors();
            RegisterTxIds();
        }

        // ---- shared input helpers --------------------------------------------------------------------------

        /// <summary>
        /// Shape check for vector inputs, so a malformed input is a ValueError, not an incidental TypeError.
        /// </summary>
        internal static string Str(JsonObject i, string key)
        {
            if (i.TryGetValue(key, out var v) && v is string s) return s;
            throw new ValueError("vector input " + key + " must be a string");
        }

        /// <summary>
        /// Token units: vectors write them as decimal integer strings, read with Python's <c>int(str)</c>. Anything
        /// else is a TypeError, as in tier1.ts.
        /// </summary>
        internal static BigInteger Units(object? v)
        {
            if (!(v is string s)) throw new TypeError("vector units must be a decimal string");
            return Py.Int(s);
        }

        /// <summary>A vector value: <c>{"float": "&lt;repr&gt;"}</c> is that float, anything else is itself (replay.py's decode_value).</summary>
        internal static object? VectorValue(object? v)
        {
            if (v is JsonObject o && o.Count == 1 && o.TryGetValue("float", out var repr) && repr is string text)
                return double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
            return v;
        }

        private static string Str(Dec d) => d.ToString();

        // ---- amounts ---------------------------------------------------------------------------------------

        private static void RegisterAmounts()
        {
            Funcs["parse_amount"] = i =>
            {
                var type = i["type"] as string;
                object? value = type == "float" ? 4.99 : type == "bool" ? (object)Py.Truthy(i["value"]) : i["value"];
                return Str(Amounts.ParseAmount(value));
            };
            Funcs["encode"] = i => Amounts.Encode(i["price"], i["code"], i["nonce"]).ToString(CultureInfo.InvariantCulture);
            Funcs["decode"] = i =>
            {
                var (price, code, nonce) = Amounts.Decode(Units(i["units"]));
                return new JsonObject { { "price", Str(price) }, { "code", (long)code }, { "nonce", (long)nonce } };
            };
            Funcs["units_to_decimal"] = i => Str(Amounts.UnitsToDecimal(Units(i["units"])));
            Funcs["decimal_to_units"] = i =>
            {
                var d = Dec.Parse(Str(i, "amount")) ?? throw new ValueError("amount must be finite");
                return Amounts.DecimalToUnits(d).ToString(CultureInfo.InvariantCulture);
            };
            Funcs["format_units"] = i => Amounts.FormatUnits(Units(i["units"]));
            Funcs["stored_amount"] = i => Str(Amounts.StoredAmount(VectorValue(i["value"])));
        }

        // ---- keccak ----------------------------------------------------------------------------------------

        private static void RegisterKeccak()
        {
            Funcs["keccak256"] = i => Bytes.ToHex(Keccak.Keccak256(Bytes.FromHex(Str(i, "data_hex"))));
        }

        // ---- evm_address (the address fields are passed through untouched: non-strings are Python's errors) --

        private static void RegisterEvm()
        {
            Funcs["to_checksum_address"] = i => Evm.ToChecksumAddress(i.Get("address"));
            Funcs["is_checksum_address"] = i => Evm.IsChecksumAddress(i.Get("address"));
            Funcs["normalize_address"] = i => Evm.NormalizeAddress(i.Get("address"));
            Funcs["address_topic"] = i => Evm.AddressTopic(i.Get("address"));
            Funcs["parse_transfer_log"] = i =>
            {
                if (!(i.Get("log") is JsonObject log)) throw new ValueError("vector input log must be an object");
                var t = Evm.ParseTransferLog(log);
                if (t == null) return null;
                return new JsonObject
                {
                    { "token", t.Token }, { "sender", t.Sender }, { "recipient", t.Recipient },
                    { "value", t.Value.ToString(CultureInfo.InvariantCulture) },
                    { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block_number", t.BlockNumber },
                    { "block_hash", t.BlockHash },
                };
            };
        }

        // ---- base58 and ed25519 (landed with the config port, which needs them; text is passed through untouched) --

        private static void RegisterBase58()
        {
            Funcs["b58encode"] = i => Solana.Base58.B58Encode(Bytes.FromHex(Str(i, "data_hex")));
            Funcs["b58decode"] = i => Bytes.ToHex(Solana.Base58.B58Decode(i.Get("text")));
            Funcs["decode_fixed"] = i => Bytes.ToHex(Solana.Base58.DecodeFixed(i.Get("text"), i.Get("length")));
            Funcs["normalize_pubkey"] = i => Solana.Base58.NormalizePubkey(i.Get("text"));
            Funcs["normalize_signature"] = i => Solana.Base58.NormalizeSignature(i.Get("text"));
            Funcs["is_on_curve"] = i => Solana.Ed25519.IsOnCurve(Bytes.FromHex(Str(i, "point_hex")));
        }

        // ---- pda and solana_tx -----------------------------------------------------------------------------------

        private static byte[][] Seeds(JsonObject i)
        {
            var list = Arr(i.Get("seeds_hex"), "seeds_hex");
            var seeds = new byte[list.Count][];
            for (int k = 0; k < seeds.Length; k++)
                seeds[k] = Bytes.FromHex(list[k] as string ?? throw new ValueError("vector seed must be a string"));
            return seeds;
        }

        private static bool BoolField(JsonObject i, string key) =>
            i.Get(key) is bool b ? b : throw new ValueError("vector input " + key + " must be a boolean");

        private static void RegisterSolanaTx()
        {
            Funcs["create_program_address"] = i =>
            {
                var o = Solana.Pda.CreateProgramAddress(Seeds(i), Solana.Base58.DecodeFixed(Str(i, "program_id"), 32));
                return o == null ? null : Solana.Base58.B58Encode(o);
            };
            Funcs["find_program_address"] = i =>
            {
                var (address, bump) = Solana.Pda.FindProgramAddress(Seeds(i), Solana.Base58.DecodeFixed(Str(i, "program_id"), 32));
                return new JsonObject { { "address", Solana.Base58.B58Encode(address) }, { "bump", (long)bump } };
            };
            Funcs["associated_token_address"] = i => Solana.Pda.AssociatedTokenAddress(Str(i, "owner"), Str(i, "mint"));
            Funcs["compact_u16"] = i =>
            {
                var n = i.Get("n");
                if (n is BigInteger) throw new ValueError("compact-u16 out of range");
                if (!(n is long l)) throw new TypeError("compact-u16 needs a number");
                return Bytes.ToHex(Solana.Tx.CompactU16(l));
            };
            Funcs["compile_message"] = i =>
            {
                var ixs = new List<Solana.Instruction>();
                foreach (var xo in Arr(i.Get("instructions"), "instructions"))
                {
                    var x = Obj(xo, "instruction");
                    var metas = new List<Solana.AccountMeta>();
                    foreach (var mo in Arr(x.Get("accounts"), "accounts"))
                    {
                        var m = Obj(mo, "account");
                        metas.Add(new Solana.AccountMeta(Str(m, "pubkey"), BoolField(m, "is_signer"), BoolField(m, "is_writable")));
                    }
                    ixs.Add(new Solana.Instruction(Str(x, "program_id"), metas.ToArray(), Bytes.FromHex(Str(x, "data_hex"))));
                }
                return Bytes.ToHex(Solana.Tx.CompileMessage(Str(i, "payer"), ixs, Str(i, "recent_blockhash")));
            };
            Funcs["unsigned_transaction"] = i =>
                Bytes.ToHex(Solana.Tx.UnsignedTransaction(Bytes.FromHex(Str(i, "message_hex"))));
        }

        // ---- chains, config, custom_chains, items, payments --------------------------------------------------
        // Inputs follow conformance/python/scfg_conformance/tier1_config.py: a Config is the kwargs of build_config
        // (a custom chain inside it the kwargs of build_chain, a non-object entry is passed through untouched), items
        // are lists of add_item kwargs, and {"float": "<repr>"} is a float. Outputs are to_jsonable of the dataclasses.

        private static JsonObject Obj(object? v, string what) =>
            v as JsonObject ?? throw new ValueError("vector input " + what + " must be an object");

        private static List<object?> Arr(object? v, string what) =>
            v as List<object?> ?? throw new ValueError("vector input " + what + " must be a list");

        private static List<object?> Strs(IEnumerable<string> xs) => xs.Cast<object?>().ToList();

        private static JsonObject TokenJson(Token t) => new JsonObject
        {
            { "address", t.Address }, { "decimals", (long)t.Decimals },
            { "log_address", t.LogAddress }, { "log_decimals", (long)t.LogDecimals },
        };

        private static JsonObject ChainJson(Chain c)
        {
            var tokens = new JsonObject();
            foreach (var kv in c.Tokens) tokens[kv.Key] = TokenJson(kv.Value);
            return new JsonObject
            {
                { "key", c.Key }, { "chain_id", c.ChainId }, { "name", c.Name }, { "rpc", Strs(c.Rpc) },
                { "confirmations", c.Confirmations }, { "max_log_range", c.MaxLogRange },
                { "explorer_tx", c.ExplorerTx }, { "native_symbol", c.NativeSymbol }, { "fee_hint", c.FeeHint },
                { "tokens", tokens }, { "testnet", c.Testnet }, { "family", c.Family },
                { "wallet_chain", c.WalletChain }, { "native_token", c.NativeToken }, { "min_fee_wei", c.MinFeeWei },
            };
        }

        private static JsonObject ChainsJson(IReadOnlyDictionary<string, Chain> chains)
        {
            var o = new JsonObject();
            foreach (var kv in chains) o[kv.Key] = ChainJson(kv.Value);
            return o;
        }

        private static JsonObject ItemJson(Item it) => new JsonObject
        {
            { "item_id", it.ItemId }, { "code", (long)it.Code }, { "price", Str(it.Price) }, { "token", it.Token },
            { "title", it.Title }, { "min_price", Str(it.MinPrice) },
        };

        private static JsonObject ConfigJson(ConfigData c)
        {
            var rpcUrls = new JsonObject();
            foreach (var kv in c.RpcUrls) rpcUrls[kv.Key] = Strs(kv.Value);
            var confirmations = new JsonObject();
            foreach (var kv in c.Confirmations) confirmations[kv.Key] = kv.Value;
            var items = new JsonObject();
            foreach (var kv in c.Items) items[kv.Key] = ItemJson(kv.Value);
            return new JsonObject
            {
                { "wallet", c.Wallet }, { "testnet", c.Testnet }, { "chains", Strs(c.Chains) },
                { "tokens", Strs(c.Tokens) }, { "methods", c.Methods }, { "rpc_urls", rpcUrls },
                { "confirmations", confirmations }, { "previous_wallets", Strs(c.PreviousWallets) },
                { "solana_wallet", c.SolanaWallet }, { "previous_solana_wallets", Strs(c.PreviousSolanaWallets) },
                { "logo", c.Logo }, { "accent", c.Accent }, { "custom_chains", ChainsJson(c.CustomChains) },
                { "onramp", new JsonObject
                    { { "enabled", c.Onramp.Enabled }, { "url", c.Onramp.Url }, { "chains", Strs(c.Onramp.Chains) } } },
                { "items", items },
            };
        }

        private static JsonObject SpecJson(PaymentSpec s) => new JsonObject
        {
            { "kind", s.Kind }, { "item", s.Item }, { "price", Str(s.Price) }, { "token", s.Token },
            { "code", (long)s.Code }, { "title", s.Title }, { "chains", Strs(s.Chains) },
        };

        private static JsonObject Copy(JsonObject o)
        {
            var c = new JsonObject();
            foreach (var kv in o) c[kv.Key] = kv.Value;
            return c;
        }

        private static ConfigData ConfigFrom(JsonObject kwargs)
        {
            var kw = Copy(kwargs);
            if (kw.ContainsKey("custom_chains"))
                kw["custom_chains"] = Arr(kw["custom_chains"], "custom_chains")
                    .Select(e => e is JsonObject o ? Config.BuildChain(o) : e).ToList();
            return Config.BuildConfig(kw);
        }

        /// <summary>An item (or payment) kwargs object with every value decoded: item_id apart from the rest.</summary>
        private static (object? ItemId, JsonObject Kwargs) ItemKwargs(JsonObject d)
        {
            var rest = new JsonObject();
            foreach (var kv in d) rest[kv.Key] = VectorValue(kv.Value);
            var id = rest["item_id"];
            rest.Remove("item_id");
            return (id, rest);
        }

        private static ConfigData WithItems(JsonObject configKw, List<object?> items)
        {
            var config = ConfigFrom(configKw);
            foreach (var item in items)
            {
                var (id, rest) = ItemKwargs(Obj(item, "item"));
                Config.AddItem(config, id, rest);
            }
            return config;
        }

        private static void RegisterConfig()
        {
            Funcs["get_chains"] = i => ChainsJson(Chains.GetChains(Py.Truthy(i["testnet"])));
            Funcs["build_chain"] = i => ChainJson(Config.BuildChain(i));
            Funcs["build_config"] = i => ConfigJson(ConfigFrom(i));
            Funcs["add_item"] = i =>
            {
                var config = WithItems(Obj(i["config"], "config"), i.TryGetValue("existing", out var e) ? Arr(e, "existing") : new List<object?>());
                var (id, rest) = ItemKwargs(Obj(i["item"], "item"));
                return ItemJson(Config.AddItem(config, id, rest));
            };
            Funcs["prepare_payment"] = i =>
            {
                var config = WithItems(Obj(i["config"], "config"), i.TryGetValue("items", out var e) ? Arr(e, "items") : new List<object?>());
                var kw = new JsonObject();
                foreach (var k in new[] { "item", "amount", "token", "title" })
                    if (i.TryGetValue(k, out var v)) kw[k] = VectorValue(v);
                return SpecJson(Payments.PreparePayment(config, i["kind"], kw));
            };
        }

        // ---- receipts ---------------------------------------------------------------------------------------

        private static JsonObject Store(JsonObject i) => Copy(Obj(i["store"], "store"));

        private static Dec FiniteDec(object? v) =>
            (v is string s ? Dec.Parse(s) : null) ?? throw new ValueError("vector decimal must be a finite decimal string");

        private static long IntField(JsonObject i, string key) =>
            i.Get(key) is long n ? n : throw new ValueError("vector input " + key + " must be a number");

        private static void RegisterPaylinks()
        {
            Funcs["eip681_transfer_uri"] = i => Paylinks.Eip681TransferUri(Str(i, "token_address"), IntField(i, "chain_id"),
                Str(i, "recipient"), Units(i["units"]));
            Funcs["onramp_amount"] = i => Paylinks.OnrampAmount(Units(i["units"]),
                i.Get("gas_is_token") is bool gas ? gas : throw new ValueError("vector input gas_is_token must be a bool"));
            Funcs["onramp_url"] = i => Paylinks.OnrampUrl(Str(i, "template"), Str(i, "chain"), IntField(i, "chain_id"),
                Str(i, "token"), Str(i, "token_address"), Str(i, "amount"));
            Funcs["solana_pay_uri"] = i => Solana.SolanaPaylinks.SolanaPayUri(Str(i, "recipient"), Units(i["units"]), Str(i, "mint"),
                Str(i, "reference"), Str(i, "label"), Str(i, "message"));
            Funcs["qr_matrix"] = i =>
            {
                var rows = new List<object?>();
                foreach (var row in Paylinks.QrMatrix(Str(i, "text")))
                {
                    var sb = new System.Text.StringBuilder(row.Length);
                    foreach (bool m in row) sb.Append(m ? '1' : '0');
                    rows.Add(sb.ToString()); // culture-ok: StringBuilder
                }
                return rows;
            };
            Funcs["qr_rects"] = i =>
            {
                var (size, runs) = Paylinks.QrRects(Str(i, "text"), (int)IntField(i, "border"));
                var rects = new List<object?>();
                foreach (var r in runs) rects.Add(new List<object?> { (long)r.X, (long)r.Y, (long)r.Width });
                return new JsonObject { { "size", (long)size }, { "rects", rects } };
            };
        }

        // ---- page_messages ---------------------------------------------------------------------------------

        private static void RegisterPageMessages()
        {
            Funcs["parse_tx_message"] = i =>
            {
                var (txHash, sender) = PageMessages.ParseTxMessage(i.Get("data"));
                return new List<object?> { txHash, sender };
            };
            Funcs["parse_build_message"] = i => PageMessages.ParseBuildMessage(i.Get("data"));
        }

        // ---- tx_ids (normalize_signature is registered with base58) ------------------------------------------

        private static void RegisterTxIds()
        {
            Funcs["normalize_tx_hash"] = i => Sessions.NormalizeTxHash(i.Get("text"));
            Funcs["normalize_any_tx"] = i => Backends.NormalizeAnyTx(i.Get("text"));
        }

        // ---- rpc_errors: numbers read out of a node's error message ----------------------------------------

        private static void RegisterRpcErrors()
        {
            Funcs["suggested_range"] = i => Rpc.SuggestedRange(new RpcError(-32005, Str(i, "message")), IntField(i, "tried"));
            // Python's _named_version reads only the client's tx_version
            Funcs["named_version"] = i =>
                Solana.SolanaRpcErrors.NamedVersion(new RpcError(IntField(i, "code"), Str(i, "message")), IntField(i, "tx_version"));
        }

        private static void RegisterReceipts()
        {
            Funcs["receipt_key"] = i => Receipts.ReceiptKey(Str(i, "chain"), Str(i, "tx_hash"),
                i["log_index"] is long n ? n : throw new ValueError("vector input log_index must be an int"));
            Funcs["make_receipt"] = i =>
            {
                var t = Obj(i["transfer"], "transfer");
                var cls = t.Get("class") as string;
                if (cls != "Transfer" && cls != "TransferLog") throw new ValueError("unknown transfer class " + Py.Repr((object?)cls));
                string chain = Str(i, "chain"), kind = Str(i, "kind"), token = Str(i, "token"), source = Str(i, "source");
                var item = i["item"] as string;
                string txHash = Str(t, "tx_hash"), sender = Str(t, "sender"), recipient = Str(t, "recipient"), ttoken = Str(t, "token");
                long logIndex = (long)t["log_index"]!, block = (long)t["block_number"]!;
                var value = Units(t["value"]);
                return cls == "Transfer"
                    ? Receipts.MakeReceipt(chain, new Transfer(ttoken, sender, recipient, value, txHash, logIndex, block), kind, item, token, source)
                    : Receipts.MakeReceipt(chain, new TransferLog(ttoken, sender, recipient, value, txHash, logIndex, block, Str(t, "block_hash")), kind, item, token, source);
            };
            Funcs["add_receipt"] = i =>
            {
                var store = Store(i);
                var added = Receipts.AddReceipt(store, Obj(i["receipt"], "receipt"));
                return new JsonObject { { "added", added }, { "store", store } };
            };
            Funcs["remove_receipt"] = i =>
            {
                var store = Store(i);
                Receipts.RemoveReceipt(store, Str(i, "key"));
                return store;
            };
            Funcs["compute_ownership"] = i =>
            {
                var items = new Dictionary<string, Item>(StringComparer.Ordinal);
                foreach (var e in Arr(i["items"], "items"))
                {
                    var x = Obj(e, "item");
                    var id = Str(x, "item_id");
                    items[id] = new Item(id, (int)(long)x["code"]!, FiniteDec(x["price"]), Str(x, "token"), Str(x, "title"), FiniteDec(x["min_price"]));
                }
                var o = Receipts.ComputeOwnership(Store(i), items);
                return new JsonObject { { "owned", Strs(o.Owned) }, { "tip_total", Str(o.TipTotal) } };
            };
        }

        // ---- the runner ------------------------------------------------------------------------------------

        /// <summary>The cases of a topic, as the vector file holds them.</summary>
        public static List<JsonObject> Cases(string topic)
        {
            var doc = Json.Loads(Vectors.ReadText("tier1/" + topic + ".json")) as JsonObject
                      ?? throw new InvalidOperationException("tier1/" + topic + ".json is not an object");
            if (!(doc.Get("topic") is string named) || named != topic)
                throw new InvalidOperationException("tier1/" + topic + ".json names topic " + Json.Dumps(doc.Get("topic")));
            if (!(doc.Get("cases") is List<object?> cases))
                throw new InvalidOperationException("tier1/" + topic + ".json has no cases list");
            return cases.Select(c => c as JsonObject
                                     ?? throw new InvalidOperationException("tier1/" + topic + ".json: a case is not an object"))
                .ToList();
        }

        /// <summary>Every failure message of a topic's cases, in file order (empty when all pass).</summary>
        public static List<string> RunTopic(string topic)
        {
            var failures = new List<string>();
            var cases = Cases(topic);
            for (int n = 0; n < cases.Count; n++)
            {
                var diff = RunCase(cases[n]);
                if (diff != null) failures.Add("#" + n.ToString(CultureInfo.InvariantCulture) + " " + diff);
            }
            return failures;
        }

        /// <summary>
        /// Recomputes a case through <see cref="Funcs"/>: null when it agrees with the vector, otherwise a message
        /// naming the first difference. An "error" with a dot is a ConfigError with that code; "invalid" is a
        /// ValueError or TypeError that is not a ConfigError. Any other exception fails the case (stricter than
        /// Python on purpose), and so does an fn without an adapter.
        /// </summary>
        public static string? RunCase(JsonObject c)
        {
            var fn = c.Get("fn") as string ?? "<no fn>";
            var input = c.Get("in");
            if (!Funcs.TryGetValue(fn, out var f)) return "no C# adapter for " + fn;
            if (!(input is JsonObject args)) return fn + ": the vector's in is not an object";

            var got = new JsonObject();
            try
            {
                got["out"] = f(args);
            }
            catch (ConfigError e)
            {
                got["error"] = e.Code;
            }
            catch (Exception e) when (e is ValueError || e is TypeError)
            {
                got["error"] = "invalid";
            }
            catch (Exception e)
            {
                return fn + "(" + Show(input) + "): raised " + e.GetType().Name + ": " + e.Message
                       + " (only ValueError, TypeError and ConfigError are rejections)";
            }

            var want = new JsonObject();
            if (c.TryGetValue("error", out var error) && error != null) want["error"] = error;
            else want["out"] = c.Get("out");

            var diff = JsonEq.FirstDiff(got, want);
            if (diff == null) return null;
            return fn + "(" + Show(input) + "): " + diff + " (got " + Show(got) + ", vector says " + Show(want) + ")";
        }

        private static string Show(object? v)
        {
            string s;
            try { s = Json.Dumps(v, ensureAscii: false); }
            catch (Exception) { s = "<" + (v?.GetType().Name ?? "null") + ">"; }
            return s.Length > 300 ? s.Substring(0, 297) + "..." : s;
        }
    }
}
