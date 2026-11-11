/**
 * Developer configuration: validation of configure()/addItem() arguments (twin of scfg_core/config.py).
 *
 * Every mistake raises ConfigError at startup, so it surfaces while the developer is testing
 * instead of when a player tries to pay. Option objects use the Python kwarg names; unknown keys
 * and missing required ones are a TypeError, like a Python call with a bad signature.
 *
 * JS has one number type: an integral `number` stands for a Python `int`, a non-integral one for
 * a `float`. Python's integral float (`5.0`) cannot be expressed.
 *
 * Item ids and chain keys may be integer-like ("10"), and a JS object lists those keys first, so
 * every dict keyed by them whose order shows (`items`, `custom_chains`, `registry()`,
 * `enabledChains()`) is a `Map`, which keeps insertion order as a Python dict does.
 * `rpc_urls` and `confirmations` are only looked up, never iterated, and stay plain objects.
 */
import { parseAmount } from "./amounts.ts";
import { DISPLAY_ORDER, makeToken, getChains, type Chain, type Token } from "./chains.ts";
import { deepCopy } from "./copy.ts";
import { Dec } from "./decimal.ts";
import { ConfigError, IndexError, KeyError, ValueError } from "./errors.ts";
import { normalizeAddress } from "./evm.ts";
import type { Item } from "./models.ts";
import { PY_DIGIT, hasOwn, pyFormat1, pyStrip, pyTruthy } from "./pystr.ts";
import { MIN_LOG_RANGE } from "./rpc.ts";
import { b58decode, normalizePubkey } from "./solana/base58.ts";
import { isOnCurve } from "./solana/ed25519.ts";

export const TOKENS: readonly string[] = ["USDC", "USDT"];
export const DEFAULT_METHODS: Record<string, Record<string, unknown>> = {
  browser: { enabled: true, timeout_min: 30 },
  qr: { enabled: true, wallet_button: true, chains: null },
  manual: { enabled: true, allow_tx_paste: true },
};
// The id patterns match the whole string (Python's fullmatch: no trailing newline).
const ITEM_ID = /^[a-z0-9_-]{1,64}$/;
const CHAIN_KEY = /^[a-z0-9_-]{1,32}$/;
const NATIVE_SYMBOL = /^[A-Za-z0-9]{1,11}$/;
// Plain http is only accepted for a node on this computer (e.g. a local Anvil test chain). Python's `\d` is
// Unicode (PY_DIGIT: Python 3.9's digits); the URL ends there or continues with a path (`\Z`: no trailing newline).
const LOCAL_HTTP = new RegExp(`^http://(127\\.0\\.0\\.1|localhost)(:${PY_DIGIT}{1,5})?(/|$)`, "u");
export const FEE_HINTS: readonly string[] = ["lowest", "low", "high"];

export const DEFAULT_ACCENT = "#0e7c74";
export const INK = "#17202e";
// Raster formats only: an SVG served from the checkout page's origin could run script.
export const LOGO_TYPES: Record<string, string> = {
  ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp",
};
const HEX_COLOR = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

/** Set an own data property (a plain `o[k] = v` on `__proto__` would change the prototype). */
function put<T>(o: Record<string, T>, k: string, v: T): void {
  Object.defineProperty(o, k, { value: v, writable: true, enumerable: true, configurable: true });
}

const isDict = (v: unknown): v is Record<string, any> => typeof v === "object" && v !== null && !Array.isArray(v);
const isInt = (v: unknown): v is number => typeof v === "number" && Number.isSafeInteger(v);
const cpLength = (s: string): number => [...s].length;

/** The TypeError Python's `key in container` raises for an unhashable key (a list or dict). */
function hashable(key: unknown): void {
  if (typeof key === "object" && key !== null) throw new TypeError(`unhashable type: ${Array.isArray(key) ? "list" : "dict"}`);
}

/** Python `tuple(x)` / iteration for the JSON-like values a caller can pass. */
function iterate(value: unknown, what: string): unknown[] {
  if (Array.isArray(value)) return [...value];
  if (typeof value === "string") return [...value];
  if (isDict(value)) return Object.keys(value);
  throw new TypeError(`${what} is not iterable`);
}

