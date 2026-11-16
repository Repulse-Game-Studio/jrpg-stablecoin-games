// Twin of scfg_core/config.py (and of rpgmaker/src/core/config.ts).
// Developer configuration: validation of configure()/add_item() arguments. Every mistake raises ConfigError at
// startup, so it surfaces while the developer is testing instead of when a player tries to pay.
//
// Python's keyword arguments arrive as a JsonObject: an unknown key is a TypeError, as Python's unexpected keyword
// argument is, and so is a missing required one. Values stay loosely typed (JSON values; a Chain object stands for
// a custom chain), and every isinstance check of config.py is mapped onto the C# type of the value: Python's bool is
// an int but a C# bool is not an integral type, and a float is a double. The checks run in config.py's order,
// because the first failing check decides the error code.
//
// config.py has no case folding beyond ASCII (its patterns are case-sensitive, its colour digits and logo
// extension are lowercased), so the folding here is ASCII-only and culture-free by construction.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Scfg.Core.Solana;

namespace Scfg.Core
{
    /// <summary>Python's <c>Config</c> dataclass (named apart from the static <see cref="Config"/> that builds it).</summary>
    public sealed class ConfigData
    {
        /// <summary>EVM payout address; null for a Solana-only game.</summary>
        public string? Wallet { get; }
        public bool Testnet { get; }
        public IReadOnlyList<string> Chains { get; }
        public IReadOnlyList<string> Tokens { get; }
        /// <summary>method name to its options (a JsonObject of JsonObjects).</summary>
        public JsonObject Methods { get; }
        public Dictionary<string, IReadOnlyList<string>> RpcUrls { get; }
        public Dictionary<string, long> Confirmations { get; }
        public IReadOnlyList<string> PreviousWallets { get; }
        public string? SolanaWallet { get; }
        public IReadOnlyList<string> PreviousSolanaWallets { get; }
        /// <summary>The image path, shown on the screens and the checkout page.</summary>
        public string? Logo { get; }
        /// <summary>#rrggbb used for selected options.</summary>
        public string Accent { get; }
        /// <summary>add_chain(), this network, in the order added.</summary>
        public Dictionary<string, Chain> CustomChains { get; }
        /// <summary>add_item(), in the order added.</summary>
        public Dictionary<string, Item> Items { get; }

        public ConfigData(string? wallet, bool testnet, IReadOnlyList<string> chains, IReadOnlyList<string> tokens,
                          JsonObject methods, Dictionary<string, IReadOnlyList<string>> rpcUrls,
                          Dictionary<string, long> confirmations, IReadOnlyList<string> previousWallets,
                          string? solanaWallet, IReadOnlyList<string> previousSolanaWallets, string? logo,
                          string accent, Dictionary<string, Chain> customChains)
        {
            Wallet = wallet;
            Testnet = testnet;
            Chains = chains;
            Tokens = tokens;
            Methods = methods;
            RpcUrls = rpcUrls;
            Confirmations = confirmations;
            PreviousWallets = previousWallets;
            SolanaWallet = solanaWallet;
            PreviousSolanaWallets = previousSolanaWallets;
            Logo = logo;
            Accent = accent;
            CustomChains = customChains;
            Items = new Dictionary<string, Item>();
        }

        /// <summary>Every chain this game knows on its network: built-in, then the developer's own.</summary>
        public Dictionary<string, Chain> Registry
        {
            get
            {
                var r = new Dictionary<string, Chain>(Scfg.Core.Chains.GetChains(Testnet));
                foreach (var kv in CustomChains) r[kv.Key] = kv.Value;
                return r;
            }
        }

        /// <summary>EVM: current wallet first, then wallets used by earlier versions of the game.</summary>
        public IReadOnlyList<string> AcceptedWallets
        {
            get
            {
                var o = new List<string>();
                if (!string.IsNullOrEmpty(Wallet)) o.Add(Wallet!);
                o.AddRange(PreviousWallets);
                return o;
            }
        }

        public IReadOnlyList<string> AcceptedWalletsFor(string family)
        {
            if (family == "solana")
            {
                var o = new List<string>();
                if (!string.IsNullOrEmpty(SolanaWallet)) o.Add(SolanaWallet!);
                o.AddRange(PreviousSolanaWallets);
                return o;
            }
            return AcceptedWallets;
        }

        public string? WalletFor(Chain chain) => chain.Family == "solana" ? SolanaWallet : Wallet;

