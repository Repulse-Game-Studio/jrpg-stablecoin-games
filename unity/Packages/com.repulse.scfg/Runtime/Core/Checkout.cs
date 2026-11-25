// Twin of scfg_core/checkout.py (CheckoutController and its tables), ported member for member from
// rpgmaker/src/core/checkout.ts (and godot/addons/scfg/core/checkout.gd): the state behind the payment screen,
// independent of any engine.
//
// The screen only displays this object's fields and calls its methods from buttons, on the game's thread. All Engine
// work goes through the SubmitFn (the Worker), and results come back through Poll() (jobs) and OnOutcome() (watcher
// outcomes routed by session id).
//
// States: choose -> starting -> waiting -> confirming -> paid
//                                       \-> underpaid | expired
//
// The sheet shows one step at a time (Step):
// amount (tips only) -> network -> method -> browser | qr | manual -> waiting -> done,
// or, through a network's "Pay with card" button: network -> card -> waiting -> done.
// Steps with a single option are skipped both ways. Back() works until a transaction is seen; a seen payment moves the
// sheet to waiting and a final state to done.
#nullable enable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Scfg.Core.Solana;

namespace Scfg.Core
{
    /// <summary>
    /// How the network step lays out the networks the player can pick (Python's checkout.network_view): the tiles always
    /// shown, those behind "More networks", whether that is open, and the networks with a "Pay with card" button.
    /// </summary>
    public readonly struct NetworkView
    {
        public IReadOnlyList<string> Main { get; }
        public IReadOnlyList<string> More { get; }
        public bool Expanded { get; }
        public IReadOnlyList<string> Card { get; }

        public NetworkView(IReadOnlyList<string> main, IReadOnlyList<string> more, bool expanded, IReadOnlyList<string> card)
        {
            Main = main;
            More = more;
            Expanded = expanded;
            Card = card;
        }
    }

    /// <summary>checkout.py's module tables, read-only (Unity's domain reload off keeps statics between plays).</summary>
    public static class Checkout
    {
        /// <summary>
        /// Whether the network gets a "Pay with card" button: configure's onramp is on and lists it (by default the
        /// built-in EVM chains of a mainnet game; never Solana). (Python's checkout.card_offered.)
        /// </summary>
        public static bool CardOffered(ConfigData config, string? chainKey)
        {
            var opts = config.Onramp;
            return opts.Enabled && !string.IsNullOrEmpty(chainKey) && opts.Chains.Contains(chainKey!)
                   && Scfg.Core.Config.EnabledChains(config).ContainsKey(chainKey!);
        }

        /// <summary>
        /// The card checkout page for a running session (Python's checkout.card_url, pinned by the tier1 vectors):
        /// configure's onramp template filled in for the network, the developer's wallet there and the session's exact
        /// amount (<paramref name="units"/>, all 6 decimals). Null without a session or where the network has no card
        /// button.
        /// </summary>
        public static string? CardUrl(ConfigData config, string token, string? chainKey, BigInteger? units)
        {
            if (units == null || !CardOffered(config, chainKey)) return null;
            var chain = Scfg.Core.Config.EnabledChains(config)[chainKey!];
            return Paylinks.OnrampUrl(config.Onramp.Url, chain.Key, chain.ChainId, token, chain.Tokens[token].Address,
                                      Amounts.FormatUnits(units.Value), config.WalletFor(chain)!);
        }

