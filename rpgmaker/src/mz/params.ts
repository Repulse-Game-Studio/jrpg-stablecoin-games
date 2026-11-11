/**
 * The plugin parameters, as MZ's Plugin Manager hands them over (`PluginManager.parameters("SCFG")`):
 * every value is a string. A struct is the JSON text of an object of strings, and a struct list
 * (`struct<Item>[]`) the JSON text of an array of such JSON texts, so an Items list is decoded
 * twice. Booleans are "true" / "false", numbers and switch ids their digits, an unset value "".
 *
 * `parseParams` turns them into `configure()` options (the Python option names), the items, and
 * the plugin's own settings. The advanced options (chains, tokens, methods, rpcUrls,
 * confirmations, previousWallets, customChains: Ren'Py's chains, tokens, methods, rpc_urls,
 * confirmations, previous_wallets and add_chain) are JSON text, as written in a `multiline_string`
 * (also accepted JSON-quoted, as a `note` stores it). A value it cannot read is a ConfigError
 * naming the parameter; boot reports it and leaves the plugin disabled (boot never throws). What the
 * core refuses in a value it can read (a Solana confirmation count, a bad chain) is the core's
 * own ConfigError, reported the same way.
 */
import { ConfigError } from "../core/index.ts";

/** Parameter names (the `@param` names of the header; the Plugin Manager shows their `@text`). */
export const PARAM = {
  wallet: "wallet",
  solanaWallet: "solanaWallet",
  testnet: "testnet",
  logo: "logo",
  accent: "accent",
  items: "items",
  notify: "notify",
  tipJarMenu: "tipJarMenu",
  tipJarLabel: "tipJarLabel",
  texts: "texts",
  chains: "chains",
  tokens: "tokens",
  methods: "methods",
  rpcUrls: "rpcUrls",
  confirmations: "confirmations",
  previousWallets: "previousWallets",
  customChains: "customChains",
} as const;

/** Field names of one `struct<Item>` entry. */
export const ITEM_FIELD = {
  id: "id", code: "code", price: "price", token: "token", title: "title", minPrice: "minPrice", ownedSwitch: "ownedSwitch",
} as const;

/** Field names of one `struct<Text>` entry. */
export const TEXT_FIELD = { id: "id", text: "text" } as const;

export const DEFAULT_TIP_JAR_LABEL = "Tip Jar";
/** The suggested tip of the Tip command (empty Amount) and the Tip Jar, as the Ren'Py and Godot demos. */
export const DEFAULT_TIP_AMOUNT = "1.00";

/** One Items entry, ready for `SCFG.addItem(id, {code, price, token, title, min_price})`. */
export interface ParsedItem {
  id: string;
  code: unknown;
  price: string;
  token: string;
  title: string;
  min_price: string | null;
  /** The switch the plugin keeps ON while the item is owned; 0 = none. */
  ownedSwitch: number;
}

export interface ParsedParams {
  /** `configure()` options; null when neither wallet is set (the game may configure from a script). */
  configure: Record<string, unknown> | null;
  items: ParsedItem[];
  notify: boolean;
  tipJarMenu: boolean;
  tipJarLabel: string;
  /** Text overrides, message id -> text. */
  texts: Record<string, string>;
  /** The Custom Chains parameter: [key, `SCFG.addChain` options], added before configure. */
  customChains: [string, Record<string, unknown>][];
}

const bad = (param: string, why: string): ConfigError =>
  new ConfigError(`plugin parameter ${param} ${why}`, { code: "params.invalid" });

const str = (raw: Record<string, unknown>, key: string): string => {
  const v = raw[key];
  return typeof v === "string" ? v.trim() : v === undefined || v === null ? "" : String(v).trim();
};

function bool(raw: Record<string, unknown>, key: string, fallback: boolean): boolean {
  const v = str(raw, key);
  if (v === "") return fallback;
  if (v === "true") return true;
  if (v === "false") return false;
  throw bad(key, `must be true or false, not ${JSON.stringify(v)}`);
}

/** JSON text, or undefined for "". */
function json(raw: Record<string, unknown>, key: string): unknown {
  const v = str(raw, key);
  if (v === "") return undefined;
  try {
    return JSON.parse(v);
  } catch {
    throw bad(key, `is not valid JSON: ${JSON.stringify(v.length > 80 ? v.slice(0, 80) + "..." : v)}`);
  }
}

/**
 * An advanced option's JSON text. The header declares these `multiline_string` (stored as
 * written); a value stored JSON-quoted (MZ's `note` type, a hand-edited plugins.js) is the JSON
 * text of the JSON text, so a string is read once more. None of these options is a string.
 */
function jsonOption(raw: Record<string, unknown>, key: string): unknown {
  const v = json(raw, key);
  if (typeof v !== "string") return v;
  return json({ [key]: v }, key);
}

