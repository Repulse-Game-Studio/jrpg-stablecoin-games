// Twin of scfg_core/backends/solana.py (and of rpgmaker/src/core/backends/solana.ts).
// Solana: payments are found by their Solana Pay reference, by a reported signature, or by scanning the developer's
// token account for the exact encoded amount. A payment counts once the cluster marks its transaction finalized.
//
// What a transaction paid is read from meta.pre/postTokenBalances, never from its instructions, so it works whatever
// program or wallet moved the tokens.
//
// Node replies and stored records are read the way Python reads them: Py.Get for d[k], DictGet for d.get(k) (a
// value that is not a dict has no .get: Python's AttributeError, a TypeError here), Py.Truthy for `or`. A malformed
// reply or record throws a non-RPC error, which the Engine contains. Stored fields that Python sends to the node as
// they are (a hint, a reference, a cursor) are sent unchecked here too.
//
// Restore (Lookup) needs restore.py's classify_or_skip and is ported with it (task 12).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using Scfg.Core.Solana;

namespace Scfg.Core
{
    public sealed class SolanaBackend : IBackend
    {
        /// <summary>Solana's "finalized" corresponds to 32 confirmed blocks.</summary>
        public const long FinalConfirmations = 32;
        /// <summary>getTransaction calls per step for the token-account scan.</summary>
        public const int ScanTxBudget = 5;
        /// <summary>Signatures asked for per getSignaturesForAddress page.</summary>
        public const int SigPage = 100;
        /// <summary>Pages one scan walk reads before it moves <c>scan_before</c> back instead.</summary>
        public const int MaxSigPages = 10;
        public const long StartSlotMargin = 150;
        public const long ComputeUnits = 30_000;
        /// <summary>Micro-lamports per compute unit.</summary>
        public const long MinPriorityFee = 1_000;
        /// <summary>x 30,000 CU = at most 30,000 lamports (0.00003 SOL).</summary>
        public const long MaxPriorityFee = 1_000_000;
        /// <summary>
        /// A wallet returns the signature before the transaction lands, and a transaction whose blockhash expires
        /// (~60-90 s) never lands. A hint unseen for this long was dropped.
        /// </summary>
        public const long SolanaHintTimeoutS = 180;
        /// <summary>reverify.py's FUTURE_TOLERANCE_BLOCKS: a receipt this far past the node's finalized slot is dropped.</summary>
        internal const long FutureToleranceBlocks = 10_000;

        public string Family => "solana";

        public object MakeRpc(Chain chain, ITransport transport) => new SolanaRpc(new RpcClient(chain.Rpc, transport));

        public string NormalizeTx(object? text) => Base58.NormalizeSignature(text);

        /// <summary>The <see cref="SolanaRpc"/> MakeRpc built (Python calls whatever it is handed).</summary>
        internal static SolanaRpc Client(object rpc) =>
            rpc as SolanaRpc ?? throw new TypeError("a Solana chain needs a SolanaRpc, not " + Py.TypeName(rpc));

        // ---- Python's view of node replies ------------------------------------------------------------------------

        /// <summary><c>d.get(key)</c>: a dict's value or null; anything else has no .get (AttributeError, a TypeError here).</summary>
        internal static object? DictGet(object? d, string key) =>
            d is JsonObject o ? o.Get(key) : throw new TypeError("'" + Py.TypeName(d) + "' object has no attribute 'get'");

        /// <summary><c>v or fallback</c></summary>
        private static object? Or(object? v, object? fallback) => Py.Truthy(v) ? v : fallback;

        /// <summary><c>v[0]</c> on a parsed JSON value.</summary>
        private static object? First(object? v)
        {
            switch (v)
            {
                case List<object?> list:
                    if (list.Count == 0) throw new IndexError();
                    return list[0];
                case string s:
                    if (s.Length == 0) throw new IndexError("string index out of range");
                    Py.CodePointAt(s, 0, out int width);
                    return s.Substring(0, width);
                case JsonObject _:
                    throw new KeyError("0"); // a dict has no int key 0
                default:
                    throw new TypeError("'" + Py.TypeName(v) + "' object is not subscriptable");
            }
        }

        /// <summary>
        /// A truthy value Python iterates and then reads with <c>.get</c>: only a list yields rows (a str's characters
        /// and a dict's keys have no .get, and a number is not iterable). A falsy value is no rows.
        /// </summary>
        private static List<object?> Rows(object? v)
        {
            if (!Py.Truthy(v)) return new List<object?>();
            return v as List<object?> ?? throw new TypeError("'" + Py.TypeName(v) + "' items have no attribute 'get'");
        }