        public Item? ItemByCode(int code)
        {
            foreach (var item in Items.Values)
                if (item.Code == code) return item;
            return null;
        }
    }

    public static class Config
    {
        public static readonly string[] Tokens = { "USDC", "USDT" };
        public static readonly string[] FeeHints = { "lowest", "low", "high" };

        public const string DefaultAccent = "#0e7c74";
        public const string Ink = "#17202e";

        // rpc.py's MIN_LOG_RANGE, the smallest max_log_range a custom chain may declare.
        private const long MinLogRange = 10;

        /// <summary>A fresh copy of config.py's DEFAULT_METHODS.</summary>
        public static JsonObject NewDefaultMethods() => new JsonObject
        {
            { "browser", new JsonObject { { "enabled", true }, { "timeout_min", 30L } } },
            { "qr", new JsonObject { { "enabled", true }, { "wallet_button", true }, { "chains", null } } },
            { "manual", new JsonObject { { "enabled", true }, { "allow_tx_paste", true } } },
        };

        // Raster formats only: an SVG served from the checkout page's origin could run script.
        public static readonly IReadOnlyDictionary<string, string> LogoTypes = new Dictionary<string, string>
        {
            { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" },
        };

        // ---- Python-shaped helpers ------------------------------------------------------------------------

        private static bool TryInt(object? v, out BigInteger n)
        {
            switch (v)
            {
                case int i: n = i; return true;
                case long l: n = l; return true;
                case BigInteger b: n = b; return true;
                case short s: n = s; return true;
                case ushort us: n = us; return true;
                case byte by: n = by; return true;
                case sbyte sb: n = sb; return true;
                case uint ui: n = ui; return true;
                case ulong ul: n = ul; return true;
                default: n = BigInteger.Zero; return false;
            }
        }

        private static long ToLong(BigInteger v, string what)
        {
            if (v < long.MinValue || v > long.MaxValue) throw new ValueError(what + " does not fit 64 bits");
            return (long)v;
        }

        /// <summary>Python's iteration of a value, as <c>tuple(x)</c> and <c>for x in</c> see it.</summary>
        private static List<object?> Iterate(object? v)
        {
            switch (v)
            {
                case string s:
                {
                    var o = new List<object?>();
                    for (int i = 0; i < s.Length;)
                    {
                        Py.CodePointAt(s, i, out int w);
                        o.Add(s.Substring(i, w));
                        i += w;
                    }
                    return o;
                }
                case JsonObject d:
                    return d.Keys.Cast<object?>().ToList();
                case IList list:
                {
                    var o = new List<object?>(list.Count);
                    foreach (var x in list) o.Add(x);
                    return o;
                }
                default:
                    throw new TypeError("'" + Py.TypeName(v) + "' object is not iterable");
            }
        }

        /// <summary>Python's TypeError for a list or dict used as a dict key or set member.</summary>
        private static void Hashable(object? v)
        {
            if (v is JsonObject) throw new TypeError("unhashable type: 'dict'");
            if (v is IList) throw new TypeError("unhashable type: 'list'");
        }

        private static string Lower(string ascii)
        {
            // Python's str.lower(); only ASCII letters can reach it (hex digits, a logo extension) and none of the
            // non-ASCII characters that fold to ASCII (K, U+212A) is part of what it is compared with.
            var c = ascii.ToCharArray();
            for (int i = 0; i < c.Length; i++)
                if (c[i] >= 'A' && c[i] <= 'Z') c[i] = (char)(c[i] + 32);
            return new string(c);
        }

        private static void CheckKwargs(JsonObject kw, string fn, string[] allowed, string[] required)
        {
            foreach (var k in kw.Keys)
                if (!allowed.Contains(k))
                    throw new TypeError(fn + "() got an unexpected keyword argument " + Py.Repr(k));
            foreach (var r in required)
                if (!kw.ContainsKey(r))
                    throw new TypeError(fn + "() missing required keyword-only argument: " + Py.Repr(r));
        }

        private static object? Opt(JsonObject kw, string name, object? fallback = null) =>
            kw.TryGetValue(name, out var v) ? v : fallback;

        // Python: re.fullmatch of [a-z0-9_-]{1,max} (ASCII only, no trailing newline).
        private static bool IsId(object? v, int max, bool upper)
        {
            if (!(v is string s) || s.Length < 1 || s.Length > max) return false;
            foreach (char c in s)
            {
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
                          || (upper && c >= 'A' && c <= 'Z');
                if (!ok) return false;
            }
            return true;
        }

        private static bool IsHex(char c) => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');

        // ---- addresses --------------------------------------------------------------------------------------

        private static string Address(object? value, string what)
        {
            try
            {
                return Evm.NormalizeAddress(value);
            }
            catch (ValueError e)
            {
                throw new ConfigError(what + ": " + e.Message, "address.invalid");
            }
        }

        private static string SolanaAddress(object? value, string what)
        {
            string address;
            try
            {
                address = Base58.NormalizePubkey(value);
            }
            catch (ValueError)
            {
                throw new ConfigError(what + ": not a Solana address: " + Py.Repr(value), "solana_address.invalid");
            }
            if (!Ed25519.IsOnCurve(Base58.B58Decode(address)))
                throw new ConfigError(what + ": " + address
                                      + " looks like a token account or program address; use your wallet address",
                                      "solana_address.not_on_curve");
            return address;
        }

        /// <summary>(family, normalized address): 0x addresses are EVM, anything else Solana.</summary>
        private static (string Family, string Address) AnyWallet(object? value, string what)
        {
            if (value is string s && Py.Strip(s).StartsWith("0x", StringComparison.Ordinal))
                return ("evm", Address(Py.Strip(s), what));
            return ("solana", SolanaAddress(value, what));
        }

        // ---- rpc urls and numbers ---------------------------------------------------------------------------

        // Python: re.match(r"^http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/|\Z)", u), with \d the Unicode digits of
        // CPython 3.9 (Py.IsDecimalDigit) and \Z the very end of the string (no trailing newline).
        private static bool IsLocalHttp(string u)
        {
            const string scheme = "http://";
            if (!u.StartsWith(scheme, StringComparison.Ordinal)) return false;
            int p = scheme.Length;
            if (u.Length - p >= 9 && (string.CompareOrdinal(u, p, "127.0.0.1", 0, 9) == 0
                                      || string.CompareOrdinal(u, p, "localhost", 0, 9) == 0))
                p += 9;
            else
                return false;
            if (p < u.Length && u[p] == ':')
            {
                int q = p + 1;
                int digits = 0;
                while (q < u.Length)
                {
                    int cp = Py.CodePointAt(u, q, out int w);
                    if (!Py.IsDecimalDigit(cp)) break;
                    digits++;
                    q += w;
                }
                // More than five digits cannot backtrack into a match: the next character would be a digit.
                if (digits < 1 || digits > 5) return false;
                return q == u.Length || u[q] == '/';
            }
            return p == u.Length || u[p] == '/';
        }

        private static IReadOnlyList<string> RpcUrls(object? value, string what)
        {
            // Python: urls = [value] if isinstance(value, str) else value; it must be a non-empty list or tuple.
            List<object?>? urls = value is string single ? new List<object?> { single }
                : value is IList list ? Iterate(list) : null;
            if (urls == null || urls.Count == 0)
                throw new ConfigError(what + " must be a URL or a non-empty list of URLs", "rpc.bad_list");
            var o = new List<string>(urls.Count);
            foreach (var u in urls)
            {
                if (!(u is string us) || !(us.StartsWith("https://", StringComparison.Ordinal) || IsLocalHttp(us)))
                    throw new ConfigError("rpc url must start with https://: " + Py.Repr(u), "rpc.not_https");
                o.Add(us);
            }
            return o;
        }

        private static BigInteger PositiveInt(object? value, string what, long minimum = 1)
        {
            if (!TryInt(value, out var n) || n < minimum)
                throw new ConfigError(what + " must be an int >= " + minimum.ToString(CultureInfo.InvariantCulture),
                                      "int.below_minimum");
            return n;
        }

        // ---- build_chain ------------------------------------------------------------------------------------

        private static readonly string[] ChainKeys =
            { "key", "name", "chain_id", "rpc", "tokens", "confirmations", "explorer_tx", "native_symbol",
              "fee_hint", "max_log_range", "testnet" };
        private static readonly string[] ChainRequired =
            { "key", "name", "chain_id", "rpc", "tokens", "confirmations", "explorer_tx" };

        // Python: str.count("{}") (non-overlapping).
        private static int CountBraces(string s)
        {
            int n = 0;
            for (int i = s.IndexOf("{}", StringComparison.Ordinal); i >= 0;
                 i = s.IndexOf("{}", i + 2, StringComparison.Ordinal))
                n++;
            return n;
        }

        /// <summary>
        /// A developer-defined EVM chain (scfg.add_chain), from build_chain's keyword arguments (<c>key</c> included).
        /// Validated like the rest of the configuration, so a typo fails at startup rather than when a player pays.
        /// Only EVM chains work: payments are found through ERC-20 Transfer logs over Ethereum JSON-RPC. The tokens
        /// must have 6 decimals, because the item code and nonce live in the last four decimals of the amount.
        /// </summary>
        public static Chain BuildChain(JsonObject kwargs)
        {
            CheckKwargs(kwargs, "build_chain", ChainKeys, ChainRequired);
            var key = kwargs["key"];
            var name = kwargs["name"];
            var chainIdValue = kwargs["chain_id"];
            var rpcValue = kwargs["rpc"];
            var tokens = kwargs["tokens"];
            var confirmationsValue = kwargs["confirmations"];
            var explorerTx = kwargs["explorer_tx"];
            var nativeSymbol = Opt(kwargs, "native_symbol", "ETH");
            var feeHint = Opt(kwargs, "fee_hint", "low");
            var maxLogRangeValue = Opt(kwargs, "max_log_range", 1000L);
            var testnet = Opt(kwargs, "testnet", false);

            if (!IsId(key, 32, false))
                throw new ConfigError("invalid chain key " + Py.Repr(key) + " (use a-z, 0-9, _ and -)", "chain.invalid_key");
            var keyText = (string)key!;
            string what = "chain " + Py.Repr(keyText);
            if (!(name is string nameText) || Py.Strip(nameText).Length == 0 || Py.Len(nameText) > 32)
                throw new ConfigError(what + ": name must be a non-empty string of at most 32 characters", "chain.invalid_name");
            var chainId = PositiveInt(chainIdValue, what + ": chain_id");
            var rpc = RpcUrls(rpcValue, what + ": rpc");
            if (!(tokens is JsonObject tokenMap) || tokenMap.Count == 0)
                throw new ConfigError(what + ": tokens must be a dict like {'USDC': '0x...'}", "chain.tokens_invalid");
            var parsed = new Dictionary<string, Token>();
            foreach (var kv in tokenMap)
            {
                if (!Tokens.Contains(kv.Key))
                    throw new ConfigError(what + ": unknown token " + Py.Repr(kv.Key) + " (supported: USDC, USDT)", "chain.unknown_token");
                parsed[kv.Key] = new Token(Address(kv.Value, what + ": " + kv.Key + " address"));
            }
            var confirmations = PositiveInt(confirmationsValue, what + ": confirmations");
            if (!(explorerTx is string explorer) || !explorer.StartsWith("https://", StringComparison.Ordinal)
                || CountBraces(explorer) != 1)
                throw new ConfigError(what + ": explorer_tx must be an https:// URL with {} where the transaction hash goes, "
                                      + "like 'https://snowtrace.io/tx/{}'", "chain.bad_explorer_tx");
            try
            {
                Py.Format1(explorer, "0x");
            }
            catch (Exception e) when (e is IndexError || e is KeyError || e is ValueError)
            {
                throw new ConfigError(what + ": explorer_tx may contain no braces besides {}", "chain.bad_explorer_tx_braces");
            }
            if (!IsNativeSymbol(nativeSymbol))
                throw new ConfigError(what + ": native_symbol must be letters/digits, like 'AVAX'", "chain.bad_native_symbol");
            if (!(feeHint is string hint) || !FeeHints.Contains(hint))
                throw new ConfigError(what + ": fee_hint must be one of " + string.Join(", ", FeeHints), "chain.bad_fee_hint");
            var maxLogRange = PositiveInt(maxLogRangeValue, what + ": max_log_range", MinLogRange);
            return new Chain(keyText, ToLong(chainId, "chain_id"), Py.Strip(nameText), rpc,
                             ToLong(confirmations, "confirmations"), ToLong(maxLogRange, "max_log_range"), explorer,
                             (string)nativeSymbol!, hint, parsed, Py.Truthy(testnet));
        }

        // Python: re.fullmatch(r"[A-Za-z0-9]{1,11}", s).
        private static bool IsNativeSymbol(object? v)
        {
            if (!(v is string s) || s.Length < 1 || s.Length > 11) return false;
            foreach (char c in s)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            return true;
        }

        // ---- custom chains, logo, accent --------------------------------------------------------------------

        /// <summary>(chains for this network keyed by key, keys only defined for the other network).</summary>
        private static (Dictionary<string, Chain> Mine, HashSet<string> Other) CustomChainsFor(object? custom, bool testnet)
        {
            var builtin = Chains.GetChains(testnet);
            var mine = new Dictionary<string, Chain>();
            var other = new HashSet<string>(StringComparer.Ordinal);
            if (Py.Truthy(custom))
            {
                foreach (var entry in Iterate(custom))
                {
                    if (!(entry is Chain chain))
                        throw new ConfigError("custom chains must be created with scfg.add_chain()", "chain.not_from_add_chain");
                    if (Chains.GetChains(false).ContainsKey(chain.Key) || Chains.GetChains(true).ContainsKey(chain.Key))
                        throw new ConfigError("chain " + Py.Repr(chain.Key)
                                              + " is built in; use rpc_urls/confirmations to change it, or pick another key",
                                              "chain.is_builtin");
                    if (chain.Testnet != testnet)
                    {
                        other.Add(chain.Key);
                        continue;
                    }
                    if (mine.ContainsKey(chain.Key))
                        throw new ConfigError("chain " + Py.Repr(chain.Key) + " added twice", "chain.duplicate");
                    foreach (var c in builtin.Values.Concat(mine.Values).ToList())
                    {
                        if (c.ChainId == chain.ChainId)
                            throw new ConfigError("chain " + Py.Repr(chain.Key) + " has the same chain_id ("
                                                  + chain.ChainId.ToString(CultureInfo.InvariantCulture) + ") as "
                                                  + Py.Repr(c.Key), "chain.duplicate_chain_id");
                    }
                    mine[chain.Key] = chain;
                }
            }
            other.ExceptWith(mine.Keys);
            return (mine, other);
        }

        private static string? LogoArg(object? value)
        {
            if (value == null) return null;
            if (!(value is string s) || s.Length == 0 || s.Contains("\\") || s.Contains(":")
                || s[0] == '/' || s.Split('/').Contains(".."))
                throw new ConfigError("logo must be a path inside the game folder, like 'images/logo.png': " + Py.Repr(value),
                                      "logo.bad_path");
            LogoContentType(s);
            return s;
        }

        public static string LogoContentType(string path)
        {
            int dot = path.LastIndexOf(".", StringComparison.Ordinal);
            string ext = dot >= 0 ? Lower(path.Substring(dot)) : "";
            if (!LogoTypes.TryGetValue(ext, out var type))
                throw new ConfigError("logo must be a .png, .jpg or .webp image: " + Py.Repr(path), "logo.bad_type");
            return type;
        }

        private static string AccentArg(object? value)
        {
            if (value == null) return DefaultAccent;
            // Python: re.fullmatch(r"#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})", value).
            bool ok = value is string t && t.Length > 0 && t[0] == '#'
                      && (t.Length == 4 || t.Length == 7 || t.Length == 9) && t.Skip(1).All(IsHex);
            if (!ok)
                throw new ConfigError("accent must be a color like '#0e7c74': " + Py.Repr(value), "accent.invalid");
            var digits = ((string)value!).Substring(1);
            if (digits.Length == 3) digits = string.Concat(digits.Select(c => new string(c, 2)));
            return "#" + Lower(digits.Substring(0, 6)); // alpha, if any, is dropped
        }

        private static double Channel(string color, int i)
        {
            double c = (HexValue(color[i]) * 16 + HexValue(color[i + 1])) / 255.0;
            return c <= 0.03928 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);
        }