        /// <summary>
        /// How the network step lays out <paramref name="optionKeys"/>, the networks the player can pick in display order
        /// (Python's checkout.network_view, pinned by the tier1 vectors). Main: the options among
        /// <see cref="Chains.MainChains"/> and, when there are fewer than <see cref="Chains.MainTiles"/> of those, the
        /// next options in order until there are that many (an EVM-only game: Ethereum and Arc), in display order;
        /// More: the rest, behind "More networks" (empty: no "More networks"). Expanded: "More networks" is open, as the player left it
        /// (<paramref name="expanded"/>) or because the chosen network is behind it. Card: the options with a "Pay with
        /// card" button (<see cref="CardOffered"/>), in display order.
        /// </summary>
        public static NetworkView NetworkView(ConfigData config, IReadOnlyList<string> optionKeys, string? chainKey,
                                              bool expanded)
        {
            var others = optionKeys.Where(k => !Chains.MainChains.Contains(k)).ToList();
            var fill = others.Take(Math.Max(0, Chains.MainTiles - (optionKeys.Count - others.Count))).ToList();
            var main = optionKeys.Where(k => Chains.MainChains.Contains(k) || fill.Contains(k)).ToList();
            var more = optionKeys.Where(k => !main.Contains(k)).ToList();
            bool open = more.Count > 0 && (expanded || (chainKey != null && more.Contains(chainKey)));
            return new NetworkView(main.AsReadOnly(), more.AsReadOnly(), open,
                                   optionKeys.Where(k => CardOffered(config, k)).ToList().AsReadOnly());
        }

        public static readonly IReadOnlyList<string> FinalStates = Array.AsReadOnly(new[] { "paid", "underpaid", "expired" });

        /// <summary>Errors that judge the reported transaction (the hint) and drop it.</summary>
        public static readonly IReadOnlyList<string> HintVerdicts =
            Array.AsReadOnly(new[] { "tx_reverted", "tx_not_a_payment", "tx_not_landed" });

        /// <summary>Order of the steps; the three method pages share a place.</summary>
        public static readonly IReadOnlyDictionary<string, int> StepOrder = new ReadOnlyDictionary<string, int>(
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                { "amount", 0 }, { "network", 1 }, { "method", 2 }, { "browser", 3 }, { "qr", 3 }, { "manual", 3 },
                { "card", 3 }, { "waiting", 4 }, { "done", 5 },
            });