        /// <summary>A base58 key that must be a str (Python's b58decode of anything else fails).</summary>
        private static string Key(object? v) => v as string ?? throw new ValueError("not base58: " + Py.Repr(v));

        /// <summary>
        /// Python <c>slot &lt; min_slot</c> for an int slot and a stored start slot: exact against an int or a bool,
        /// and against a float (an int is below a non-integral float when it is at most its floor). Anything else is a
        /// TypeError.
        /// </summary>
        private static bool Below(BigInteger slot, object? minSlot)
        {
            switch (minSlot)
            {
                case double d:
                    if (double.IsNaN(d)) return false;
                    if (double.IsInfinity(d)) return d > 0;
                    var floor = new BigInteger(Math.Floor(d));
                    return Math.Floor(d) == d ? slot < floor : slot <= floor;
                case long _:
                case int _:
                case bool _:
                case BigInteger _:
                    return slot < Py.Int(minSlot);
                default:
                    throw new TypeError("'<' not supported between instances of 'int' and '" + Py.TypeName(minSlot) + "'");
            }
        }

        /// <summary>
        /// Python's dict key equality for a token-balance owner: None, a str, or a number (1, 1.0 and True are one
        /// key, as in a dict). Both are hashable.
        /// </summary>
        private static bool SameKey(object? a, object? b)
        {
            if (a == null || b == null) return a == null && b == null;
            if (a is string x) return b is string y && x == y;
            if (b is string) return false;
            if (a is double da) return b is double db ? da == db : Watcher.IntEquals(Py.Int(b), da);
            return Watcher.IntEquals(Py.Int(a), b);
        }

        /// <summary>The fee payer's pubkey (<c>accountKeys[0]</c>, a str or a <c>{pubkey}</c> entry), or null if there is none.</summary>
        private static object? FeePayer(object? tx)
        {
            var keys = Or(DictGet(Or(DictGet(Or(DictGet(tx, "transaction"), new JsonObject()), "message"), new JsonObject()), "accountKeys"),
                          new List<object?>());
            if (!Py.Truthy(keys)) return null;
            var head = First(keys);
            return head is string ? head : DictGet(head, "pubkey");
        }

        private static Dictionary<long, (object? Owner, BigInteger Amount)> Table(object? meta, string name, string mint)
        {
            var rows = new Dictionary<long, (object?, BigInteger)>();
            foreach (var b in Rows(DictGet(meta, name)))
            {
                if (!(DictGet(b, "mint") is string m && m == mint)) continue;
                // Python evaluates the right-hand side before the subscript key.
                var row = (DictGet(b, "owner"), Ints.IntFrom(Py.Get(Py.Get(b, "uiTokenAmount"), "amount")));
                var idx = Py.Get(b, "accountIndex");
                // Nodes always send an int; Python would also take any other hashable index, which no node sends.
                long index = idx is long l ? l : idx is int i ? i : throw new TypeError("unsupported accountIndex " + Py.Repr(idx));
                rows[index] = row;
            }
            return rows;
        }

        /// <summary>
        /// Python <c>transfer_to</c>: what <paramref name="owner"/> received of <paramref name="mint"/> in a transaction,
        /// from its token-balance changes, or null. The payer is the account that lost the most of the token (the first
        /// of them), else the fee payer, else null.
        /// </summary>
        public static Transfer? TransferTo(object? tx, string sig, string mint, string owner)
        {
            var meta = Or(DictGet(tx, "meta"), new JsonObject());
            if (DictGet(meta, "err") != null) return null;
            var pre = Table(meta, "preTokenBalances", mint);
            var post = Table(meta, "postTokenBalances", mint);
            BigInteger gained = BigInteger.Zero;
            long? index = null;
            var drops = new List<(object? Who, BigInteger Sum)>(); // a dict in insertion order
            var indexes = new SortedSet<long>(pre.Keys.Concat(post.Keys));
            foreach (var idx in indexes)
            {
                var who = (post.TryGetValue(idx, out var p) ? p : pre[idx]).Owner;
                var delta = (post.TryGetValue(idx, out var after) ? after.Amount : BigInteger.Zero)
                            - (pre.TryGetValue(idx, out var before) ? before.Amount : BigInteger.Zero);
                if (who is string w && w == owner)
                {
                    gained += delta;
                    if (delta > 0 && index == null) index = idx;
                }
                else if (delta < 0)
                {
                    if (who is JsonObject || who is List<object?>) throw new TypeError("unhashable type: '" + Py.TypeName(who) + "'");
                    int at = drops.FindIndex(d => SameKey(who, d.Who));
                    if (at < 0) drops.Add((who, delta));
                    else drops[at] = (drops[at].Who, drops[at].Sum + delta);
                }
            }
            if (gained <= 0 || index == null) return null;
            object? payer;
            if (drops.Count > 0)
            {
                var best = drops[0]; // Python min(drops, key=drops.get): the first of the largest drops
                foreach (var d in drops)
                    if (d.Sum < best.Sum) best = d;
                payer = best.Who;
            }
            else
            {
                payer = FeePayer(tx);
            }
            long slot = Scfg.Core.Rpc.ToLong(Ints.IntFrom(Py.Get(tx, "slot")), "slot");
            // Python carries any payer value; a node sends a base58 str (or nothing).
            return new Transfer(mint, Watcher.StrOrNone(payer, "payer"), owner, gained, sig, index.Value, slot);
        }

