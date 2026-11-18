// Twin of scfg_core/chains.py (and of rpgmaker/src/core/chains.ts).
// Registry of supported chains and stablecoin contracts.
//
// Token addresses come from Circle's and Tether's official contract lists. Solana mints are compared exactly
// (base58), never case-folded. Tokens are identified by contract address, never by symbol. Testnet entries use the
// same keys so a game can switch with testnet=true. Developers' own EVM chains (Config.BuildChain) never enter
// these dictionaries; they live on the ConfigData.
//
// Log sources: on most chains a token's Transfer events come from its own contract. On Arc, USDC is the native gas
// coin: a plain native send logs a Transfer only at a system emitter address (18 decimals), while an ERC-20
// transfer() logs there and at the 0x3600 interface (6 decimals). Such a token names the emitter as its LogAddress,
// and Chain.ParseTransfer reads only that stream, floored to 6-decimal units.
//
// Mainnet, Testnet and every Chain are read-only through the public API: with Unity's domain reload off, statics
// survive between play sessions, so nothing a game does may change them. The integration driver and the tests point
// an entry at a local node through the internal Chains.Override and restore the built-in tables with
// Chains.ResetRegistries (InternalsVisibleTo in AssemblyInfo.cs).
#nullable enable
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Numerics;
using Scfg.Core.Solana;

namespace Scfg.Core
{
    /// <summary>Python's frozen <c>Token</c> dataclass, including <c>__post_init__</c>'s normalisation.</summary>
    public sealed class Token
    {
        /// <summary>EIP-55 checksummed for an EVM token; the base58 mint for Solana.</summary>
        public string Address { get; }
        public int Decimals { get; }
        /// <summary>Where this token's Transfer events are logged, when that is not its own contract (Arc's system emitter).</summary>
        public string LogAddress { get; }
        /// <summary>The decimals of the values logged at <see cref="LogAddress"/>.</summary>
        public int LogDecimals { get; }

        /// <summary>
        /// EVM addresses are always EIP-55 checksummed, so comparisons with parsed logs never depend on case. Solana
        /// mints are base58 and compared exactly. A log address needs <c>logDecimals &gt;= decimals</c> (ValueError).
        /// </summary>
        public Token(string address, int decimals = 6, string logAddress = "", int logDecimals = 0)
        {
            if (address == null) throw new TypeError("token address must be a string");
            Address = address.StartsWith("0x", StringComparison.Ordinal)
                ? Evm.ToChecksumAddress(address)
                : Base58.NormalizePubkey(address);
            Decimals = decimals;
            LogAddress = "";
            LogDecimals = logDecimals;
            if (!string.IsNullOrEmpty(logAddress))
            {
                LogAddress = Evm.ToChecksumAddress(logAddress);
                if (logDecimals < decimals) throw new ValueError("log_decimals must be at least decimals");
            }
        }

        /// <summary>The contract address whose Transfer events record this token's payments.</summary>
        public string LogSource => LogAddress.Length != 0 ? LogAddress : Address;
    }

    /// <summary>
    /// Python's frozen <c>Chain</c> dataclass. Immutable: the constructor copies <c>rpc</c> and <c>tokens</c> into
    /// read-only collections. <see cref="With"/> stands in for <c>dataclasses.replace</c>. The integers are 64-bit
    /// here (Python's are unbounded).
    /// </summary>
    public sealed class Chain
    {
        public string Key { get; }
        public long ChainId { get; }
        public string Name { get; }
        public IReadOnlyList<string> Rpc { get; }
        public long Confirmations { get; }
        public long MaxLogRange { get; }
        public string ExplorerTx { get; }
        public string NativeSymbol { get; }
        public string FeeHint { get; }
        /// <summary>Symbol to token, in declaration order.</summary>
        public IReadOnlyDictionary<string, Token> Tokens { get; }
        public bool Testnet { get; }
        /// <summary>"evm" or "solana".</summary>
        public string Family { get; }
        public string WalletChain { get; }
        /// <summary>The token in <see cref="Tokens"/> that is also the gas coin (Arc: "USDC").</summary>
        public string NativeToken { get; }
        /// <summary>The maxFeePerGas floor the checkout page enforces (0: none).</summary>
        public long MinFeeWei { get; }

