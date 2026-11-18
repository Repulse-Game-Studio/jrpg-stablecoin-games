// Twin of scfg_core/solana/rpc.py: Solana JSON-RPC on top of the shared failover client (Scfg.Core.RpcClient).
// Modeled on rpgmaker/src/core/solana/rpc.ts.
//
// History-sensitive lookups (restore, launch re-check) ask endpoints one by one, because public endpoints keep
// very different amounts of history: publicnode about two days, api.mainnet-beta everything. A missing
// transaction only counts as "doesn't exist" when an endpoint whose history reaches back to the slot in
// question says so.
#nullable enable
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Threading.Tasks;

namespace Scfg.Core.Solana
{
    public static class SolanaRpcErrors
    {
        /// <summary>The highest transaction version this build can read (only meta is used).</summary>
        public const long TxVersion = 1;

        internal const long UnknownHistory = 1L << 62;
        internal const long UnsupportedVersion = -32015;

        /// <summary>
        /// Python <c>SolanaRpc._named_version</c> for a client whose tx_version is <paramref name="txVersion"/>:
        /// the newer version a -32015 "Transaction version (N) is not supported" names, else null. Only meta, slot
        /// and accountKeys are read, so any version can be read. The match is Python's
        /// <c>re.search(r"version \((\d+)\)", message, re.IGNORECASE)</c> (<see cref="FindNamedVersion"/>); a
        /// number of more than 18 significant digits is no match (a request cannot carry it as an int).
        /// </summary>
        public static long? NamedVersion(RpcError error, long txVersion)
        {
            if (error.Code != UnsupportedVersion) return null;
            var run = FindNamedVersion(error.Message);
            long? version = run == null ? null : Ints.DigitRunValue(run);
            if (version == null || version <= txVersion) return null;
            return version;
        }

        // Python's IGNORECASE: each letter of "version" matches these code points and no others (checked over every
        // code point with CPython 3.9): s also matches U+017F (long s), i also U+0130 and U+0131.
        private static readonly string[] VersionLetters = { "Vv", "Ee", "Rr", "Ss\u017f", "Ii\u0130\u0131", "Oo", "Nn" }; // static-ok: private letter table, only read

        /// <summary>
        /// The digits group of the leftmost <c>version \((\d+)\)</c> match, IGNORECASE, or null. <c>\d+</c> is a run
        /// of Python 3.9 decimal digits; backtracking into it cannot help (a shorter run is followed by a digit,
        /// not ")"), so each start is tried once and the scan stays linear.
        /// </summary>
        internal static string? FindNamedVersion(string m)
        {
            for (int i = 0; i + 9 <= m.Length; i++)
            {
                int k = 0;
                while (k < VersionLetters.Length && VersionLetters[k].IndexOf(m[i + k], StringComparison.Ordinal) >= 0) k++;
                if (k < VersionLetters.Length || m[i + 7] != ' ' || m[i + 8] != '(') continue;
                int start = i + 9, j = start;
                while (j < m.Length)
                {
                    int cp = Py.CodePointAt(m, j, out int w);
                    if (!Py.IsDecimalDigit(cp)) break;
                    j += w;
                }
                if (j > start && j < m.Length && m[j] == ')') return m.Substring(start, j - start);
            }
            return null;
        }
    }

    /// <summary>
    /// Python <c>solana.rpc.SolanaRpc</c>. Results stay JSON values; numbers read with <c>int_from</c> are a
    /// BigInteger (a balance, a fee) or a long (a slot).
    /// </summary>
    public sealed class SolanaRpc
    {
        public RpcClient Client { get; set; }

        /// <summary>The transaction version asked for; lowered once per -32602 from a node that does not know it.</summary>
        public long TxVersion { get; set; }

        private readonly Dictionary<string, BigInteger> _firstSlot = new Dictionary<string, BigInteger>(StringComparer.Ordinal);

        public SolanaRpc(RpcClient client)
        {
            Client = client;
            TxVersion = SolanaRpcErrors.TxVersion;
        }

        public Task<object?> Call(string method, List<object?> prms) => Client.Call(method, prms);

        public async Task<long> GetSlot(string commitment = "confirmed") =>
            Ints.ToLong(Ints.IntFrom(await Call("getSlot", new List<object?> { new JsonObject { { "commitment", commitment } } })), "slot");

        /// <summary>
        /// Python <c>get_signatures_for_address</c>: the result, or an empty list for a falsy one (<c>... or []</c>).
        /// The address and the bounds go out as given (a stored record's fields are sent unchecked, as in Python); a
        /// falsy bound is left out.
        /// </summary>
        public async Task<object?> GetSignaturesForAddress(object? address, object? until = null, object? before = null,
            long limit = 100, string commitment = "confirmed")
        {
            var opts = new JsonObject { { "limit", limit }, { "commitment", commitment } };
            if (Py.Truthy(until)) opts["until"] = until;
            if (Py.Truthy(before)) opts["before"] = before;
            var res = await Call("getSignaturesForAddress", new List<object?> { address, opts });
            return Py.Truthy(res) ? res : new List<object?>();
        }

        private List<object?> TxParams(object? sig, string commitment, long? version = null) =>
            new List<object?>
            {
                sig,
                new JsonObject
                {
                    { "encoding", "json" }, { "commitment", commitment }, { "maxSupportedTransactionVersion", version ?? TxVersion },
                },
            };

        private long? NamedVersion(RpcError error) => SolanaRpcErrors.NamedVersion(error, TxVersion);