        private static int HexValue(char c) => c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;

        /// <summary>WCAG relative luminance of a #rrggbb color.</summary>
        private static double Luminance(string color) =>
            0.2126 * Channel(color, 1) + 0.7152 * Channel(color, 3) + 0.0722 * Channel(color, 5);

        /// <summary>White or ink, whichever has the higher WCAG contrast on <paramref name="color"/> (#rrggbb).</summary>
        public static string TextOn(string color)
        {
            double lum = Luminance(color);
            double white = 1.05 / (lum + 0.05);
            double ink = (lum + 0.05) / (Luminance(Ink) + 0.05);
            return white >= ink ? "#ffffff" : Ink;
        }

        // ---- methods -----------------------------------------------------------------------------------------

        private static JsonObject MergeMethods(object? methods, IReadOnlyList<string> chains)
        {
            var merged = NewDefaultMethods();
            if (Py.Truthy(methods))
            {
                if (!(methods is JsonObject given)) throw new TypeError("methods must be a dict");
                foreach (var entry in given)
                {
                    if (!merged.TryGetValue(entry.Key, out var current))
                        throw new ConfigError("unknown method: " + Py.Repr(entry.Key), "methods.unknown_method");
                    if (!(entry.Value is JsonObject opts)) throw new TypeError("methods." + entry.Key + " must be a dict");
                    var target = (JsonObject)current!;
                    foreach (var option in opts)
                    {
                        if (!target.ContainsKey(option.Key))
                            throw new ConfigError("unknown option " + Py.Repr(option.Key) + " for method " + Py.Repr(entry.Key),
                                                  "methods.unknown_option");
                        target[option.Key] = option.Value;
                    }
                }
            }
            foreach (var entry in merged)
            {
                if (!(((JsonObject)entry.Value!)["enabled"] is bool))
                    throw new ConfigError("methods." + entry.Key + ".enabled must be True or False", "methods.enabled_not_bool");
            }
            var timeout = ((JsonObject)merged["browser"]!)["timeout_min"];
            bool timeoutOk = TryInt(timeout, out var minutes) ? minutes.Sign > 0 : timeout is double d && d > 0;
            if (!timeoutOk)
                throw new ConfigError("methods.browser.timeout_min must be a number of minutes above 0", "methods.bad_timeout");
            if (!merged.Values.Any(o => (bool)((JsonObject)o!)["enabled"]!))
                throw new ConfigError("at least one payment method must be enabled", "methods.none_enabled");
            var qrChains = ((JsonObject)merged["qr"]!)["chains"];
            if (qrChains != null)
            {
                foreach (var c in Iterate(qrChains))
                {
                    if (!(c is string cs && chains.Contains(cs)))
                        throw new ConfigError("methods.qr.chains contains disabled chain " + Py.Repr(c), "methods.qr_disabled_chain");
                }
            }
            return merged;
        }