/** Reject keys outside `allowed` and missing `required` ones, like Python's keyword-only parameters. */
function checkOpts(opts: unknown, allowed: readonly string[], required: readonly string[], fn: string): Record<string, any> {
  if (!isDict(opts)) throw new TypeError(`${fn}() options must be an object`);
  for (const k of Object.keys(opts)) {
    if (!allowed.includes(k)) throw new TypeError(`${fn}() got an unexpected keyword argument '${k}'`);
  }
  for (const k of required) {
    if (opts[k] === undefined) throw new TypeError(`${fn}() missing required keyword-only argument: '${k}'`);
  }
  return opts;
}

const noneish = (v: unknown): boolean => v === null || v === undefined;

export interface Config {
  wallet: string | null; // EVM payout address; null for a Solana-only game
  testnet: boolean;
  chains: string[];
  tokens: string[];
  methods: Record<string, Record<string, any>>;
  rpc_urls: Record<string, string[]>;
  confirmations: Record<string, number>;
  previous_wallets: string[];
  solana_wallet: string | null;
  previous_solana_wallets: string[];
  logo: string | null; // image path, shown on the screens and the checkout page
  accent: string; // #rrggbb used for selected options
  custom_chains: Map<string, Chain>; // addChain(), this network, in the order added
  items: Map<string, Item>; // in the order added
}

/** Every chain this game knows on its network: built-in, then the developer's own. */
export function registry(config: Config): Map<string, Chain> {
  return new Map([...Object.entries(getChains(config.testnet)), ...config.custom_chains]);
}

/** EVM: current wallet first, then wallets used by earlier versions of the game. */
export function acceptedWallets(config: Config): string[] {
  return [...(config.wallet ? [config.wallet] : []), ...config.previous_wallets];
}

export function acceptedWalletsFor(config: Config, family: string): string[] {
  if (family === "solana") {
    return [...(config.solana_wallet ? [config.solana_wallet] : []), ...config.previous_solana_wallets];
  }
  return acceptedWallets(config);
}

export function walletFor(config: Config, chain: Pick<Chain, "family">): string | null {
  return chain.family === "solana" ? config.solana_wallet : config.wallet;
}

export function itemByCode(config: Config, code: number): Item | null {
  for (const item of config.items.values()) {
    if (item.code === code) return item;
  }
  return null;
}

function address(value: unknown, what: string): string {
  try {
    return normalizeAddress(value as string);
  } catch (e) {
    if (e instanceof ValueError) throw new ConfigError(`${what}: ${e.message}`, { code: "address.invalid" });
    throw e;
  }
}

function solanaAddress(value: unknown, what: string): string {
  let addr: string;
  try {
    addr = normalizePubkey(value);
  } catch (e) {
    if (e instanceof ValueError) {
      throw new ConfigError(`${what}: not a Solana address: ${JSON.stringify(value)}`, { code: "solana_address.invalid" });
    }
    throw e;
  }
  if (!isOnCurve(b58decode(addr))) {
    throw new ConfigError(
      `${what}: ${addr} looks like a token account or program address; use your wallet address`,
      { code: "solana_address.not_on_curve" });
  }
  return addr;
}

/** [family, normalized address]: 0x addresses are EVM, anything else Solana. */
function anyWallet(value: unknown, what: string): ["evm" | "solana", string] {
  if (typeof value === "string" && pyStrip(value).startsWith("0x")) return ["evm", address(pyStrip(value), what)];
  return ["solana", solanaAddress(value, what)];
}

function rpcUrls(value: unknown, what: string): string[] {
  const urls = typeof value === "string" ? [value] : value;
  if (!Array.isArray(urls) || urls.length === 0) {
    throw new ConfigError(`${what} must be a URL or a non-empty list of URLs`, { code: "rpc.bad_list" });
  }
  for (const u of urls) {
    if (typeof u !== "string" || !(u.startsWith("https://") || LOCAL_HTTP.test(u))) {
      throw new ConfigError(`rpc url must start with https://: ${JSON.stringify(u)}`, { code: "rpc.not_https" });
    }
  }
  return [...urls];
}

