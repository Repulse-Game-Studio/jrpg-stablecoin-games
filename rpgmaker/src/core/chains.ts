/**
 * Registry of supported chains and stablecoin contracts (twin of scfg_core/chains.py).
 *
 * Token addresses come from Circle's and Tether's official contract lists. Solana mints are
 * compared exactly (base58), never case-folded. Tokens are identified by contract address,
 * never by symbol. Testnet entries use the same keys so a game can switch with `testnet`.
 * Developers' own EVM chains (config.buildChain) never enter these records; they live on the Config.
 *
 * Log sources: on Arc, USDC is the native gas coin: a plain native send logs a Transfer only at
 * a system emitter address (18 decimals), while an ERC-20 transfer() logs there *and* at the
 * 0x3600 interface (6 decimals). Such a token names the emitter as its log_address, and
 * parseTransfer reads only that stream, floored to 6-decimal units.
 *
 * MAINNET and TESTNET are mutable records: the integration driver points entries at local nodes.
 */
import { asUnits } from "./amounts.ts";
import { ValueError } from "./errors.ts";
import { parseTransferLog, toChecksumAddress, type TransferLog } from "./evm.ts";
import { pyFormat1 } from "./pystr.ts";
import { normalizePubkey } from "./solana/base58.ts";

export const ARC_USDC = "0x3600000000000000000000000000000000000000";
export const ARC_SYSTEM_EMITTER = "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE";
export const ARC_MIN_FEE_WEI = 20_000_000_000n; // Arc's mempool silently drops anything below 20 gwei

/** Python `Token` dataclass (frozen there; plain data here). */
export interface Token {
  address: string;
  decimals: number;
  /** Where Transfer events are logged when that is not the token's own contract (Arc's system emitter). */
  log_address: string;
  /** Decimals of the values logged at log_address. */
  log_decimals: number;
}

/** Python `Token(address, decimals, log_address, log_decimals)` incl. `__post_init__` normalisation. */
export function makeToken(
  address: string,
  opts: { decimals?: number; log_address?: string; log_decimals?: number } = {},
): Token {
  if (typeof address !== "string") throw new TypeError("token address must be a string");
  const decimals = opts.decimals ?? 6;
  let log_address = opts.log_address ?? "";
  const log_decimals = opts.log_decimals ?? 0;
  // EVM addresses are always EIP-55 checksummed; Solana mints are base58 and compared exactly.
  const canonical = address.startsWith("0x") ? toChecksumAddress(address) : normalizePubkey(address);
  if (log_address !== "") {
    log_address = toChecksumAddress(log_address);
    if (log_decimals < decimals) throw new ValueError("log_decimals must be at least decimals");
  }
  return { address: canonical, decimals, log_address, log_decimals };
}

/** The contract address whose Transfer events record this token's payments. */
export function logSource(token: Token): string {
  return token.log_address || token.address;
}

/** Python `Chain` dataclass; every field is part of the cross-engine wire format. */
export interface Chain {
  key: string;
  chain_id: number;
  name: string;
  rpc: readonly string[];
  confirmations: number;
  max_log_range: number;
  explorer_tx: string;
  native_symbol: string;
  fee_hint: string;
  tokens: Record<string, Token>;
  testnet: boolean;
  family: string; // "evm" | "solana"
  wallet_chain: string;
  native_token: string; // the token in `tokens` that is also the gas coin (Arc: "USDC")
  min_fee_wei: number; // maxFeePerGas floor the checkout page enforces (0: none)
}

/** `explorer_tx.format(tx_hash)` */
export function explorerUrl(chain: Chain, tx_hash: string): string {
  return pyFormat1(chain.explorer_tx, tx_hash);
}

/**
 * A raw log as a payment in one of this chain's tokens, or null. Only logs from a token's log
 * source count. Values logged with more decimals than the token has are floored to its units;
 * a log that floors to 0 is not a payment. The result always carries the token's canonical address.
 */
export function parseTransfer(chain: Chain, log: Record<string, any>): TransferLog | null {
  const t = parseTransferLog(log);
  if (t === null) return null;
  const token = Object.values(chain.tokens).find((tk) => logSource(tk) === t.token);
  if (token === undefined) return null;
  // The token's own contract logs in the token's decimals: its raw value already is base units.
  if (!token.log_address) return { ...t, value: asUnits(t.value) };
  const value = t.value / 10n ** BigInt(token.log_decimals - token.decimals);
  if (value <= 0n) return null;
  return { ...t, token: token.address, value: asUnits(value) };
}