        public Chain(string key, long chainId, string name, IReadOnlyList<string> rpc, long confirmations,
                     long maxLogRange, string explorerTx, string nativeSymbol, string feeHint,
                     IReadOnlyDictionary<string, Token>? tokens = null, bool testnet = false, string family = "evm",
                     string walletChain = "", string nativeToken = "", long minFeeWei = 0)
        {
            Key = key;
            ChainId = chainId;
            Name = name;
            Rpc = Array.AsReadOnly((rpc ?? throw new TypeError("rpc must be a list of URLs")).ToArray());
            Confirmations = confirmations;
            MaxLogRange = maxLogRange;
            ExplorerTx = explorerTx;
            NativeSymbol = nativeSymbol;
            FeeHint = feeHint;
            var copy = new Dictionary<string, Token>();
            if (tokens != null)
                foreach (var kv in tokens) copy.Add(kv.Key, kv.Value);
            Tokens = new ReadOnlyDictionary<string, Token>(copy);
            Testnet = testnet;
            Family = family;
            WalletChain = walletChain;
            NativeToken = nativeToken;
            MinFeeWei = minFeeWei;
        }

        /// <summary><c>dataclasses.replace</c> for the fields a caller changes: a null argument keeps the field.</summary>
        public Chain With(IReadOnlyList<string>? rpc = null, long? confirmations = null,
                          IReadOnlyDictionary<string, Token>? tokens = null) =>
            new Chain(Key, ChainId, Name, rpc ?? Rpc, confirmations ?? Confirmations, MaxLogRange, ExplorerTx,
                      NativeSymbol, FeeHint, tokens ?? Tokens, Testnet, Family, WalletChain, NativeToken, MinFeeWei);

        /// <summary><c>explorer_tx.format(tx_hash)</c>.</summary>
        public string ExplorerUrl(string txHash) => Py.Format1(ExplorerTx, txHash);

        /// <summary>
        /// A raw log as a payment in one of this chain's tokens, or null. Only logs from a token's log source count.
        /// Values logged with more decimals than the token has are floored to its units; a log that floors to 0 is not
        /// a payment. The result always carries the token's canonical address.
        /// </summary>
        public TransferLog? ParseTransfer(JsonObject log)
        {
            var t = Evm.ParseTransferLog(log);
            if (t == null) return null;
            Token? token = null;
            foreach (var tk in Tokens.Values)
            {
                if (tk.LogSource == t.Token)
                {
                    token = tk;
                    break;
                }
            }
            if (token == null) return null;
            if (token.LogAddress.Length == 0) return t;
            var value = BigInteger.Divide(t.Value, BigInteger.Pow(10, token.LogDecimals - token.Decimals));
            if (value.Sign <= 0) return null;
            return new TransferLog(token.Address, t.Sender, t.Recipient, value, t.TxHash, t.LogIndex, t.BlockNumber,
                                   t.BlockHash);
        }
    }

    public static class Chains
    {
        public const string ArcUsdc = "0x3600000000000000000000000000000000000000";
        public const string ArcSystemEmitter = "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE";
        /// <summary>Arc's mempool silently drops anything below 20 gwei.</summary>
        public const long ArcMinFeeWei = 20_000_000_000;

        /// <summary>Python <c>Token(address, decimals, log_address, log_decimals)</c> (the driver builds tokens with it).</summary>
        public static Token MakeToken(string address, int decimals = 6, string logAddress = "", int logDecimals = 0) =>
            new Token(address, decimals, logAddress, logDecimals);

        private static IReadOnlyDictionary<string, Token> Toks(params (string Symbol, string Address)[] tokens)
        {
            var d = new Dictionary<string, Token>();
            foreach (var (symbol, address) in tokens) d[symbol] = new Token(address);
            return d;
        }