function positiveInt(value: unknown, what: string, minimum = 1): number {
  if (!isInt(value) || value < minimum) {
    throw new ConfigError(`${what} must be an int >= ${minimum}`, { code: "int.below_minimum" });
  }
  return value;
}

export interface BuildChainOptions {
  name: string;
  chain_id: number;
  rpc: string | string[];
  tokens: Record<string, string>;
  confirmations: number;
  explorer_tx: string;
  native_symbol?: string;
  fee_hint?: string;
  max_log_range?: number;
  testnet?: boolean;
}

/**
 * A developer-defined EVM chain (scfg.add_chain). Validated like the rest of the configuration,
 * so a typo fails at startup rather than when a player pays.
 *
 * Only EVM chains work: payments are found through ERC-20 Transfer logs over Ethereum JSON-RPC.
 * The tokens must have 6 decimals, because the item code and nonce live in the last four decimals.
 */
export function buildChain(key: unknown, opts: BuildChainOptions): Chain {
  const o = checkOpts(opts, ["name", "chain_id", "rpc", "tokens", "confirmations", "explorer_tx", "native_symbol",
    "fee_hint", "max_log_range", "testnet"], ["name", "chain_id", "rpc", "tokens", "confirmations", "explorer_tx"], "build_chain");
  if (key === undefined) throw new TypeError("build_chain() missing 1 required positional argument: 'key'");
  const nativeSymbol = o["native_symbol"] === undefined ? "ETH" : o["native_symbol"];
  const feeHint = o["fee_hint"] === undefined ? "low" : o["fee_hint"];
  const maxLogRange = o["max_log_range"] === undefined ? 1000 : o["max_log_range"];
  const testnet = o["testnet"] === undefined ? false : o["testnet"];
  if (typeof key !== "string" || !CHAIN_KEY.test(key)) {
    throw new ConfigError(`invalid chain key ${JSON.stringify(key)} (use a-z, 0-9, _ and -)`, { code: "chain.invalid_key" });
  }
  const what = `chain '${key}'`;
  const name = o["name"];
  if (typeof name !== "string" || !pyStrip(name) || cpLength(name) > 32) {
    throw new ConfigError(`${what}: name must be a non-empty string of at most 32 characters`, { code: "chain.invalid_name" });
  }
  const chainId = positiveInt(o["chain_id"], `${what}: chain_id`);
  const rpc = rpcUrls(o["rpc"], `${what}: rpc`);
  const tokens = o["tokens"];
  if (!isDict(tokens) || Object.keys(tokens).length === 0) {
    throw new ConfigError(`${what}: tokens must be a dict like {'USDC': '0x...'}`, { code: "chain.tokens_invalid" });
  }
  const parsed: Record<string, Token> = {};
  for (const [symbol, addr] of Object.entries(tokens)) {
    if (!TOKENS.includes(symbol)) {
      throw new ConfigError(`${what}: unknown token '${symbol}' (supported: USDC, USDT)`, { code: "chain.unknown_token" });
    }
    put(parsed, symbol, makeToken(address(addr, `${what}: ${symbol} address`)));
  }
  const confirmations = positiveInt(o["confirmations"], `${what}: confirmations`);
  const explorerTx = o["explorer_tx"];
  if (typeof explorerTx !== "string" || !explorerTx.startsWith("https://") || explorerTx.split("{}").length - 1 !== 1) {
    throw new ConfigError(
      `${what}: explorer_tx must be an https:// URL with {} where the transaction hash goes, like 'https://snowtrace.io/tx/{}'`,
      { code: "chain.bad_explorer_tx" });
  }
  try {
    pyFormat1(explorerTx, "0x");
  } catch (e) {
    if (e instanceof IndexError || e instanceof KeyError || e instanceof ValueError) {
      throw new ConfigError(`${what}: explorer_tx may contain no braces besides {}`, { code: "chain.bad_explorer_tx_braces" });
    }
    throw e;
  }
  if (typeof nativeSymbol !== "string" || !NATIVE_SYMBOL.test(nativeSymbol)) {
    throw new ConfigError(`${what}: native_symbol must be letters/digits, like 'AVAX'`, { code: "chain.bad_native_symbol" });
  }
  if (!FEE_HINTS.includes(feeHint)) {
    throw new ConfigError(`${what}: fee_hint must be one of ${FEE_HINTS.join(", ")}`, { code: "chain.bad_fee_hint" });
  }
  const range = positiveInt(maxLogRange, `${what}: max_log_range`, MIN_LOG_RANGE);
  return {
    key, chain_id: chainId, name: pyStrip(name), rpc, confirmations, max_log_range: range, explorer_tx: explorerTx,
    native_symbol: nativeSymbol, fee_hint: feeHint, tokens: parsed, testnet: pyTruthy(testnet),
    family: "evm", wallet_chain: "", native_token: "", min_fee_wei: 0,
  };
}