/** A struct list: an array of structs, each the JSON text of an object (MZ's encoding). */
function structList(raw: Record<string, unknown>, key: string): Record<string, unknown>[] {
  const list = json(raw, key);
  if (list === undefined) return [];
  if (!Array.isArray(list)) throw bad(key, "is not a list");
  return list.map((entry, i) => {
    let obj: unknown = entry;
    if (typeof entry === "string") {
      try {
        obj = JSON.parse(entry);
      } catch {
        throw bad(key, `entry ${i + 1} is not valid JSON`);
      }
    }
    if (typeof obj !== "object" || obj === null || Array.isArray(obj)) throw bad(key, `entry ${i + 1} is not a struct`);
    return obj as Record<string, unknown>;
  });
}

/** An integer parameter as a number; other text is passed on as it is, for the core to refuse. */
function intOrText(v: string): unknown {
  return /^-?\d+$/.test(v) ? Number(v) : v;
}

const isPlainObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

/**
 * Custom Chains: a JSON object of `SCFG.addChain(key, opts)` calls, like
 * `{"optimism": {"name": "Optimism", "chain_id": 10, "rpc": ["https://..."], "tokens": {"USDC": "0x..."},
 * "confirmations": 10, "explorer_tx": "https://optimistic.etherscan.io/tx/{}"}}`. The options
 * themselves are the core's to check (buildChain).
 */
function customChains(raw: Record<string, unknown>): [string, Record<string, unknown>][] {
  const v = jsonOption(raw, PARAM.customChains);
  if (v === undefined) return [];
  if (!isPlainObject(v)) throw bad(PARAM.customChains, "must be a JSON object of chains by key, like {\"optimism\": {...}}");
  return Object.entries(v).map(([key, opts]) => {
    if (!isPlainObject(opts)) throw bad(PARAM.customChains, `entry ${JSON.stringify(key)} is not an object of addChain options`);
    return [key, opts];
  });
}

export function parseParams(raw: Record<string, unknown> | null | undefined): ParsedParams {
  const p = raw ?? {};
  const wallet = str(p, PARAM.wallet);
  const solana = str(p, PARAM.solanaWallet);
  let configure: Record<string, unknown> | null = null;
  if (wallet !== "" || solana !== "") {
    configure = { testnet: bool(p, PARAM.testnet, false) };
    if (wallet !== "") configure["wallet"] = wallet;
    if (solana !== "") configure["solana_wallet"] = solana;
    const logo = str(p, PARAM.logo);
    if (logo !== "") configure["logo"] = logo;
    const accent = str(p, PARAM.accent);
    if (accent !== "") configure["accent"] = accent;
    for (const [param, option] of [[PARAM.chains, "chains"], [PARAM.tokens, "tokens"], [PARAM.methods, "methods"],
      [PARAM.rpcUrls, "rpc_urls"], [PARAM.confirmations, "confirmations"], [PARAM.previousWallets, "previous_wallets"]] as const) {
      const v = jsonOption(p, param);
      if (v !== undefined) configure[option] = v;
    }
  }
  const items = structList(p, PARAM.items).map((s, i): ParsedItem => {
    const id = str(s, ITEM_FIELD.id);
    const sw = str(s, ITEM_FIELD.ownedSwitch);
    if (sw !== "" && !/^\d+$/.test(sw)) throw bad(PARAM.items, `entry ${i + 1}: the owned switch must be a switch number, not ${JSON.stringify(sw)}`);
    const minPrice = str(s, ITEM_FIELD.minPrice);
    return {
      id, code: intOrText(str(s, ITEM_FIELD.code)), price: str(s, ITEM_FIELD.price), token: str(s, ITEM_FIELD.token),
      title: str(s, ITEM_FIELD.title), min_price: minPrice === "" ? null : minPrice, ownedSwitch: sw === "" ? 0 : Number(sw),
    };
  });
  // the plugin keeps each owned switch equal to owns(id) (switches.ts): one switch, one item
  const switchOwner = new Map<number, string>();
  for (const [i, item] of items.entries()) {
    if (item.ownedSwitch === 0) continue;
    const other = switchOwner.get(item.ownedSwitch);
    if (other !== undefined) {
      throw bad(PARAM.items, `entry ${i + 1}: owned switch ${item.ownedSwitch} is used by both ${other} and ${item.id}; give each item its own switch`);
    }
    switchOwner.set(item.ownedSwitch, item.id);
  }
  if (items.length > 0 && configure === null) throw bad(PARAM.items, "needs wallet (an EVM 0x address) and/or solanaWallet");
  const texts: Record<string, string> = {};
  for (const [i, s] of structList(p, PARAM.texts).entries()) {
    const id = str(s, TEXT_FIELD.id);
    if (id === "") throw bad(PARAM.texts, `entry ${i + 1} has no id`);
    // defineProperty: an id such as "__proto__" stays an own key
    Object.defineProperty(texts, id, {
      value: typeof s[TEXT_FIELD.text] === "string" ? s[TEXT_FIELD.text] : "", enumerable: true, writable: true, configurable: true,
    });
  }
  const label = str(p, PARAM.tipJarLabel);
  return {
    configure, items, notify: bool(p, PARAM.notify, true), tipJarMenu: bool(p, PARAM.tipJarMenu, false),
    tipJarLabel: label === "" ? DEFAULT_TIP_JAR_LABEL : label, texts, customChains: customChains(p),
  };
}