        private static IReadOnlyDictionary<string, Token> ArcTokens() =>
            new Dictionary<string, Token>
            {
                ["USDC"] = new Token(ArcUsdc, logAddress: ArcSystemEmitter, logDecimals: 18),
            };

        /// <summary>The built-in mainnet chains. The first RPC endpoint of a chain should be the most generous.</summary>
        private static Dictionary<string, Chain> BuiltinMainnet() => new Dictionary<string, Chain>
        {
            ["ethereum"] = new Chain(
                "ethereum", 1, "Ethereum",
                new[] { "https://ethereum-rpc.publicnode.com", "https://eth.drpc.org" },
                12, 125, "https://etherscan.io/tx/{}", "ETH", "high",
                Toks(("USDC", "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"),
                     ("USDT", "0xdAC17F958D2ee523a2206206994597C13D831ec7"))),
            ["arbitrum"] = new Chain(
                "arbitrum", 42161, "Arbitrum",
                // publicnode only serves very recent Arbitrum blocks without a token: last resort.
                new[]
                {
                    "https://arb1.arbitrum.io/rpc", "https://arbitrum.drpc.org", "https://1rpc.io/arb",
                    "https://arbitrum-one-rpc.publicnode.com",
                },
                20, 2000, "https://arbiscan.io/tx/{}", "ETH", "low",
                Toks(("USDC", "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"),
                     ("USDT", "0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9"))),
            ["polygon"] = new Chain(
                "polygon", 137, "Polygon",
                new[] { "https://polygon-bor-rpc.publicnode.com", "https://polygon.drpc.org", "https://1rpc.io/matic" },
                32, 500, "https://polygonscan.com/tx/{}", "POL", "lowest",
                Toks(("USDC", "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359"),
                     ("USDT", "0xc2132D05D31c914a87C6611C10748AEb04B58e8F"))),
            ["base"] = new Chain(
                "base", 8453, "Base",
                new[]
                {
                    "https://mainnet.base.org", "https://base-rpc.publicnode.com", "https://base.drpc.org",
                    "https://1rpc.io/base",
                },
                10, 125, "https://basescan.org/tx/{}", "ETH", "low",
                Toks(("USDC", "0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913"))),
            ["arc"] = new Chain(
                "arc", 5042, "Arc",
                // dRPC caps getLogs at a few dozen blocks, so it is never first, and never last either: the last
                // endpoint is the one handed to a wallet that adds the network.
                new[]
                {
                    "https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io",
                    "https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io",
                },
                1, 9_999, // BFT finality: one block is final
                "https://explorer.arc.io/tx/{}", "USDC", "lowest",
                ArcTokens(), nativeToken: "USDC", minFeeWei: ArcMinFeeWei),
            ["solana"] = new Chain(
                "solana", 0, "Solana",
                // publicnode answers fast but keeps ~2 days of history; mainnet-beta keeps all of it.
                new[] { "https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com" },
                32, 0, "https://solscan.io/tx/{}", "SOL", "lowest",
                Toks(("USDC", "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
                     ("USDT", "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB")),
                family: "solana", walletChain: "solana:mainnet"),
        };

        /// <summary>The built-in testnet chains, under the mainnet keys.</summary>
        private static Dictionary<string, Chain> BuiltinTestnet() => new Dictionary<string, Chain>
        {
            ["ethereum"] = new Chain(
                "ethereum", 11155111, "Sepolia",
                new[] { "https://ethereum-sepolia-rpc.publicnode.com", "https://1rpc.io/sepolia" },
                3, 2000, "https://sepolia.etherscan.io/tx/{}", "ETH", "high",
                Toks(("USDC", "0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238")), testnet: true),
            ["arbitrum"] = new Chain(
                "arbitrum", 421614, "Arbitrum Sepolia",
                new[] { "https://sepolia-rollup.arbitrum.io/rpc", "https://arbitrum-sepolia-rpc.publicnode.com" },
                3, 2000, "https://sepolia.arbiscan.io/tx/{}", "ETH", "low",
                Toks(("USDC", "0x75faf114eafb1BDbe2F0316DF893fd58CE46AA4d")), testnet: true),
            ["polygon"] = new Chain(
                "polygon", 80002, "Polygon Amoy",
                new[] { "https://polygon-amoy-bor-rpc.publicnode.com" },
                3, 2000, "https://amoy.polygonscan.com/tx/{}", "POL", "lowest",
                Toks(("USDC", "0x41E94Eb019C0762f9Bfcf9Fb1E58725BfB0e7582")), testnet: true),
            ["base"] = new Chain(
                "base", 84532, "Base Sepolia",
                new[] { "https://base-sepolia-rpc.publicnode.com", "https://sepolia.base.org" },
                3, 2000, "https://sepolia.basescan.org/tx/{}", "ETH", "low",
                Toks(("USDC", "0x036CbD53842c5426634e7929541eC2318f3dCF7e")), testnet: true),
            ["arc"] = new Chain(
                "arc", 5042002, "Arc Testnet",
                // Blockdaemon's testnet endpoint answers gzip unasked (urllib can't read it): left out.
                new[] { "https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io", "https://rpc.quicknode.testnet.arc.io" },
                1, 9_999, "https://explorer.testnet.arc.io/tx/{}", "USDC", "lowest",
                ArcTokens(), testnet: true, nativeToken: "USDC", minFeeWei: ArcMinFeeWei),
            ["solana"] = new Chain(
                "solana", 0, "Solana Devnet",
                new[] { "https://api.devnet.solana.com" },
                32, 0, "https://solscan.io/tx/{}?cluster=devnet", "SOL", "lowest",
                Toks(("USDC", "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU")),
                testnet: true, family: "solana", walletChain: "solana:devnet"),
        };

        // The live tables behind the read-only views; only Override and ResetRegistries write them.
        private static readonly Dictionary<string, Chain> MainnetTable = BuiltinMainnet(); // static-ok: private, written only by the internal Override/ResetRegistries
        private static readonly Dictionary<string, Chain> TestnetTable = BuiltinTestnet(); // static-ok: private, written only by the internal Override/ResetRegistries

        /// <summary>The built-in mainnet chains (read-only).</summary>
        public static readonly IReadOnlyDictionary<string, Chain> Mainnet = new ReadOnlyDictionary<string, Chain>(MainnetTable);
        /// <summary>The built-in testnet chains (read-only).</summary>
        public static readonly IReadOnlyDictionary<string, Chain> Testnet = new ReadOnlyDictionary<string, Chain>(TestnetTable);

        public static readonly IReadOnlyList<string> DisplayOrder =
            Array.AsReadOnly(new[] { "arbitrum", "base", "arc", "polygon", "ethereum" });

        public static IReadOnlyDictionary<string, Chain> GetChains(bool testnet) => testnet ? Testnet : Mainnet;

        /// <summary>
        /// Test and driver hook: replace the built-in entry <c>chain.Key</c> of one network (keeping the registry's
        /// order), so the integration driver can point it at a local node. Only an existing key (else a KeyError) on
        /// the chain's own network (else a ValueError). Undo with <see cref="ResetRegistries"/>.
        /// </summary>
        internal static void Override(bool testnet, Chain chain)
        {
            if (chain == null) throw new TypeError("chain must be a Chain");
            var table = testnet ? TestnetTable : MainnetTable;
            if (!table.ContainsKey(chain.Key)) throw new KeyError(chain.Key);
            if (chain.Testnet != testnet)
                throw new ValueError("chain " + Py.Repr(chain.Key) + " is not a " + (testnet ? "testnet" : "mainnet") + " chain");
            table[chain.Key] = chain;
        }

        /// <summary>Test and driver hook: rebuild both registries from code, undoing every <see cref="Override"/>.</summary>
        internal static void ResetRegistries()
        {
            Refill(MainnetTable, BuiltinMainnet());
            Refill(TestnetTable, BuiltinTestnet());
        }

        private static void Refill(Dictionary<string, Chain> table, Dictionary<string, Chain> builtin)
        {
            table.Clear();
            foreach (var kv in builtin) table.Add(kv.Key, kv.Value);
        }
    }
}