/** Python `isinstance(x, Chain)` for plain-data chains. */
function isChain(x: unknown): x is Chain {
  return isDict(x) && typeof x["key"] === "string" && typeof x["chain_id"] === "number" && Array.isArray(x["rpc"])
    && isDict(x["tokens"]) && typeof x["testnet"] === "boolean" && typeof x["family"] === "string"
    && typeof x["confirmations"] === "number";
}

/** [chains for this network keyed by key, keys only defined for the other network]. */
function customChainsFor(custom: unknown, testnet: boolean): [Map<string, Chain>, Set<string>] {
  const builtin = getChains(testnet);
  const mine = new Map<string, Chain>();
  const other = new Set<string>();
  for (const chain of noneish(custom) || !pyTruthy(custom) ? [] : iterate(custom, "custom_chains")) {
    if (!isChain(chain)) {
      throw new ConfigError("custom chains must be created with scfg.add_chain()", { code: "chain.not_from_add_chain" });
    }
    if (hasOwn(getChains(false), chain.key) || hasOwn(getChains(true), chain.key)) {
      throw new ConfigError(
        `chain '${chain.key}' is built in; use rpc_urls/confirmations to change it, or pick another key`,
        { code: "chain.is_builtin" });
    }
    if (chain.testnet !== testnet) {
      other.add(chain.key);
      continue;
    }
    if (mine.has(chain.key)) {
      throw new ConfigError(`chain '${chain.key}' added twice`, { code: "chain.duplicate" });
    }
    for (const c of [...Object.values(builtin), ...mine.values()]) {
      if (c.chain_id === chain.chain_id) {
        throw new ConfigError(`chain '${chain.key}' has the same chain_id (${chain.chain_id}) as '${c.key}'`,
          { code: "chain.duplicate_chain_id" });
      }
    }
    mine.set(chain.key, chain);
  }
  for (const k of mine.keys()) other.delete(k);
  return [mine, other];
}

function logoArg(value: unknown): string | null {
  if (noneish(value)) return null;
  if (typeof value !== "string" || !value || value.includes("\\") || value.includes(":") || value.startsWith("/")
    || value.split("/").includes("..")) {
    throw new ConfigError(
      `logo must be a path inside the game folder, like 'images/logo.png': ${JSON.stringify(value)}`,
      { code: "logo.bad_path" });
  }
  logoContentType(value);
  return value;
}

export function logoContentType(path: string): string {
  const ext = path.includes(".") ? path.slice(path.lastIndexOf(".")).toLowerCase() : "";
  if (!hasOwn(LOGO_TYPES, ext)) {
    throw new ConfigError(`logo must be a .png, .jpg or .webp image: ${JSON.stringify(path)}`, { code: "logo.bad_type" });
  }
  return LOGO_TYPES[ext]!;
}