        private static JsonObject MatchDict(Transfer t) => new JsonObject
        {
            { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block", t.BlockNumber },
            { "value", Watcher.IntValue(t.Value) }, { "payer", t.Sender }, { "recipient", t.Recipient },
        };

        // ---- payment -----------------------------------------------------------------------------------------------

        /// <summary>
        /// Unsigned transfer from the player's wallet, for the browser page to hand to the wallet. The wallet simulates
        /// it and shows the player what it does.
        /// </summary>
        public async Task<JsonObject> BuildPayment(JsonObject rec, object rpc, Chain chain, string account)
        {
            var client = Client(rpc);
            account = Base58.NormalizePubkey(account);
            var mint = Watcher.TokenOf(chain, rec).Address;
            var source = Pda.AssociatedTokenAddress(account, mint);
            var tokenBalance = await client.GetTokenBalance(source);
            var solBalance = await client.GetBalance(account);
            var blockhash = await client.GetLatestBlockhash();
            var fee = await client.GetPriorityFee(new List<object?> { Py.Get(rec, "dest") });
            var price = BigInteger.Min(MaxPriorityFee, BigInteger.Max(MinPriorityFee, fee));
            var instructions = new List<Instruction>
            {
                Tx.SetComputeUnitLimit(ComputeUnits), Tx.SetComputeUnitPrice(price),
                Tx.TransferChecked(source, mint, Key(Py.Get(rec, "dest")), account, Watcher.Units(Py.Get(rec, "units"), "units"),
                                   6, new[] { Key(Py.Get(rec, "reference")) }),
            };
            var tx = Tx.UnsignedTransaction(Tx.CompileMessage(account, instructions, Key(blockhash)));
            return new JsonObject
            {
                { "transaction", Bytes.Base64Encode(tx) },
                { "token_balance", tokenBalance.ToString(CultureInfo.InvariantCulture) },
                { "sol_balance", solBalance.ToString(CultureInfo.InvariantCulture) },
            };
        }

        public async Task<JsonObject> Start(object rpc, Chain chain, string? wallet, string token, IEntropy entropy)
        {
            var client = Client(rpc);
            if (!chain.Tokens.TryGetValue(token, out var tk)) throw new KeyError(token);
            var dest = Pda.AssociatedTokenAddress(Key(wallet), tk.Address);
            long slot = await client.GetSlot();
            var newest = await client.GetSignaturesForAddress(dest, limit: 1);
            return new JsonObject
            {
                { "start_block", Math.Max(0, slot - StartSlotMargin) },
                { "reference", Base58.B58Encode(entropy.TokenBytes(32)) }, { "dest", dest },
                { "scan_cursor", Py.Truthy(newest) ? Py.Get(First(newest), "signature") : null },
                { "scan_before", null },
            };
        }

        // ---- watching ----------------------------------------------------------------------------------------------

        public async Task<StepOutcome> Advance(JsonObject pending, JsonObject receipts, string sid, object rpc, Chain chain,
                                               string wallet, IReadOnlyDictionary<string, Item> items, long now)
        {
            var found = pending.Get(sid);
            var output = new StepOutcome(sid, "missing", required: FinalConfirmations);
            if (found == null) return output;
            Py.Get(found, "token"); // a record that is not a dict is a TypeError here, as in Python
            var rec = (JsonObject)found;
            var mint = Watcher.TokenOf(chain, rec).Address;
            var client = Client(rpc);
            try
            {
                if (Py.Get(rec, "state") is string state && state == "awaiting")
                {
                    output.Status = "waiting";
                    if (Sessions.IsPastHorizon(rec, now))
                    {
                        Sessions.RemoveSession(pending, sid);
                        output.Status = "closed";
                        return output;
                    }
                    string? outcome = null;
                    if (Py.Truthy(Py.Get(rec, "hint_tx")))
                        outcome = await VerifyHint(rec, client, chain, mint, wallet, items, receipts, now, output);
                    if (outcome == null)
                    {
                        // The reference also runs while a hint is pending: the hinted transaction may have been dropped
                        // and the player paid again.
                        outcome = await ByReference(rec, client, chain, mint, wallet, items, receipts);
                        if (outcome != null)
                        {
                            Sessions.ClearHint(rec);
                            output.Error = null;
                            output.HintTx = null;
                        }
                        else if (!Py.Truthy(Py.Get(rec, "hint_tx")) && await Scan(rec, client, chain, mint, wallet, receipts))
                        {
                            outcome = "matched";
                        }
                    }
                    if (outcome == "underpaid")
                    {
                        Sessions.RemoveSession(pending, sid);
                        output.Status = "underpaid";
                        output.Result = Watcher.ResultFromMatch(rec, "underpaid", chain.Key, rec["match"]);
                        return output;
                    }
                    if (outcome != "matched") return output;
                    rec["state"] = "confirming";
                }

                output.Status = "confirming";
                var m = Py.Get(rec, "match");
                var status = First(await client.GetSignatureStatuses(new List<object?> { Py.Get(m, "tx_hash") }));
                if (status == null)
                {
                    // Unknown to this endpoint: a dropped fork or a lagging endpoint. Keep the signature as a hint so it
                    // is verified again rather than lost.
                    rec["state"] = "awaiting";
                    rec["match"] = null;
                    rec["hint_tx"] = Py.Get(m, "tx_hash");
                    rec["hint_at"] = now;
                    output.Status = "reorged";
                    return output;
                }
                if (!(DictGet(status, "confirmationStatus") is string cs && cs == "finalized"))
                {
                    output.Confirmations = Scfg.Core.Rpc.ToLong(Ints.IntFrom(Or(DictGet(status, "confirmations"), 0L)), "confirmations");
                    return output;
                }
                output.Confirmations = FinalConfirmations;
                var transfer = new Transfer(
                    mint, Watcher.StrOrNone(Py.Get(m, "payer"), "payer"), Watcher.Str(Py.Get(m, "recipient"), "recipient"),
                    Watcher.Units(Py.Get(m, "value"), "value"), Watcher.Str(Py.Get(m, "tx_hash"), "tx_hash"),
                    Watcher.Int(Py.Get(m, "log_index"), "log_index"), Watcher.Int(Py.Get(m, "block"), "block"));
                Receipts.AddReceipt(receipts, Receipts.MakeReceipt(
                    chain.Key, transfer, Watcher.Str(Py.Get(rec, "kind"), "kind"), Watcher.StrOrNone(Py.Get(rec, "item"), "item"),
                    Watcher.Str(Py.Get(rec, "token"), "token"), "payment"));
                Sessions.RemoveSession(pending, sid);
                output.Status = "paid";
                output.Result = Watcher.ResultFromMatch(rec, "paid", chain.Key, m);
                return output;
            }
            catch (Exception e) when (e is RpcUnavailable || e is RpcError)
            {
                // A verdict on the hint from earlier in this step wins: the hint is already cleared, and later steps are
                // plain "waiting", so it would never be reported.
                if (output.Error == null) output.Error = "network_unavailable";
                return output;
            }
        }

        private static bool Qualifies(Transfer? t, JsonObject rec, Chain chain, JsonObject receipts) =>
            t != null
            && t.BlockNumber >= Sessions.NumField(rec, "start_block")
            && !receipts.ContainsKey(Receipts.ReceiptKey(chain.Key, t.TxHash, t.LogIndex));

        private static string Verdict(Transfer t, JsonObject rec, IReadOnlyDictionary<string, Item> items)
        {
            rec["match"] = MatchDict(t);
            return Amounts.UnitsToDecimal(t.Value) < Watcher.MinAmount(rec, items) ? "underpaid" : "matched";
        }

        /// <summary>A transaction's signature as Transfer.tx_hash holds it: a str (a stored hint, a listed signature).</summary>
        private static string Sig(object? v) => Watcher.Str(v, "signature");

        private static async Task<string?> VerifyHint(JsonObject rec, SolanaRpc rpc, Chain chain, string mint, string wallet,
                                                      IReadOnlyDictionary<string, Item> items, JsonObject receipts, long now,
                                                      StepOutcome output)
        {
            bool expired = now - Sessions.NumField(rec, "hint_at") >= SolanaHintTimeoutS;
            object? tx;
            try
            {
                tx = await rpc.GetTransaction(Py.Get(rec, "hint_tx"));
            }
            catch (RpcError) when (expired) // an endpoint that keeps refusing must not keep a hint forever
            {
                tx = null;
            }
            if (tx == null) // not visible yet, or dropped: the player may pay again after a while
            {
                if (expired) DropHint(rec, output, "tx_not_landed");
                return null;
            }
            if (DictGet(Or(DictGet(tx, "meta"), new JsonObject()), "err") != null)
            {
                DropHint(rec, output, "tx_reverted");
                return null;
            }
            var t = TransferTo(tx, Sig(Py.Get(rec, "hint_tx")), mint, wallet);
            if (!Qualifies(t, rec, chain, receipts))
            {
                DropHint(rec, output, "tx_not_a_payment");
                return null;
            }
            return Verdict(t!, rec, items);
        }

        private static void DropHint(JsonObject rec, StepOutcome output, string error)
        {
            output.Error = error;
            output.HintTx = Watcher.StrOrNone(Py.Get(rec, "hint_tx"), "hint_tx");
            Sessions.ClearHint(rec);
        }

        /// <summary>The reference ties a transaction to this session, so any amount matches.</summary>
        private static async Task<string?> ByReference(JsonObject rec, SolanaRpc rpc, Chain chain, string mint, string wallet,
                                                       IReadOnlyDictionary<string, Item> items, JsonObject receipts)
        {
            var infos = Rows(await rpc.GetSignaturesForAddress(Py.Get(rec, "reference"), limit: SigPage));
            for (int i = infos.Count - 1; i >= 0; i--)
            {
                var info = infos[i];
                if (DictGet(info, "err") != null) continue;
                var sig = Py.Get(info, "signature");
                var tx = await rpc.GetTransaction(sig);
                var t = Py.Truthy(tx) ? TransferTo(tx, Sig(sig), mint, wallet) : null;
                if (Qualifies(t, rec, chain, receipts)) return Verdict(t!, rec, items);
            }
            return null;
        }

        /// <summary>
        /// Pages of signatures on <paramref name="address"/> between <paramref name="until"/> (exclusive, older bound)
        /// and <paramref name="before"/> (exclusive, newer bound), newest first. Also says whether the walk reached
        /// <paramref name="until"/>; if not, the pages cover only the newest MaxSigPages * SigPage of them.
        ///
        /// A signature from before <paramref name="minSlot"/> also ends the walk (and is dropped): with no cursor, or one
        /// this endpoint doesn't know, the walk would otherwise go through the account's whole history.
        /// </summary>
        private static async Task<(List<object?> Found, bool Reached)> Walk(SolanaRpc rpc, object? address, object? until,
                                                                             object? before, object? minSlot)
        {
            var found = new List<object?>();
            for (int p = 0; p < MaxSigPages; p++)
            {
                var page = Rows(await rpc.GetSignaturesForAddress(address, until, before, SigPage));
                for (int n = 0; n < page.Count; n++)
                {
                    var info = page[n];
                    if (DictGet(info, "slot") != null && Below(Ints.IntFrom(Py.Get(info, "slot")), minSlot))
                    {
                        found.AddRange(page.GetRange(0, n));
                        return (found, true);
                    }
                }
                found.AddRange(page);
                if (page.Count < SigPage) return (found, true);
                before = Py.Get(page[page.Count - 1], "signature");
            }
            // The last page was full too: reached `until` only if nothing older is left.
            var more = await rpc.GetSignaturesForAddress(address, until, before, 1);
            return (found, !Py.Truthy(more));
        }

        /// <summary>
        /// Exchange and manual transfers carry no reference: match the exact amount.
        ///
        /// Signatures are only listed newest first, so a backlog larger than one walk is crossed in steps: each walk
        /// that cannot reach <c>scan_cursor</c> moves <c>scan_before</c> back and examines nothing; once a walk reaches
        /// the cursor its window is processed oldest first, then <c>scan_before</c> is cleared to look at the newer
        /// signatures again. A busy token account only delays detection, it never hides a payment. Signatures from
        /// before the session's start slot are never examined.
        /// </summary>
        private static async Task<bool> Scan(JsonObject rec, SolanaRpc rpc, Chain chain, string mint, string wallet, JsonObject receipts)
        {
            var (found, reached) = await Walk(rpc, Py.Get(rec, "dest"), Py.Get(rec, "scan_cursor"), rec.Get("scan_before"),
                                              Py.Get(rec, "start_block"));
            if (!reached)
            {
                rec["scan_before"] = Py.Get(found[found.Count - 1], "signature");
                return false;
            }
            int budget = ScanTxBudget;
            for (int i = found.Count - 1; i >= 0; i--)
            {
                var info = found[i];
                if (DictGet(info, "err") != null)
                {
                    rec["scan_cursor"] = Py.Get(info, "signature");
                    continue;
                }
                if (budget == 0) return false;
                budget -= 1;
                var sig = Py.Get(info, "signature");
                var tx = await rpc.GetTransaction(sig);
                if (tx == null) return false; // listed but not served yet: retry from here next step
                rec["scan_cursor"] = sig;
                var t = TransferTo(tx, Sig(sig), mint, wallet);
                // Python reads rec["units"] only for a transfer that qualifies
                if (Qualifies(t, rec, chain, receipts) && Watcher.IntEquals(t!.Value, Py.Get(rec, "units")))
                {
                    rec["match"] = MatchDict(t);
                    return true;
                }
            }
            rec["scan_before"] = null; // window done: look at the newer signatures again
            return false;
        }

        // ---- launch re-check, restore, readiness -------------------------------------------------------------------

        public async Task<string> Reverify(JsonObject receipt, object rpc, Chain chain, IReadOnlyList<string> wallets)
        {
            var client = Client(rpc);
            var slot = Ints.IntFrom(Py.Get(receipt, "block"));
            var owner = receipt.Get("recipient");
            if (!(owner is string ownerKey && wallets.Contains(ownerKey))) return "drop";
            object? tx;
            bool authoritative;
            try
            {
                long current = await client.GetSlot("finalized");
                if (slot > current) return slot > current + FutureToleranceBlocks ? "drop" : "keep";
                (tx, authoritative) = await client.GetTransactionAny(Py.Get(receipt, "tx_hash"), minSlot: (long)slot);
            }
            catch (Exception e) when (e is RpcUnavailable || e is RpcError)
            {
                return "keep";
            }
            if (tx == null) return authoritative ? "drop" : "keep";
            Transfer? t;
            try
            {
                var sig = Py.Get(receipt, "tx_hash");
                t = TransferTo(tx, Sig(sig), Watcher.TokenOf(chain, receipt).Address, ownerKey);
            }
            catch (Exception e) when (e is KeyError || e is TypeError || e is ValueError)
            {
                return "keep"; // a payload this build can't read (AttributeError in Python too): can't judge the receipt
            }
            bool genuine = t != null && t.BlockNumber == slot
                           && t.LogIndex == Ints.IntFrom(Py.Get(receipt, "log_index"))
                           && t.Value == Amounts.DecimalToUnits(Amounts.StoredAmount(Py.Get(receipt, "amount")));
            return genuine ? "keep" : "drop";
        }

        public Task<List<RestoreCandidate>?> Lookup(string tx, ConfigData config, Chain chain, object rpc,
                                                    JsonObject receipts, Action<Exception>? onError) =>
            throw new NotImplementedException("restore: task 12"); // needs restore.py's classify_or_skip

        /// <summary>
        /// Wallets refuse to pay a wallet with no token account for the mint. A getAccountInfo result without
        /// <c>value</c> is a KeyError, which the Engine reports (the route stays ready).
        /// </summary>
        public async Task<Dictionary<string, bool>> CheckReady(Func<object> rpc, Chain chain, string? wallet)
        {
            var ready = new Dictionary<string, bool>();
            foreach (var kv in chain.Tokens)
            {
                var client = Client(rpc()); // Python builds the lazy RPC on the attribute lookup, before the argument
                ready[kv.Key] = await client.AccountExists(Pda.AssociatedTokenAddress(Key(wallet), kv.Value.Address));
            }
            return ready;
        }
    }
}
