// Twin of scfg_core/engine.py (and of rpgmaker/src/core/engine.ts).
// The Engine composes config, sessions, watcher, receipts and restore.
//
// It owns all plugin state (deep copies of what the game persists) and is NOT re-entrant: one caller drives it, one
// command at a time, on one thread. Readers get Snapshots, which are detached copies. Methods that need the network
// are async and only await the injected transport (Python's cooperative *_co jobs); the others are plain.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>Python's frozen <c>Snapshot</c>: what readers see, detached from the Engine's state.</summary>
    public sealed class Snapshot
    {
        public JsonObject Pending { get; }
        public JsonObject Receipts { get; }
        /// <summary>Owned item ids, sorted by code point (Python's frozenset).</summary>
        public IReadOnlyList<string> Owned { get; }
        public Dec TipTotal { get; }
        /// <summary>The (chain, token) routes the developer cannot receive on, sorted (Python's frozenset of tuples).</summary>
        public IReadOnlyList<(string Chain, string Token)> Unready { get; }

        public Snapshot(JsonObject pending, JsonObject receipts, IReadOnlyList<string> owned, Dec tipTotal,
                        IReadOnlyList<(string Chain, string Token)> unready)
        {
            Pending = pending;
            Receipts = receipts;
            Owned = owned;
            TipTotal = tipTotal;
            Unready = unready;
        }
    }

    public sealed class Engine
    {
        private static readonly string[] SessionKeys = { "item", "amount", "token", "title" }; // static-ok: private keyword table, only read

        public ConfigData Config { get; }
        /// <summary>Where a contained crash goes (one session's step, one stored receipt, one restore candidate).</summary>
        public Action<Exception>? OnError { get; set; }
        public JsonObject Pending { get; }
        public JsonObject Receipts { get; }
        public Func<long> Clock { get; }
        public ITransport Transport { get; }
        public IEntropy Entropy { get; }

        /// <summary>
        /// While it returns true, the RPC requests the Engine's own RPC clients make are refused with
        /// <see cref="Abandoned"/>: the worker points it at the job it runs (worker.py's per-job transport).
        /// </summary>
        public Func<bool>? RefuseRequests { get; set; }

        private readonly Func<Chain, object> _rpcFactory;
        private readonly Dictionary<string, object> _rpcs = new Dictionary<string, object>(StringComparer.Ordinal);
        private HashSet<(string Chain, string Token)> _unready = new HashSet<(string, string)>();

        /// <param name="clock">Whole seconds; the session timestamps and windows run on it.</param>
        /// <param name="rpcFactory">The RPC object of a chain; default: its backend's MakeRpc over <paramref name="transport"/>.</param>
        /// <param name="onError">Default: <see cref="Errors.PrintError"/>.</param>
        /// <param name="entropy">Python's <c>secrets</c>; default: <see cref="SystemEntropy"/>.</param>
        public Engine(ConfigData config, ITransport transport, JsonObject? pending, JsonObject? receipts, Func<long> clock,
                      Func<Chain, object>? rpcFactory = null, Action<Exception>? onError = null, IEntropy? entropy = null)
        {
            Config = config ?? throw new TypeError("an Engine needs a config");
            Transport = transport ?? throw new TypeError("an Engine needs a transport");
            Clock = clock ?? throw new TypeError("an Engine needs a clock");
            OnError = onError ?? Errors.PrintError;
            Pending = (JsonObject)DeepCopy.Of(pending ?? new JsonObject())!;
            Receipts = (JsonObject)DeepCopy.Of(receipts ?? new JsonObject())!;
            Entropy = entropy ?? SystemEntropy.Instance;
            _rpcFactory = rpcFactory ?? (chain => Backends.BackendFor(chain).MakeRpc(chain, new RefusingTransport(this)));
        }

        // --- chains & RPC ---------------------------------------------------------

        /// <summary>The Engine's transport, refusing requests while <see cref="RefuseRequests"/> says so.</summary>
        private sealed class RefusingTransport : ITransport
        {
            private readonly Engine _engine;

            public RefusingTransport(Engine engine) { _engine = engine; }

            public Task<object?> PostJson(string url, JsonObject payload, double timeout)
            {
                if (_engine.RefuseRequests?.Invoke() == true)
                    return Task.FromException<object?>(new Abandoned("abandoned before its request to " + url));
                return _engine.Transport.PostJson(url, payload, timeout);
            }
        }

        /// <summary>Full registry chain (all tokens, so old receipts stay verifiable) with overrides. KeyError if unknown.</summary>
        public Chain Chain(string key)
        {
            if (!Config.Registry.TryGetValue(key, out var b)) throw new KeyError(key);
            var rpc = new List<string>();
            if (Config.RpcUrls.TryGetValue(key, out var extra)) rpc.AddRange(extra);
            rpc.AddRange(b.Rpc);
            return b.With(rpc, Config.Confirmations.TryGetValue(key, out var n) ? n : b.Confirmations);
        }

        /// <summary>The chain's RPC object, built on first use and kept.</summary>
        public object Rpc(string key)
        {
            if (!_rpcs.TryGetValue(key, out var rpc))
            {
                rpc = _rpcFactory(Chain(key));
                _rpcs[key] = rpc;
            }
            return rpc;
        }

        /// <summary><c>config.registry[rec["chain"]]</c> as Python indexes it: a KeyError, or a TypeError for an unhashable key.</summary>
        private string ChainKeyOf(object? rec)
        {
            var key = Py.Get(rec, "chain");
            if (key is JsonObject || key is List<object?>) throw new TypeError("unhashable type: '" + Py.TypeName(key) + "'");
            return key as string ?? throw new KeyError(Py.Repr(key));
        }

        // --- payments ---------------------------------------------------------------

        /// <summary>
        /// start_session(kind, chain, *, item=None, amount=None, token=None, title=None): <paramref name="opts"/> holds
        /// the keyword arguments; an unknown one is a TypeError. Returns a copy of the new session record.
        /// </summary>
        public async Task<JsonObject> StartSession(string kind, string chain, JsonObject opts)
        {
            foreach (var k in opts.Keys)
                if (!SessionKeys.Contains(k))
                    throw new TypeError("start_session() got an unexpected keyword argument " + Py.Repr(k));
            var spec = Payments.PreparePayment(Config, kind, opts);
            if (!spec.Chains.Contains(chain))
                throw new ConfigError(spec.Token + " is not available on chain " + Py.Repr(chain), "chain.token_unavailable");
            var chainObj = Chain(chain);
            var fields = await Backends.BackendFor(chainObj).Start(Rpc(chain), chainObj, Config.WalletFor(chainObj), spec.Token, Entropy);
            var start = Watcher.Int(Py.Get(fields, "start_block"), "start_block"); // fields.pop("start_block")
            fields.Remove("start_block");
            var rec = Sessions.CreateSession(Pending, spec.Kind, spec.Item, spec.Title, spec.Token, chain, spec.Price, spec.Code,
                                             start, Clock(), null, fields, Entropy);
            return (JsonObject)DeepCopy.Of(rec)!;
        }

        /// <summary>
        /// The unsigned transaction a browser wallet signs for a Solana session (<paramref name="account"/> pays):
        /// <c>transaction</c> (base64), <c>token_balance</c> and <c>sol_balance</c> (decimal strings of base units).
        /// </summary>
        public async Task<JsonObject> BuildSolanaPayment(string sid, string account)
        {
            var rec = Pending[sid];
            var chain = Chain(ChainKeyOf(rec));
            if (chain.Family != "solana") throw new ValueError("not a Solana session");
            return await Backends.BackendFor(chain).BuildPayment((JsonObject)rec!, Rpc(chain.Key), chain, account);
        }

        public void ReportHint(string sid, object? txHash)
        {
            var rec = Pending[sid];
            var backend = Backends.BackendFor(Chain(ChainKeyOf(rec)));
            Sessions.SetHint(Pending, sid, txHash, Clock(), backend.NormalizeTx);
        }

        public void SetUiStatus(string sid, object? status)
        {
            if (Pending.ContainsKey(sid)) Sessions.SetUiStatus(Pending, sid, status);
        }

        /// <summary>Drop a session the player abandoned before paying (e.g. switched network).</summary>
        public void DiscardSession(string sid)
        {
            var rec = Pending.Get(sid);
            if (rec != null && Py.Get(rec, "state") is string state && state == "awaiting" && !Py.Truthy(Py.Get(rec, "hint_tx")))
                Pending.Remove(sid);
        }

        /// <summary>Every session, one after another in pending order (never concurrently), then the expiries.</summary>
        public async Task<List<StepOutcome>> Tick()
        {
            long now = Clock();
            var outcomes = new List<StepOutcome>();
            foreach (var kv in Pending.ToList()) // list(self.pending.items())
            {
                var outcome = await AdvanceOne(kv.Key, kv.Value, now);
                if (outcome != null) outcomes.Add(outcome);
            }
            outcomes.AddRange(Expire(now));
            return outcomes;
        }

        /// <summary>
        /// One session's step. A session this build can no longer watch (its chain left the registry, or the game
        /// stopped accepting that family) is kept until its scan horizon, then closed. A crash in one session never
        /// stops the others: it goes to <see cref="OnError"/> and the session reports internal_error.
        /// </summary>
        private async Task<StepOutcome?> AdvanceOne(string sid, object? rec, long now)
        {
            try
            {
                var key = Py.Get(rec, "chain");
                if (key is JsonObject || key is List<object?>) throw new TypeError("unhashable type: '" + Py.TypeName(key) + "'");
                if (!(key is string k) || !Config.Registry.ContainsKey(k) || Config.WalletFor(Chain(k)) == null)
                {
                    if (!Sessions.IsPastHorizon((JsonObject)rec!, now)) return null;
                    Sessions.RemoveSession(Pending, sid);
                    return new StepOutcome(sid, "closed");
                }
                var chain = Chain(k);
                return await Backends.BackendFor(chain).Advance(Pending, Receipts, sid, Rpc(k), chain, Config.WalletFor(chain)!,
                                                                Config.Items, now);
            }
            catch (Exception e)
            {
                Report(e);
                return new StepOutcome(sid, "waiting", error: "internal_error");
            }
        }

        /// <summary>Report the 30-minute payment window closing (scanning continues up to 24 h).</summary>
        private List<StepOutcome> Expire(long now)
        {
            var output = new List<StepOutcome>();
            foreach (var kv in Pending)
            {
                var rec = kv.Value;
                if (Py.Get(rec, "ui_status") is string ui && (ui == "open" || ui == "pending")
                    && Sessions.IsPastWindow((JsonObject)rec!, now))
                {
                    var r = (JsonObject)rec!;
                    r["ui_status"] = "expired";
                    output.Add(new StepOutcome(kv.Key, "expired", result: new PaymentResult("expired", Watcher.Str(Py.Get(r, "kind"), "kind"))
                    {
                        Item = Watcher.StrOrNone(Py.Get(r, "item"), "item"),
                        Token = Watcher.StrOrNone(Py.Get(r, "token"), "token"),
                        Chain = Watcher.StrOrNone(Py.Get(r, "chain"), "chain"),
                        SessionId = kv.Key,
                    }));
                }
            }
            return output;
        }

        // --- receipts ---------------------------------------------------------------

        /// <summary>
        /// Launch checks: re-verify stored receipts against the full registry (so old receipts stay verifiable), then
        /// check that the developer can receive each enabled token (Solana needs a token account). Returns the removed
        /// receipt keys, in store order. A receipt whose check crashed is kept and reported to <see cref="OnError"/>.
        /// </summary>
        public async Task<List<string>> Reverify()
        {
            var chains = new Dictionary<string, Chain>(StringComparer.Ordinal);
            foreach (var k in Config.Registry.Keys) chains[k] = Chain(k);
            var wallets = new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
            foreach (var f in new[] { "evm", "solana" }) wallets[f] = Config.AcceptedWalletsFor(f);
            var removed = await Scfg.Core.Reverify.ReverifyReceipts(Receipts, Rpc, chains, wallets, Report);
            await CheckReady();
            return removed;
        }

        /// <summary>
        /// Python <c>_check_ready</c>: the (chain, token) routes of the enabled chains the developer cannot receive. A
        /// network failure leaves a route ready (never hide a payment option on doubt); any other failure is reported.
        /// </summary>
        private async Task CheckReady()
        {
            var unready = new HashSet<(string, string)>();
            foreach (var kv in Scfg.Core.Config.EnabledChains(Config))
            {
                var key = kv.Key;
                var chain = kv.Value;
                Dictionary<string, bool> ready;
                try
                {
                    ready = await Backends.BackendFor(chain).CheckReady(() => Rpc(key), chain, Config.WalletFor(chain));
                }
                catch (Exception e) when (e is RpcUnavailable || e is RpcError)
                {
                    continue;
                }
                catch (Exception e)
                {
                    Report(e);
                    continue;
                }
                foreach (var r in ready)
                    if (!r.Value) unready.Add((key, r.Key));
            }
            _unready = unready;
        }

        /// <summary>
        /// Restore candidates for a pasted transaction id, from the first chain that knows it: the game's chains in
        /// its order, then the rest of the registry. Throws RestoreError (see <see cref="Restore.Lookup"/>).
        /// </summary>
        public Task<List<RestoreCandidate>> RestoreLookup(string tx)
        {
            var registry = Config.Registry;
            var ordered = Config.Chains.Concat(registry.Keys.Where(k => !Config.Chains.Contains(k)));
            var chains = ordered.Select(k => new KeyValuePair<string, Chain>(k, Chain(k))).ToList();
            return Restore.Lookup(tx, Config, chains, Rpc, Receipts, Report);
        }

        /// <summary>
        /// Store the candidates as receipts; returns the receipts added. <paramref name="choices"/> maps a candidate's
        /// index to an item id or "tip" for the candidates the player must choose for (see <see cref="Restore.Apply"/>).
        /// </summary>
        public List<JsonObject> RestoreApply(IReadOnlyList<RestoreCandidate> candidates, IReadOnlyDictionary<int, object?>? choices = null) =>
            Restore.Apply(Receipts, candidates, choices);

        /// <summary>
        /// Where a contained crash goes: <see cref="OnError"/> as it is when the error is reported (a host may replace
        /// its handler with a pass in flight; reverify and restore_lookup report as they go). An unset handler prints
        /// the error, as Python's <c>on_error or print_error</c> does.
        /// </summary>
        private void Report(Exception e) => (OnError ?? Errors.PrintError)(e);

        public Snapshot Snapshot()
        {
            var ownership = Scfg.Core.Receipts.ComputeOwnership(Receipts, Config.Items);
            var unready = _unready.ToList();
            unready.Sort((x, y) =>
            {
                int c = Py.CompareCodePoints(x.Chain, y.Chain);
                return c != 0 ? c : Py.CompareCodePoints(x.Token, y.Token);
            });
            return new Snapshot((JsonObject)DeepCopy.Of(Pending)!, (JsonObject)DeepCopy.Of(Receipts)!, ownership.Owned,
                                ownership.TipTotal, unready);
        }
    }
}