function accentArg(value: unknown): string {
  if (noneish(value)) return DEFAULT_ACCENT;
  if (typeof value !== "string" || !HEX_COLOR.test(value)) {
    throw new ConfigError(`accent must be a color like '#0e7c74': ${JSON.stringify(value)}`, { code: "accent.invalid" });
  }
  let digits = value.slice(1);
  if (digits.length === 3) digits = [...digits].map((c) => c + c).join("");
  return "#" + digits.slice(0, 6).toLowerCase(); // alpha, if any, is dropped
}

/** WCAG relative luminance of a #rrggbb color. */
function luminance(color: string): number {
  const channel = (i: number): number => {
    const c = parseInt(color.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5);
}

/** White or ink, whichever has the higher WCAG contrast on `color` (#rrggbb). */
export function textOn(color: string): string {
  const lum = luminance(color);
  const white = 1.05 / (lum + 0.05);
  const ink = (lum + 0.05) / (luminance(INK) + 0.05);
  return white >= ink ? "#ffffff" : INK;
}

function mergeMethods(methods: unknown, chains: readonly string[]): Record<string, Record<string, any>> {
  const merged = deepCopy(DEFAULT_METHODS) as Record<string, Record<string, any>>;
  if (pyTruthy(methods)) {
    if (!isDict(methods)) throw new TypeError("methods must be a dict");
    for (const [name, opts] of Object.entries(methods)) {
      if (!hasOwn(merged, name)) throw new ConfigError(`unknown method: '${name}'`, { code: "methods.unknown_method" });
      if (!isDict(opts)) throw new TypeError(`methods.${name} must be a dict`);
      for (const [key, value] of Object.entries(opts)) {
        if (!hasOwn(merged[name]!, key)) {
          throw new ConfigError(`unknown option '${key}' for method '${name}'`, { code: "methods.unknown_option" });
        }
        put(merged[name]!, key, value);
      }
    }
  }
  for (const [name, opts] of Object.entries(merged)) {
    if (typeof opts["enabled"] !== "boolean") {
      throw new ConfigError(`methods.${name}.enabled must be True or False`, { code: "methods.enabled_not_bool" });
    }
  }
  const timeout = merged["browser"]!["timeout_min"];
  const numeric = typeof timeout === "number" || typeof timeout === "bigint";
  if (!numeric || !((timeout as number | bigint) > 0)) {
    throw new ConfigError("methods.browser.timeout_min must be a number of minutes above 0", { code: "methods.bad_timeout" });
  }
  if (!Object.values(merged).some((o) => o["enabled"])) {
    throw new ConfigError("at least one payment method must be enabled", { code: "methods.none_enabled" });
  }
  const qrChains = merged["qr"]!["chains"];
  if (qrChains !== null && qrChains !== undefined) {
    for (const c of iterate(qrChains, "methods.qr.chains")) {
      if (!chains.includes(c as string)) {
        throw new ConfigError(`methods.qr.chains contains disabled chain ${JSON.stringify(c)}`, { code: "methods.qr_disabled_chain" });
      }
    }
  }
  return merged;
}

export interface BuildConfigOptions {
  wallet?: string | null;
  testnet?: boolean;
  chains?: string[] | null;
  tokens?: string[] | null;
  methods?: Record<string, Record<string, unknown>> | null;
  rpc_urls?: Record<string, string | string[]> | null;
  confirmations?: Record<string, number> | null;
  previous_wallets?: string[] | null;
  logo?: string | null;
  accent?: string | null;
  custom_chains?: Chain[] | null;
  solana_wallet?: string | null;
}

/** custom_chains: Chains from buildChain(); those of the other network are ignored. */
export function buildConfig(options: BuildConfigOptions = {}): Config {
  const o = checkOpts(options, ["wallet", "testnet", "chains", "tokens", "methods", "rpc_urls", "confirmations",
    "previous_wallets", "logo", "accent", "custom_chains", "solana_wallet"], [], "build_config");
  const testnet = pyTruthy(o["testnet"]);
  const [custom, otherNetwork] = customChainsFor(o["custom_chains"], testnet);
  const reg = new Map<string, Chain>([...Object.entries(getChains(testnet)), ...custom]);
  if (noneish(o["wallet"]) && noneish(o["solana_wallet"])) {
    throw new ConfigError("configure() needs wallet= (an EVM 0x address) and/or solana_wallet=", { code: "wallet.missing" });
  }
  const wallet = noneish(o["wallet"]) ? null : address(o["wallet"], "wallet");
  const sol = noneish(o["solana_wallet"]) ? null : solanaAddress(o["solana_wallet"], "solana_wallet");
  const prev: string[] = [];
  const prevSol: string[] = [];
  for (const w of pyTruthy(o["previous_wallets"]) ? iterate(o["previous_wallets"], "previous_wallets") : []) {
    const [family, addr] = anyWallet(w, "previous_wallets");
    const [current, seen] = family === "solana" ? [sol, prevSol] : [wallet, prev];
    if (addr !== current && !seen.includes(addr)) seen.push(addr);
  }

  const solanaKeys = [...reg].filter(([, c]) => c.family === "solana").map(([k]) => k);
  const chains: string[] = noneish(o["chains"])
    ? [...(wallet ? [...DISPLAY_ORDER, ...custom.keys()] : []), ...(sol ? solanaKeys : [])]
    : (iterate(o["chains"], "chains") as string[]);
  if (chains.length === 0) throw new ConfigError("chains must not be empty", { code: "chains.empty" });
  for (const c of chains) {
    hashable(c);
    if (typeof c === "string" && otherNetwork.has(c)) {
      const net = testnet ? "mainnet" : "testnet";
      throw new ConfigError(
        `chain '${c}' was only added for ${net}; add it again with testnet=${testnet ? "True" : "False"} to use it here`,
        { code: "chains.wrong_network" });
    }
    if (typeof c !== "string" || !reg.has(c)) throw new ConfigError(`unknown chain: ${JSON.stringify(c)}`, { code: "chains.unknown" });
    const family = reg.get(c)!.family;
    if (family === "solana" && sol === null) {
      throw new ConfigError(`chain '${c}' needs solana_wallet=`, { code: "chain.needs_solana_wallet" });
    }
    if (family === "evm" && wallet === null) {
      throw new ConfigError(`chain '${c}' needs wallet= (an EVM 0x address)`, { code: "chain.needs_evm_wallet" });
    }
  }
  if (new Set(chains).size !== chains.length) throw new ConfigError("duplicate chain in chains", { code: "chains.duplicate" });

  const tokens: string[] = noneish(o["tokens"]) ? [...TOKENS] : (iterate(o["tokens"], "tokens") as string[]);
  if (tokens.length === 0) throw new ConfigError("tokens must not be empty", { code: "tokens.empty" });
  for (const t of tokens) {
    if (!TOKENS.includes(t)) throw new ConfigError(`unknown token: ${JSON.stringify(t)}`, { code: "tokens.unknown" });
  }

  const merged = mergeMethods(o["methods"], chains);

  const urls: Record<string, string[]> = {};
  const urlEntries = pyTruthy(o["rpc_urls"]) ? dictItems(o["rpc_urls"], "rpc_urls") : [];
  for (const [c, lst] of urlEntries) {
    if (!chains.includes(c)) {
      throw new ConfigError(`rpc_urls for unknown or disabled chain '${c}'`, { code: "rpc_urls.unknown_chain" });
    }
    put(urls, c, rpcUrls(lst, `rpc_urls['${c}']`));
  }

  const confs: Record<string, number> = {};
  const confEntries = pyTruthy(o["confirmations"]) ? dictItems(o["confirmations"], "confirmations") : [];
  for (const [c, n] of confEntries) {
    if (!chains.includes(c)) {
      throw new ConfigError(`confirmations for unknown or disabled chain '${c}'`, { code: "confirmations.unknown_chain" });
    }
    if (reg.get(c)!.family === "solana") {
      throw new ConfigError(`Solana payments wait for finalization; confirmations can't be set for '${c}'`,
        { code: "confirmations.solana_fixed" });
    }
    if (!isInt(n) || n < 1) throw new ConfigError("confirmations must be an int >= 1", { code: "confirmations.invalid" });
    put(confs, c, n);
  }

  const cfg: Config = {
    wallet, testnet, chains, tokens, methods: merged, rpc_urls: urls, confirmations: confs, previous_wallets: prev,
    solana_wallet: sol, previous_solana_wallets: prevSol, logo: logoArg(o["logo"]), accent: accentArg(o["accent"]),
    custom_chains: custom, items: new Map(),
  };
  if (enabledChains(cfg).size === 0) {
    throw new ConfigError("no enabled chain supports any enabled token", { code: "config.no_usable_token" });
  }
  return cfg;
}

function dictItems(value: unknown, what: string): [string, any][] {
  if (!isDict(value)) throw new TypeError(`${what} must be a dict`);
  return Object.entries(value);
}

/**
 * Chains the player may pay on, in display order, filtered to enabled tokens, with developer RPC
 * URLs tried first and confirmation overrides applied.
 */
export function enabledChains(config: Config): Map<string, Chain> {
  const reg = registry(config);
  const out = new Map<string, Chain>();
  for (const key of config.chains) {
    const base = reg.get(key)!;
    const tokens: Record<string, Token> = {};
    for (const [t, v] of Object.entries(base.tokens)) {
      if (config.tokens.includes(t)) put(tokens, t, v);
    }
    if (Object.keys(tokens).length === 0) continue;
    out.set(key, {
      ...base,
      tokens,
      rpc: [...(hasOwn(config.rpc_urls, key) ? config.rpc_urls[key]! : []), ...base.rpc],
      confirmations: hasOwn(config.confirmations, key) ? config.confirmations[key]! : base.confirmations,
    });
  }
  return out;
}

export function chainsForToken(config: Config, token: string): string[] {
  return [...enabledChains(config)].filter(([, c]) => hasOwn(c.tokens, token)).map(([k]) => k);
}

export interface AddItemOptions {
  code: number;
  price: unknown;
  token: string;
  title: string;
  min_price?: unknown;
}

export function addItem(config: Config, item_id: unknown, opts: AddItemOptions): Item {
  const o = checkOpts(opts, ["code", "price", "token", "title", "min_price"], ["code", "price", "token", "title"], "add_item");
  const { code, price, token, title } = o;
  if (typeof item_id !== "string" || !ITEM_ID.test(item_id)) {
    throw new ConfigError(`invalid item id ${JSON.stringify(item_id)} (use a-z, 0-9, _ and -)`, { code: "item.invalid_id" });
  }
  if (config.items.has(item_id)) {
    throw new ConfigError(`duplicate item id '${item_id}'`, { code: "item.duplicate_id" });
  }
  if (!isInt(code) || code < 1 || code > 99) {
    throw new ConfigError("item code must be an int in 1..99", { code: "item.invalid_code" });
  }
  if (itemByCode(config, code) !== null) {
    throw new ConfigError(`duplicate item code ${code}`, { code: "item.duplicate_code" });
  }
  let p: Dec;
  let mp: Dec;
  try {
    p = parseAmount(price);
    mp = noneish(o["min_price"]) ? p : parseAmount(o["min_price"]);
  } catch (e) {
    if (e instanceof ValueError || e instanceof TypeError) {
      throw new ConfigError(`item '${item_id}': ${e.message}`, { code: "item.invalid_price" });
    }
    throw e;
  }
  if (mp.cmp(p) > 0) throw new ConfigError("min_price must not exceed price", { code: "item.min_price_above_price" });
  if (!config.tokens.includes(token) || chainsForToken(config, token).length === 0) {
    throw new ConfigError(`token ${JSON.stringify(token)} is not available on any enabled chain`, { code: "token.unavailable" });
  }
  if (typeof title !== "string" || !pyStrip(title) || cpLength(title) > 80) {
    throw new ConfigError("title must be a non-empty string of at most 80 characters", { code: "item.invalid_title" });
  }
  const item: Item = { item_id, code, price: p, token, title, min_price: mp };
  config.items.set(item_id, item);
  return item;
}