        /// <summary>Older nodes reject a maxSupportedTransactionVersion they don't know (-32602).</summary>
        private bool LowerVersion(RpcError error)
        {
            if (TxVersion > 0 && error.Code == -32602 && Scfg.Core.Rpc.PyLowerForAscii(error.Message).IndexOf("version", StringComparison.Ordinal) >= 0)
            {
                TxVersion -= 1;
                return true;
            }
            return false;
        }

        /// <summary>Python <c>get_transaction</c>: the signature goes out as given (a stored hint is sent unchecked).</summary>
        public async Task<object?> GetTransaction(object? sig, string commitment = "confirmed")
        {
            long? version;
            try
            {
                return await Call("getTransaction", TxParams(sig, commitment));
            }
            catch (RpcError e)
            {
                if (LowerVersion(e)) return await GetTransaction(sig, commitment);
                version = NamedVersion(e);
                if (version == null) throw;
            }
            return await Call("getTransaction", TxParams(sig, commitment, version));
        }

        private async Task<BigInteger> FirstAvailable(string url)
        {
            if (!_firstSlot.TryGetValue(url, out var first))
            {
                try
                {
                    first = Ints.IntFrom(await Client.CallAt(url, "getFirstAvailableBlock", new List<object?>()));
                }
                catch (Exception e) when (e is RpcUnavailable || e is RpcError || e is TypeError || e is ValueError)
                {
                    return SolanaRpcErrors.UnknownHistory; // unknown: treat as pruned, ask again next time
                }
                _firstSlot[url] = first;
            }
            return first;
        }

        /// <summary>
        /// Python <c>get_transaction_any</c>: the transaction from the first live endpoint that has it, and whether
        /// a null answer is authoritative (an endpoint whose history reaches <paramref name="minSlot"/> said so).
        /// RpcUnavailable when no endpoint answered.
        /// </summary>
        public async Task<(object? Tx, bool Authoritative)> GetTransactionAny(object? sig, long? minSlot = null, string commitment = "finalized")
        {
            bool answered = false, authoritative = false;
            foreach (var url in Client.LiveUrls())
            {
                var attempt = await Client.TryAt(url, "getTransaction", TxParams(sig, commitment));
                if (attempt == null) continue;
                object? res = attempt.ResultValue;
                if (attempt.IsError)
                {
                    var error = attempt.Error!;
                    if (LowerVersion(error)) return await GetTransactionAny(sig, minSlot, commitment);
                    var version = NamedVersion(error);
                    if (version == null) continue;
                    try
                    {
                        res = await Client.CallAt(url, "getTransaction", TxParams(sig, commitment, version));
                    }
                    catch (Exception e) when (e is RpcUnavailable || e is RpcError)
                    {
                        continue;
                    }
                }
                answered = true;
                if (res != null) return (res, false);
                if (minSlot != null && await FirstAvailable(url) <= minSlot.Value) authoritative = true;
            }
            if (!answered) throw new RpcUnavailable("no endpoint answered getTransaction for " + Coio.Str(sig));
            return (null, authoritative);
        }

        public async Task<object?> GetSignatureStatuses(IEnumerable<object?> sigs)
        {
            var list = new List<object?>();
            foreach (var s in sigs) list.Add(s);
            var res = await Call("getSignatureStatuses", new List<object?> { list, new JsonObject { { "searchTransactionHistory", true } } });
            return Py.Get(res, "value");
        }

        public async Task<bool> AccountExists(string address)
        {
            var res = await Call("getAccountInfo", new List<object?>
            {
                address, new JsonObject { { "encoding", "base64" }, { "commitment", "confirmed" } },
            });
            return Py.Get(res, "value") != null;
        }

        /// <summary>Python <c>get_latest_blockhash</c>: <c>res["value"]["blockhash"]</c>, as the node sent it.</summary>
        public async Task<object?> GetLatestBlockhash()
        {
            var res = await Call("getLatestBlockhash", new List<object?> { new JsonObject { { "commitment", "confirmed" } } });
            return Py.Get(Py.Get(res, "value"), "blockhash");
        }

        public async Task<BigInteger> GetTokenBalance(string tokenAccount)
        {
            var res = await Call("getAccountInfo", new List<object?>
            {
                tokenAccount, new JsonObject { { "encoding", "jsonParsed" }, { "commitment", "confirmed" } },
            });
            var value = Py.Get(res, "value");
            if (value == null) return BigInteger.Zero;
            object? amount = value;
            foreach (var key in new[] { "data", "parsed", "info", "tokenAmount", "amount" }) amount = Py.Get(amount, key);
            return Ints.IntFrom(amount);
        }

        public async Task<BigInteger> GetBalance(string address)
        {
            var res = await Call("getBalance", new List<object?> { address, new JsonObject { { "commitment", "confirmed" } } });
            return Ints.IntFrom(Py.Get(res, "value"));
        }

        /// <summary>Python <c>get_priority_fee</c>: the median (upper middle) recent prioritization fee, 0 for none.</summary>
        public async Task<BigInteger> GetPriorityFee(IEnumerable<object?> accounts)
        {
            var list = new List<object?>();
            foreach (var a in accounts) list.Add(a);
            var rows = await Call("getRecentPrioritizationFees", new List<object?> { list });
            var fees = new List<BigInteger>();
            if (Py.Truthy(rows))
                foreach (var f in Coio.Iterate(rows)) fees.Add(Ints.IntFrom(Py.Get(f, "prioritizationFee")));
            fees.Sort();
            return fees.Count > 0 ? fees[fees.Count / 2] : BigInteger.Zero;
        }
    }
}