        /// <summary>The English text of each checkout message id (conformance/vectors/messages.json).</summary>
        public static readonly IReadOnlyDictionary<string, string> Messages = new ReadOnlyDictionary<string, string>(
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                { "network_unavailable", "Can't reach the network. Retrying..." },
                { "tx_reverted", "That transaction failed on-chain; no funds were sent." },
                { "tx_not_a_payment", "That transaction isn't a payment for this purchase." },
                { "tx_not_landed", "That transaction didn't reach the network. You can pay again." },
                { "invalid_hash", "That doesn't look like a transaction hash or signature." },
                { "invalid_amount", "Enter an amount of at least " + Payments.MinTip + " with up to 2 decimals." },
                { "browser_unavailable", "Couldn't start the browser checkout. Use another method." },
                { "start_failed", "Couldn't start the payment. Check your connection and try again." },
                { "start_unreachable", "Can't reach the network. Pick a network to try again." },
                { "solana_unavailable", "Payments on Solana aren't available right now." },
                { "internal_error", "Something went wrong while checking the payment. Retrying..." },
                { "paste_blocked", "Your browser blocked pasting. Type it instead: the game also finds the payment without it." },
            });

        /// <summary><c>STEP_ORDER[step]</c>: a KeyError for a step that is not one.</summary>
        internal static int Order(string step) =>
            step == null ? throw KeyError.OfRepr(Py.Repr((object?)null))
            : StepOrder.TryGetValue(step, out var n) ? n : throw new KeyError(step);
    }

    public sealed class CheckoutController
    {
        private static readonly IReadOnlyCollection<(string Chain, string Token)> NothingBlocked =
            Array.AsReadOnly(Array.Empty<(string, string)>());

        public ConfigData Config { get; }
        public PaymentSpec Spec { get; }
        public SubmitFn Submit { get; }
        /// <summary>A phone: no computer browser to open the page in (unless <see cref="BrowserWallet"/> decides).</summary>
        public bool Mobile { get; set; }
        /// <summary>Builds the browser checkout (the localhost page or the web overlay); null: no browser method.</summary>
        public Func<ServerArgs, ICheckoutServer>? ServerFactory { get; set; }
        /// <summary><c>worker.Snapshot.Unready</c>: the (chain, token) routes that cannot take a payment.</summary>
        public Func<IReadOnlyCollection<(string Chain, string Token)>> Unready { get; }
        /// <summary>Web build: whether the page has a wallet that can pay on a chain; null on desktop.</summary>
        public Func<Chain, bool>? BrowserWallet { get; set; }
        public string GameName { get; }

        public Dec Amount { get; private set; }
        /// <summary>What the player typed on the amount step (tips).</summary>
        public string CustomAmount { get; set; } = "";
        /// <summary>What the player typed or pasted as a transaction id.</summary>
        public string TxInput { get; set; } = "";
        public string? ChainKey { get; private set; }
        /// <summary>A copy of the session record the Engine started (null until Poll() picks it up).</summary>
        public JsonObject? Session { get; private set; }
        public string? Method { get; private set; }
        public string State { get; private set; } = "choose";
        /// <summary>
        /// A message id of <see cref="Checkout.Messages"/>, or an error id the screen has no text for. The glue writes it
        /// too (as Ren'Py's and Godot's do): a paste sets it back to null, a paste the browser refused to "paste_blocked".
        /// </summary>
        public string? Error { get; set; }
        public long Confirmations { get; private set; }
        public long Required { get; private set; }
        public string? HintTx { get; private set; }
        /// <summary>The hint that never landed (tx_not_landed).</summary>
        public string? DroppedTx { get; private set; }
        public PaymentResult? Result { get; private set; }
        public bool BrowserFailed { get; private set; }
        public string Step { get; private set; }
        /// <summary>1: the step came in going forward, -1: going back.</summary>
        public int NavDir { get; private set; } = 1;

        private Job? _future;
        private ICheckoutServer? _server;
        private bool _pickedNetwork; // move on from the network step once the session starts

        /// <summary>The player opened "More networks" on the network step.</summary>
        public bool MoreNetworks { get; private set; }

        public CheckoutController(ConfigData config, PaymentSpec spec, SubmitFn submit, bool mobile = false,
                                  Func<ServerArgs, ICheckoutServer>? serverFactory = null,
                                  Func<IReadOnlyCollection<(string Chain, string Token)>>? unready = null,
                                  string? gameName = null, Func<Chain, bool>? browserWallet = null)
        {
            Config = config ?? throw new TypeError("a CheckoutController needs a config");
            Spec = spec ?? throw new TypeError("a CheckoutController needs a payment spec");
            Submit = submit ?? throw new TypeError("a CheckoutController needs a submit function");
            Mobile = mobile;
            ServerFactory = serverFactory;
            Unready = unready ?? (() => NothingBlocked);
            BrowserWallet = browserWallet;
            GameName = string.IsNullOrEmpty(gameName) ? "Game" : gameName!;
            Amount = spec.Price;
            Step = spec.Kind == "tip" ? "amount" : "network";
            if (Step == "network") EnterNetwork();
        }

        // --- read-only views ---------------------------------------------------------

        private bool HasSession => Py.Truthy(Session);

        public string? SessionId => HasSession ? Py.Get(Session, "id") as string : null;

        public Chain? Chain =>
            ChainKey != null && Scfg.Core.Config.EnabledChains(Config).TryGetValue(ChainKey, out var chain) ? chain : null;

        public string? Family => Chain?.Family;

        /// <summary>(key, name, fee hint) of every network this payment can use now.</summary>
        public List<(string Key, string Name, string FeeHint)> ChainOptions()
        {
            var chains = Scfg.Core.Config.EnabledChains(Config);
            var blocked = Unready();
            var output = new List<(string Key, string Name, string FeeHint)>();
            foreach (var k in Spec.Chains)
            {
                if (chains.TryGetValue(k, out var chain) && !blocked.Contains((k, Spec.Token)))
                    output.Add((k, chain.Name, chain.FeeHint));
            }
            return output;
        }

        public IReadOnlyList<Dec> TipPresets() => Payments.TipPresets;

        public List<string> Methods()
        {
            var m = Config.Methods;
            var output = new List<string>();
            if (Py.Truthy(Py.Get(Py.Get(m, "browser"), "enabled")) && !BrowserFailed && BrowserPossible()) output.Add("browser");
            var qrChains = Py.Get(Py.Get(m, "qr"), "chains");
            if (Py.Truthy(Py.Get(Py.Get(m, "qr"), "enabled")) && (qrChains == null || In(ChainKey, qrChains))) output.Add("qr");
            if (Py.Truthy(Py.Get(Py.Get(m, "manual"), "enabled"))) output.Add("manual");
            return output;
        }

        /// <summary>
        /// Python's <c>key in chains</c> for the configured QR chain list. A string is a substring test, and a null key
        /// in a string is false here where Python raises TypeError; neither is reachable, as build_config refuses a
        /// <c>methods.qr.chains</c> that is not a list of enabled chain keys.
        /// </summary>
        private static bool In(string? key, object chains)
        {
            if (chains is string s) return key != null && s.Contains(key);
            if (!(chains is IEnumerable seq)) throw new TypeError("argument of type '" + Py.TypeName(chains) + "' is not iterable");
            foreach (var c in seq)
                if (Equals(c, key)) return true;
            return false;
        }

        /// <summary>
        /// Desktop: a computer's browser can always open the page. Web build: only if a wallet that can pay on the
        /// chosen network is in the page (phones included: a wallet app's own browser injects one).
        /// </summary>
        private bool BrowserPossible()
        {
            if (BrowserWallet != null)
            {
                var chain = Chain;
                return chain != null && BrowserWallet(chain);
            }
            return !Mobile;
        }

        /// <summary>The exact amount to send, with the item code and nonce: "4.993742".</summary>
        public string PayAmountText => HasSession ? Amounts.FormatUnits(Ints.IntFrom(Py.Get(Session, "units"))) : Amount.ToString(); // culture-ok: Dec.ToString is invariant

        /// <summary>The price as the player thinks of it: "4.99".</summary>
        public string PriceText => Amount.ToString(); // culture-ok: Dec.ToString is invariant

        public string? TokenAddress
        {
            get
            {
                var chain = Chain;
                if (chain == null) return null;
                return chain.Tokens.TryGetValue(Spec.Token, out var token) ? token.Address : throw new KeyError(Spec.Token);
            }
        }

        public string? Recipient
        {
            get
            {
                var chain = Chain;
                if (chain != null) return Config.WalletFor(chain);
                return Py.Truthy(Config.Wallet) ? Config.Wallet : Config.SolanaWallet;
            }
        }

        public string? PaymentUri()
        {
            if (!HasSession) return null;
            var units = Ints.IntFrom(Py.Get(Session, "units"));
            if (Family == "solana")
            {
                return SolanaPaylinks.SolanaPayUri(Recipient!, units, TokenAddress!, (string)Py.Get(Session, "reference")!,
                                                   GameName, Spec.Title);
            }
            return Paylinks.Eip681TransferUri(TokenAddress!, Chain!.ChainId, Recipient!, units);
        }

        /// <summary>The network step's layout: main tiles, "More networks", card buttons (Checkout.NetworkView).</summary>
        public NetworkView NetworkLayout() =>
            Checkout.NetworkView(Config, ChainOptions().Select(o => o.Key).ToList(), ChainKey, MoreNetworks);

        /// <summary>Whether the network <paramref name="key"/> has a "Pay with card" button.</summary>
        public bool CardOffered(string? key) => Checkout.CardOffered(Config, key);

        /// <summary>
        /// The card checkout page for this session: thirdweb's by default, paying the exact amount to the developer's
        /// wallet on the chosen network. Null before the session starts or where the network has no card button.
        /// </summary>
        public string? CardUrl() =>
            Checkout.CardUrl(Config, Spec.Token, ChainKey, HasSession ? Ints.IntFrom(Py.Get(Session, "units")) : (BigInteger?)null);

        public string? ExplorerUrl()
        {
            var tx = Result != null && Py.Truthy(Result.TxHash) ? Result.TxHash : HintTx;
            var chain = Chain;
            return Py.Truthy(tx) && chain != null ? chain.ExplorerUrl(tx!) : null;
        }

        public string? Message()
        {
            if (Py.Truthy(Error)) return Checkout.Messages.TryGetValue(Error!, out var text) ? text : null;
            if (State == "choose" && Spec.Chains.Count > 0 && ChainOptions().Count == 0) return Checkout.Messages["solana_unavailable"];
            return null;
        }

        /// <summary>Shown in DEV MODE only: setup problems the developer must fix.</summary>
        public List<string> DevWarnings()
        {
            var registry = Config.Registry;
            var routes = Unready().ToList();
            routes.Sort(Py.CompareRoutes);
            var output = new List<string>();
            foreach (var (key, token) in routes)
            {
                if (registry.TryGetValue(key, out var chain) && chain.Family == "solana")
                    output.Add("Your Solana wallet can't receive " + token + " yet: receive any amount once.");
            }
            return output;
        }

        /// <summary>
        /// Status for the localhost page. <c>dropped_tx</c> names the transaction a <c>tx_not_landed</c> is about, so
        /// the page offers Pay again only if it is the one the page sent.
        /// </summary>
        public JsonObject StatusInfo() => new JsonObject
        {
            { "state", State }, { "confirmations", Confirmations }, { "required", Required }, { "tx_hash", HintTx },
            { "error", Error }, { "dropped_tx", DroppedTx },
        };

        /// <summary>A transaction for this payment exists, so going back could start a second one.</summary>
        public bool TxSeen => Py.Truthy(HintTx) || State == "confirming";

        /// <summary>The steps this payment walks through, without the single-option ones.</summary>
        public List<string> StepPath()
        {
            var path = Spec.Kind == "tip" ? new List<string> { "amount" } : new List<string>();
            if (NetworkHasChoice()) path.Add("network");
            if (Method == "card")
            {
                path.Add("card");
                path.Add("waiting");
                return path;
            }
            if (Methods().Count != 1) path.Add("method");
            path.Add(Py.Truthy(Method) ? Method! : "browser");
            path.Add("waiting");
            return path;
        }

        /// <summary>(index of the current step, number of steps) for the progress dots.</summary>
        public (int Index, int Count) StepDots()
        {
            var path = StepPath();
            int here = Checkout.Order(Step);
            return (path.Count(s => Checkout.Order(s) < here), path.Count);
        }

        public bool CanGoBack
        {
            get
            {
                if (Step == "done" || TxSeen) return false;
                return PreviousStep() != null;
            }
        }

        // --- actions -----------------------------------------------------------------

        public void SelectChain(string key)
        {
            if (key == null || !Spec.Chains.Contains(key))
                throw new ValueError("chain " + Py.Repr((object?)key) + " not available for this payment");
            DiscardSession();
            ChainKey = key;
            State = "starting";
            Error = null;
            var spec = Spec;
            var amount = Amount;
            _future = Submit(Commands.Of(e => e.StartSession(spec.Kind, key, new JsonObject
            {
                { "item", spec.Item }, { "amount", amount }, { "token", spec.Token }, { "title", spec.Title },
            })));
            var available = Methods();
            if ((Method == null || !available.Contains(Method)) && !(Method == "card" && CardOffered(key)))
                Method = available.Count > 0 ? available[0] : null;
        }

        /// <summary>Network step: start a session on <paramref name="key"/>, or keep the one already running.</summary>
        public void ChooseNetwork(string key)
        {
            if (Method == "card") Method = null; // the network's own button: pick a method as usual
            PickNetwork(key);
        }

        /// <summary>
        /// Network step, a network's "Pay with card" button: start a session on <paramref name="key"/> (or keep the one
        /// running), then go to the card page, skipping the method step.
        /// </summary>
        public void PayWithCard(string key)
        {
            if (!CardOffered(key)) return;
            Method = "card";
            PickNetwork(key);
        }

        /// <summary>Open or close "More networks" on the network step.</summary>
        public void ToggleMoreNetworks() => MoreNetworks = !NetworkLayout().Expanded;

        private void PickNetwork(string key)
        {
            Step = "network";
            NavDir = 1;
            if (key == ChainKey && HasSession && State == "waiting")
            {
                AfterNetwork();
                return;
            }
            _pickedNetwork = true; // Poll() moves on once the session has started
            if (key != ChainKey || State != "starting") SelectChain(key);
        }

        public void ChooseMethod(string name)
        {
            if (Methods().Contains(name))
            {
                Method = name;
                Error = null;
                Go(name, 1);
            }
        }

        /// <summary>Leave the amount step (tips), applying a typed amount first.</summary>
        public bool NextStep()
        {
            if (Step != "amount") return false;
            if (Py.Strip(CustomAmount ?? "").Length != 0 && !ApplyCustomAmount()) return false;
            EnterNetwork();
            return true;
        }

        public void Back()
        {
            if (CanGoBack)
            {
                Error = null;
                Go(PreviousStep()!, -1);
            }
        }

        /// <summary>The player says they sent the payment from the QR or manual page.</summary>
        public void MarkSent()
        {
            if (Step == "qr" || Step == "manual")
            {
                Error = null;
                Go("waiting", 1);
            }
        }

        /// <summary>
        /// Card page: returns the card checkout's URL for the sheet to open, and moves to waiting; null (staying put)
        /// before the session has started.
        /// </summary>
        public string? OpenCard()
        {
            var url = Step == "card" ? CardUrl() : null;
            if (url != null) Go("waiting", 1);
            return url;
        }

        /// <summary>Browser page: start the browser checkout and move to waiting; returns its URL.</summary>
        public string? OpenBrowser()
        {
            var url = StartBrowser();
            if (Py.Truthy(url))
            {
                Go("waiting", 1);
            }
            else if (BrowserFailed && Step == "browser")
            {
                var methods = Methods();
                Go(methods.Count != 1 ? "method" : methods[0], -1);
            }
            return url;
        }

        /// <summary>A tip's amount (str, int or Dec, as parse_amount takes); false and invalid_amount if refused.</summary>
        public bool SetTipAmount(object? value)
        {
            if (Spec.Kind != "tip") return false;
            Dec amount;
            try
            {
                amount = Amounts.ParseAmount(value);
            }
            catch (Exception e) when (e is TypeError || e is ValueError)
            {
                Error = "invalid_amount";
                return false;
            }
            if (amount < Payments.MinTip)
            {
                Error = "invalid_amount";
                return false;
            }
            Error = null;
            if (amount == Amount && Py.Truthy(ChainKey)) return true; // same amount: keep the session already started
            Amount = amount;
            if (Py.Truthy(ChainKey)) SelectChain(ChainKey!);
            return true;
        }

        public bool ApplyCustomAmount() => SetTipAmount(Py.Strip(CustomAmount ?? ""));

        /// <summary>Report a pasted transaction id (<see cref="TxInput"/> when <paramref name="text"/> is null).</summary>
        public bool SubmitHash(string? text = null)
        {
            var chain = Chain;
            if (!HasSession || chain == null) return false;
            string tx;
            try
            {
                tx = Backends.BackendFor(chain).NormalizeTx(text ?? TxInput);
            }
            catch (ValueError)
            {
                Error = "invalid_hash";
                return false;
            }
            ReportHint(tx);
            TxInput = "";
            return true;
        }

        /// <summary>checkout.py's <c>_report_hint</c>, also the page's <see cref="ServerArgs.OnTx"/>.</summary>
        internal void ReportHint(string txHash, string? sender = null)
        {
            var sid = SessionId;
            HintTx = txHash;
            Error = null;
            DroppedTx = null;
            Submit(Commands.Sync(e => e.ReportHint(sid!, txHash)));
            if (Step != "done") Go("waiting", 1);
        }

        /// <summary>Start the browser checkout for this session; returns its URL or null.</summary>
        public string? StartBrowser()
        {
            if (!HasSession || ServerFactory == null) return null;
            if (_server == null)
            {
                var chain = Chain!;
                var info = new JsonObject
                {
                    { "title", Spec.Title }, { "kind", Spec.Kind }, { "token", Spec.Token },
                    { "family", chain.Family }, { "decimals", 6L },
                    { "amount_units", Ints.IntFrom(Py.Get(Session, "units")).ToString(CultureInfo.InvariantCulture) },
                    { "amount_text", PayAmountText },
                    { "price_text", PriceText },
                    { "recipient", Recipient }, { "chain_name", chain.Name },
                    { "native_symbol", chain.NativeSymbol },
                    { "explorer_tx", chain.ExplorerTx }, { "testnet", Config.Testnet },
                    { "logo", Config.Logo != null }, { "accent", Config.Accent },
                    { "on_accent", Scfg.Core.Config.TextOn(Config.Accent) },
                };
                if (chain.Family == "solana")
                {
                    info["wallet_chain"] = chain.WalletChain;
                    info["mint"] = TokenAddress;
                    info["reference"] = Py.Get(Session, "reference");
                }
                else
                {
                    info["chain_id"] = chain.ChainId;
                    info["chain_id_hex"] = Rpc.Hex(chain.ChainId);
                    info["rpc_url"] = chain.Rpc[chain.Rpc.Count - 1];
                    info["token_address"] = TokenAddress;
                    // Arc: the token is also the gas coin, with a fee floor.
                    info["gas_is_token"] = chain.NativeToken == Spec.Token;
                    info["min_fee_wei"] = chain.MinFeeWei.ToString(CultureInfo.InvariantCulture);
                }
                Func<string, Job>? buildTx = null;
                if (chain.Family == "solana") buildTx = SubmitBuild;
                var args = new ServerArgs(info, (tx, sender) => ReportHint(tx, sender), StatusInfo, buildTx);
                try
                {
                    _server = ServerFactory(args);
                    _server.Start();
                }
                catch (Exception)
                {
                    _server = null;
                    BrowserFailed = true;
                    Error = "browser_unavailable";
                    if (Method == "browser")
                    {
                        var methods = Methods();
                        Method = methods.Count > 0 ? methods[0] : null;
                    }
                    return null;
                }
            }
            else
            {
                _server.Reopen();
            }
            return _server.Url;
        }

        /// <summary>
        /// Ask the Worker for the unsigned Solana transfer; returns its Job. The desktop page's server waits on it, the
        /// web overlay polls it.
        /// </summary>
        private Job SubmitBuild(string account)
        {
            var sid = SessionId;
            return Submit(Commands.Of(e => e.BuildSolanaPayment(sid!, account)));
        }

        public PaymentResult Cancel()
        {
            var sid = SessionId;
            if (Py.Truthy(sid) && !Checkout.FinalStates.Contains(State)) Submit(Commands.Sync(e => e.SetUiStatus(sid!, "cancelled")));
            Close();
            Result ??= MakeResult("cancelled");
            return Result;
        }

        public PaymentResult KeepPlaying()
        {
            var sid = SessionId;
            if (Py.Truthy(sid)) Submit(Commands.Sync(e => e.SetUiStatus(sid!, "pending")));
            return MakeResult("pending");
        }

        /// <summary>Close after a final state (paid/underpaid/expired).</summary>
        public PaymentResult Finish()
        {
            Close();
            return Result ?? MakeResult("cancelled");
        }

        public void Close()
        {
            AbandonStart();
            if (_server != null)
            {
                _server.Stop();
                _server = null;
            }
        }

        // --- updates -------------------------------------------------------------

        /// <summary>Pump the page and pick up the result of StartSession (call every frame from the UI).</summary>
        public void Poll()
        {
            _server?.Pump();
            var fut = _future;
            if (fut == null || !fut.Done()) return;
            _future = null;
            var exc = fut.Exception();
            var started = exc == null ? fut.Result() as JsonObject : null;
            // A success that is not a session (Python would raise on it later) is a failed start too, as in checkout.gd.
            if (exc != null || started == null)
            {
                State = "choose";
                ChainKey = null;
                Error = exc is RpcUnavailable ? "start_unreachable" : "start_failed";
                if (Step != "amount" && Step != "network") Go("network", -1);
                return;
            }
            Session = started;
            State = "waiting";
            var chain = Chain;
            Required = chain != null ? chain.Confirmations : 0;
            if (Step == "network" && _pickedNetwork) AfterNetwork();
        }

        public void OnOutcome(StepOutcome outcome)
        {
            if (outcome == null) throw new TypeError("an outcome is required");
            if (!HasSession || outcome.SessionId != SessionId) return;
            if (outcome.Error != null && Checkout.HintVerdicts.Contains(outcome.Error))
                HintVerdict(outcome);
            else if (Py.Truthy(outcome.Error))
                Error = outcome.Error;
            var status = outcome.Status;
            if (status == "confirming")
            {
                State = "confirming";
                Confirmations = outcome.Confirmations;
                Required = outcome.Required;
                if (outcome.Error == null) Error = null;
            }
            else if (status == "reorged")
            {
                State = "waiting";
                Confirmations = 0;
            }
            else if (status == "paid" || status == "underpaid")
            {
                State = status;
                Result = outcome.Result;
                Error = null;
            }
            else if (status == "expired" && State != "paid" && State != "underpaid")
            {
                State = "expired";
                Result = outcome.Result;
            }
            if (Checkout.FinalStates.Contains(State))
            {
                if (Step != "done") Go("done", 1);
            }
            else if (TxSeen && Step != "waiting")
            {
                Go("waiting", 1);
            }
        }

        // --- internals -----------------------------------------------------------

        private void Go(string step, int direction)
        {
            Step = step;
            NavDir = direction;
            _pickedNetwork = false;
        }

        private string? PreviousStep()
        {
            int here = Checkout.Order(Step);
            return StepPath().LastOrDefault(s => Checkout.Order(s) < here);
        }

        /// <summary>Go forward to the network step, or past it when there is only one network.</summary>
        private void EnterNetwork()
        {
            var options = ChainOptions();
            if (!NetworkHasChoice()) ChooseNetwork(options[0].Key);
            else Go("network", 1);
        }

        /// <summary>The network step is shown unless there is a single network without a card button.</summary>
        private bool NetworkHasChoice()
        {
            var options = ChainOptions();
            return options.Count != 1 || CardOffered(options[0].Key);
        }

        /// <summary>
        /// The session is running: go to the method step, or straight to the only method; after "Pay with card", to the
        /// card page.
        /// </summary>
        private void AfterNetwork()
        {
            if (Method == "card")
            {
                Go("card", 1);
                return;
            }
            var methods = Methods();
            if (Method == null || !methods.Contains(Method)) Method = methods.Count > 0 ? methods[0] : null;
            Go(methods.Count == 1 ? Method! : "method", 1);
        }

        private void HintVerdict(StepOutcome outcome)
        {
            var about = outcome.HintTx;
            if (Py.Truthy(about) && Py.Truthy(HintTx) && about != HintTx) return; // judged an older hint; the newer one is still pending
            Error = outcome.Error;
            DroppedTx = outcome.Error == "tx_not_landed" ? (Py.Truthy(about) ? about : HintTx) : null;
            HintTx = null;
        }

        /// <summary>
        /// Stop waiting for a session start the player moved away from, and discard that session once it exists: it
        /// would be watched for 24 h and reported expired. The done callback runs at once when the start is done.
        /// </summary>
        private void AbandonStart()
        {
            var fut = _future;
            _future = null;
            if (fut == null) return;
            fut.Abandon(); // a queued start never runs; a running one ends at once, its request in flight included (worker.py's abandon)
            var submit = Submit;
            fut.AddDoneCallback(f =>
            {
                if (f.Cancelled() || f.Exception() != null) return;
                // a start that gave no session (no string id) has nothing to drop, as in checkout.gd
                if (f.Result() is JsonObject started && started.Get("id") is string sid)
                    submit(Commands.Sync(e => e.DiscardSession(sid)));
            });
        }

        private void DiscardSession()
        {
            if (HasSession && (State == "waiting" || State == "starting"))
            {
                var sid = SessionId;
                Submit(Commands.Sync(e => e.DiscardSession(sid!)));
            }
            Close();
            Session = null;
            HintTx = null;
            DroppedTx = null;
            Confirmations = 0;
        }

        private PaymentResult MakeResult(string status) => new PaymentResult(status, Spec.Kind)
        {
            Item = Spec.Item, Token = Spec.Token, Chain = ChainKey, SessionId = SessionId,
        };
    }
}