        // ---- build_config -----------------------------------------------------------------------------------

        private static readonly string[] ConfigKeys =
            { "wallet", "testnet", "chains", "tokens", "methods", "rpc_urls", "confirmations", "previous_wallets",
              "logo", "accent", "custom_chains", "solana_wallet" };

        private static Chain RegistryChain(Dictionary<string, Chain> registry, string key)
        {
            if (registry.TryGetValue(key, out var c)) return c;
            throw new KeyError(key);
        }

        private static JsonObject DictArg(object? value, string what)
        {
            if (value is JsonObject d) return d;
            throw new TypeError(what + " must be a dict");
        }

        /// <summary>
        /// build_config's keyword arguments: wallet, testnet, chains, tokens, methods, rpc_urls, confirmations,
        /// previous_wallets, logo, accent, custom_chains (a list of <see cref="Chain"/> from <see cref="BuildChain"/>;
        /// those of the other network are ignored; any other entry is a ConfigError) and solana_wallet.
        /// </summary>
        public static ConfigData BuildConfig(JsonObject kwargs)
        {
            CheckKwargs(kwargs, "build_config", ConfigKeys, new string[0]);
            var walletArg = Opt(kwargs, "wallet");
            var solanaArg = Opt(kwargs, "solana_wallet");
            bool testnet = Py.Truthy(Opt(kwargs, "testnet", false));
            var (custom, otherNetwork) = CustomChainsFor(Opt(kwargs, "custom_chains"), testnet);
            var registry = new Dictionary<string, Chain>(Chains.GetChains(testnet));
            foreach (var kv in custom) registry[kv.Key] = kv.Value;
            if (walletArg == null && solanaArg == null)
                throw new ConfigError("configure() needs wallet= (an EVM 0x address) and/or solana_wallet=", "wallet.missing");
            string? wallet = walletArg != null ? Address(walletArg, "wallet") : null;
            string? sol = solanaArg != null ? SolanaAddress(solanaArg, "solana_wallet") : null;
            var prev = new List<string>();
            var prevSol = new List<string>();
            var previousWallets = Opt(kwargs, "previous_wallets");
            if (Py.Truthy(previousWallets))
            {
                foreach (var w in Iterate(previousWallets))
                {
                    var (family, address) = AnyWallet(w, "previous_wallets");
                    string? current = family == "solana" ? sol : wallet;
                    var seen = family == "solana" ? prevSol : prev;
                    if (address != current && !seen.Contains(address)) seen.Add(address);
                }
            }

            var chainsArg = Opt(kwargs, "chains");
            List<object?> chainValues;
            if (chainsArg == null)
            {
                chainValues = new List<object?>();
                if (!string.IsNullOrEmpty(wallet))
                {
                    chainValues.AddRange(Chains.DisplayOrder);
                    chainValues.AddRange(custom.Keys);
                }
                if (!string.IsNullOrEmpty(sol))
                    chainValues.AddRange(registry.Where(kv => kv.Value.Family == "solana").Select(kv => kv.Key));
            }
            else
            {
                chainValues = Iterate(chainsArg);
            }
            if (chainValues.Count == 0) throw new ConfigError("chains must not be empty", "chains.empty");
            foreach (var c in chainValues) CheckChain(c, testnet, otherNetwork, registry, wallet, sol);
            // Every entry is now a registry key, a string.
            var chains = chainValues.Cast<string>().ToList();
            if (new HashSet<string>(chains, StringComparer.Ordinal).Count != chains.Count)
                throw new ConfigError("duplicate chain in chains", "chains.duplicate");

            var tokensArg = Opt(kwargs, "tokens");
            var tokens = new List<string>();
            var tokenValues = tokensArg == null ? Tokens.Cast<object?>().ToList() : Iterate(tokensArg);
            if (tokenValues.Count == 0) throw new ConfigError("tokens must not be empty", "tokens.empty");
            foreach (var t in tokenValues)
            {
                if (!(t is string ts && Tokens.Contains(ts)))
                    throw new ConfigError("unknown token: " + Py.Repr(t), "tokens.unknown");
                tokens.Add(ts);
            }

            var merged = MergeMethods(Opt(kwargs, "methods"), chains);

            var urls = new Dictionary<string, IReadOnlyList<string>>();
            var rpcUrlsArg = Opt(kwargs, "rpc_urls");
            if (Py.Truthy(rpcUrlsArg))
            {
                foreach (var kv in DictArg(rpcUrlsArg, "rpc_urls"))
                {
                    if (!chains.Contains(kv.Key))
                        throw new ConfigError("rpc_urls for unknown or disabled chain " + Py.Repr(kv.Key), "rpc_urls.unknown_chain");
                    urls[kv.Key] = RpcUrls(kv.Value, "rpc_urls[" + Py.Repr(kv.Key) + "]");
                }
            }

            var confs = new Dictionary<string, BigInteger>();
            var confirmationsArg = Opt(kwargs, "confirmations");
            if (Py.Truthy(confirmationsArg))
            {
                foreach (var kv in DictArg(confirmationsArg, "confirmations"))
                {
                    if (!chains.Contains(kv.Key))
                        throw new ConfigError("confirmations for unknown or disabled chain " + Py.Repr(kv.Key), "confirmations.unknown_chain");
                    if (RegistryChain(registry, kv.Key).Family == "solana")
                        throw new ConfigError("Solana payments wait for finalization; confirmations can't be set for "
                                              + Py.Repr(kv.Key), "confirmations.solana_fixed");
                    if (!TryInt(kv.Value, out var n) || n < 1)
                        throw new ConfigError("confirmations must be an int >= 1", "confirmations.invalid");
                    confs[kv.Key] = n;
                }
            }

            string? logo = LogoArg(Opt(kwargs, "logo"));
            string accent = AccentArg(Opt(kwargs, "accent"));
            var confirmations = new Dictionary<string, long>();
            foreach (var kv in confs) confirmations[kv.Key] = ToLong(kv.Value, "confirmations");

            var cfg = new ConfigData(wallet, testnet, chains, tokens, merged, urls, confirmations, prev, sol, prevSol,
                                     logo, accent, custom);
            if (EnabledChains(cfg).Count == 0)
                throw new ConfigError("no enabled chain supports any enabled token", "config.no_usable_token");
            return cfg;
        }