export const MAINNET: Record<string, Chain> = {
  "ethereum": {
    key: "ethereum", chain_id: 1, name: "Ethereum",
    rpc: ["https://ethereum-rpc.publicnode.com", "https://eth.drpc.org"],
    confirmations: 12, max_log_range: 125,
    explorer_tx: "https://etherscan.io/tx/{}", native_symbol: "ETH", fee_hint: "high",
    tokens: { "USDC": makeToken("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"), "USDT": makeToken("0xdAC17F958D2ee523a2206206994597C13D831ec7") },
    testnet: false, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "arbitrum": {
    key: "arbitrum", chain_id: 42161, name: "Arbitrum",
    rpc: ["https://arb1.arbitrum.io/rpc", "https://arbitrum.drpc.org", "https://1rpc.io/arb", "https://arbitrum-one-rpc.publicnode.com"],
    confirmations: 20, max_log_range: 2000,
    explorer_tx: "https://arbiscan.io/tx/{}", native_symbol: "ETH", fee_hint: "low",
    tokens: { "USDC": makeToken("0xaf88d065e77c8cC2239327C5EDb3A432268e5831"), "USDT": makeToken("0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9") },
    testnet: false, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "polygon": {
    key: "polygon", chain_id: 137, name: "Polygon",
    rpc: ["https://polygon-bor-rpc.publicnode.com", "https://polygon.drpc.org", "https://1rpc.io/matic"],
    confirmations: 32, max_log_range: 500,
    explorer_tx: "https://polygonscan.com/tx/{}", native_symbol: "POL", fee_hint: "lowest",
    tokens: { "USDC": makeToken("0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359"), "USDT": makeToken("0xc2132D05D31c914a87C6611C10748AEb04B58e8F") },
    testnet: false, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "base": {
    key: "base", chain_id: 8453, name: "Base",
    rpc: ["https://mainnet.base.org", "https://base-rpc.publicnode.com", "https://base.drpc.org", "https://1rpc.io/base"],
    confirmations: 10, max_log_range: 125,
    explorer_tx: "https://basescan.org/tx/{}", native_symbol: "ETH", fee_hint: "low",
    tokens: { "USDC": makeToken("0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913") },
    testnet: false, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "arc": {
    key: "arc", chain_id: 5042, name: "Arc",
    rpc: ["https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io", "https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io"],
    confirmations: 1, max_log_range: 9999,
    explorer_tx: "https://explorer.arc.io/tx/{}", native_symbol: "USDC", fee_hint: "lowest",
    tokens: { "USDC": makeToken(ARC_USDC, { log_address: ARC_SYSTEM_EMITTER, log_decimals: 18 }) },
    testnet: false, family: "evm", wallet_chain: "",
    native_token: "USDC", min_fee_wei: Number(ARC_MIN_FEE_WEI),
  },
  "solana": {
    key: "solana", chain_id: 0, name: "Solana",
    rpc: ["https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"],
    confirmations: 32, max_log_range: 0,
    explorer_tx: "https://solscan.io/tx/{}", native_symbol: "SOL", fee_hint: "lowest",
    tokens: { "USDC": makeToken("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"), "USDT": makeToken("Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB") },
    testnet: false, family: "solana", wallet_chain: "solana:mainnet",
    native_token: "", min_fee_wei: 0,
  },
};

export const TESTNET: Record<string, Chain> = {
  "ethereum": {
    key: "ethereum", chain_id: 11155111, name: "Sepolia",
    rpc: ["https://ethereum-sepolia-rpc.publicnode.com", "https://1rpc.io/sepolia"],
    confirmations: 3, max_log_range: 2000,
    explorer_tx: "https://sepolia.etherscan.io/tx/{}", native_symbol: "ETH", fee_hint: "high",
    tokens: { "USDC": makeToken("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238") },
    testnet: true, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "arbitrum": {
    key: "arbitrum", chain_id: 421614, name: "Arbitrum Sepolia",
    rpc: ["https://sepolia-rollup.arbitrum.io/rpc", "https://arbitrum-sepolia-rpc.publicnode.com"],
    confirmations: 3, max_log_range: 2000,
    explorer_tx: "https://sepolia.arbiscan.io/tx/{}", native_symbol: "ETH", fee_hint: "low",
    tokens: { "USDC": makeToken("0x75faf114eafb1BDbe2F0316DF893fd58CE46AA4d") },
    testnet: true, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "polygon": {
    key: "polygon", chain_id: 80002, name: "Polygon Amoy",
    rpc: ["https://polygon-amoy-bor-rpc.publicnode.com"],
    confirmations: 3, max_log_range: 2000,
    explorer_tx: "https://amoy.polygonscan.com/tx/{}", native_symbol: "POL", fee_hint: "lowest",
    tokens: { "USDC": makeToken("0x41E94Eb019C0762f9Bfcf9Fb1E58725BfB0e7582") },
    testnet: true, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "base": {
    key: "base", chain_id: 84532, name: "Base Sepolia",
    rpc: ["https://base-sepolia-rpc.publicnode.com", "https://sepolia.base.org"],
    confirmations: 3, max_log_range: 2000,
    explorer_tx: "https://sepolia.basescan.org/tx/{}", native_symbol: "ETH", fee_hint: "low",
    tokens: { "USDC": makeToken("0x036CbD53842c5426634e7929541eC2318f3dCF7e") },
    testnet: true, family: "evm", wallet_chain: "",
    native_token: "", min_fee_wei: 0,
  },
  "arc": {
    key: "arc", chain_id: 5042002, name: "Arc Testnet",
    rpc: ["https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io", "https://rpc.quicknode.testnet.arc.io"],
    confirmations: 1, max_log_range: 9999,
    explorer_tx: "https://explorer.testnet.arc.io/tx/{}", native_symbol: "USDC", fee_hint: "lowest",
    tokens: { "USDC": makeToken(ARC_USDC, { log_address: ARC_SYSTEM_EMITTER, log_decimals: 18 }) },
    testnet: true, family: "evm", wallet_chain: "",
    native_token: "USDC", min_fee_wei: Number(ARC_MIN_FEE_WEI),
  },
  "solana": {
    key: "solana", chain_id: 0, name: "Solana Devnet",
    rpc: ["https://api.devnet.solana.com"],
    confirmations: 32, max_log_range: 0,
    explorer_tx: "https://solscan.io/tx/{}?cluster=devnet", native_symbol: "SOL", fee_hint: "lowest",
    tokens: { "USDC": makeToken("4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU") },
    testnet: true, family: "solana", wallet_chain: "solana:devnet",
    native_token: "", min_fee_wei: 0,
  },
};

export const DISPLAY_ORDER: readonly string[] = ["arbitrum", "base", "arc", "polygon", "ethereum"];

export function getChains(testnet: boolean): Record<string, Chain> {
  return testnet ? TESTNET : MAINNET;
}