        // One entry of `chains`, checked as the loop in build_config checks it.
        private static void CheckChain(object? c, bool testnet, HashSet<string> otherNetwork,
                                       Dictionary<string, Chain> registry, string? wallet, string? sol)
        {
            Hashable(c);
            if (c is string cs && otherNetwork.Contains(cs))
            {
                string net = testnet ? "mainnet" : "testnet";
                throw new ConfigError("chain " + Py.Repr(cs) + " was only added for " + net
                                      + "; add it again with testnet=" + (testnet ? "True" : "False") + " to use it here",
                                      "chains.wrong_network");
            }
            if (!(c is string key) || !registry.ContainsKey(key))
                throw new ConfigError("unknown chain: " + Py.Repr(c), "chains.unknown");
            string family = registry[key].Family;
            if (family == "solana" && sol == null)
                throw new ConfigError("chain " + Py.Repr(key) + " needs solana_wallet=", "chain.needs_solana_wallet");
            if (family == "evm" && wallet == null)
                throw new ConfigError("chain " + Py.Repr(key) + " needs wallet= (an EVM 0x address)", "chain.needs_evm_wallet");
        }

        /// <summary>
        /// Chains the player may pay on, in display order, filtered to enabled tokens, with developer RPC URLs tried
        /// first and confirmation overrides applied.
        /// </summary>
        public static Dictionary<string, Chain> EnabledChains(ConfigData config)
        {
            var registry = config.Registry;
            var result = new Dictionary<string, Chain>();
            foreach (var key in config.Chains)
            {
                var chain = RegistryChain(registry, key);
                var tokens = new Dictionary<string, Token>();
                foreach (var kv in chain.Tokens)
                    if (config.Tokens.Contains(kv.Key)) tokens[kv.Key] = kv.Value;
                if (tokens.Count == 0) continue;
                var rpc = new List<string>();
                if (config.RpcUrls.TryGetValue(key, out var extra)) rpc.AddRange(extra);
                rpc.AddRange(chain.Rpc);
                long confirmations = config.Confirmations.TryGetValue(key, out var n) ? n : chain.Confirmations;
                result[key] = chain.With(rpc, confirmations, tokens);
            }
            return result;
        }

        public static List<string> ChainsForToken(ConfigData config, string token) =>
            EnabledChains(config).Where(kv => kv.Value.Tokens.ContainsKey(token)).Select(kv => kv.Key).ToList();

        // ---- add_item ---------------------------------------------------------------------------------------

        private static readonly string[] ItemKeys = { "code", "price", "token", "title", "min_price" };
        private static readonly string[] ItemRequired = { "code", "price", "token", "title" };

        /// <summary>
        /// Declares an item on <paramref name="config"/>. <paramref name="kwargs"/> holds add_item's keyword arguments:
        /// code, price, token, title and min_price (default: the price). <paramref name="itemId"/> is the positional
        /// argument, any value, so a non-str is the ConfigError Python raises.
        /// </summary>
        public static Item AddItem(ConfigData config, object? itemId, JsonObject kwargs)
        {
            CheckKwargs(kwargs, "add_item", ItemKeys, ItemRequired);
            var code = kwargs["code"];
            var price = kwargs["price"];
            var token = kwargs["token"];
            var title = kwargs["title"];
            var minPrice = Opt(kwargs, "min_price");

            if (!IsId(itemId, 64, false))
                throw new ConfigError("invalid item id " + Py.Repr(itemId) + " (use a-z, 0-9, _ and -)", "item.invalid_id");
            var id = (string)itemId!;
            if (config.Items.ContainsKey(id))
                throw new ConfigError("duplicate item id " + Py.Repr(id), "item.duplicate_id");
            if (!TryInt(code, out var codeValue) || codeValue < 1 || codeValue > 99)
                throw new ConfigError("item code must be an int in 1..99", "item.invalid_code");
            int itemCode = (int)codeValue;
            if (config.ItemByCode(itemCode) != null)
                throw new ConfigError("duplicate item code " + itemCode.ToString(CultureInfo.InvariantCulture), "item.duplicate_code");
            Dec p, mp;
            try
            {
                p = Amounts.ParseAmount(price);
                mp = minPrice == null ? p : Amounts.ParseAmount(minPrice);
            }
            catch (Exception e) when (e is ValueError || e is TypeError)
            {
                throw new ConfigError("item " + Py.Repr(id) + ": " + e.Message, "item.invalid_price");
            }
            if (mp > p) throw new ConfigError("min_price must not exceed price", "item.min_price_above_price");
            if (!(token is string tokenText) || !config.Tokens.Contains(tokenText) || ChainsForToken(config, tokenText).Count == 0)
                throw new ConfigError("token " + Py.Repr(token) + " is not available on any enabled chain", "token.unavailable");
            if (!(title is string titleText) || Py.Strip(titleText).Length == 0 || Py.Len(titleText) > 80)
                throw new ConfigError("title must be a non-empty string of at most 80 characters", "item.invalid_title");
            var item = new Item(id, itemCode, p, tokenText, titleText, mp);
            config.Items[id] = item;
            return item;
        }
    }
}
