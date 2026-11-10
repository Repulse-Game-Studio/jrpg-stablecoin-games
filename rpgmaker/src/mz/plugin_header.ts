/**
 * The RPG Maker MZ plugin header: the comment blocks MZ's Plugin Manager reads from the top of
 * `js/plugins/SCFG.js`. The build (`build.mjs`) emits `renderHeader()` as the bundle's banner, so
 * it comes before the IIFE:
 *
 * - the `/*:` block: target, description, author, help, the plugin parameters (`@param`) and the
 *   plugin commands (`@command`, `@arg`);
 * - one `/*~struct~Name:` block per struct type (`Item`, `Text`).
 *
 * The parameter keys are the ones params.ts reads (PARAM, ITEM_FIELD, TEXT_FIELD; Ruling 4: the
 * `@param` key is camelCase, the editor shows the `@text`), and commands.ts registers the commands
 * and reads the arguments named here (COMMAND, ARG, COMMAND_ARGS), so the header cannot drift from
 * the code. This module stays light (no glue imports): build.mjs loads it in Node.
 *
 * MZ's annotation format is written from its public plugin documentation, not checked against the
 * real editor (no MZ here): see rpgmaker/demo/README.md.
 */
import { DEFAULT_TIP_AMOUNT, DEFAULT_TIP_JAR_LABEL, ITEM_FIELD, PARAM, TEXT_FIELD } from "./params.ts";

/** The plugin's file and command name: `js/plugins/SCFG.js`, commands registered under "SCFG". */
export const PLUGIN_NAME = "SCFG";

/** The Plugin Manager's one-line description (`@plugindesc`). */
export const PLUGIN_DESC = "Stablecoin for Games (SCFG): USDC/USDT unlocks and tips paid straight to your wallet";

/** The players' guide to wallets (the Wallet Help command opens it). */
export const WALLET_HELP_URL = "https://jrpg.com/stablecoinforgames/docs/players/wallets";

/** The plugin commands, as registered with `PluginManager.registerCommand("SCFG", name, fn)`. */
export const COMMAND = {
  unlockItem: "UnlockItem",
  tip: "Tip",
  restorePurchases: "RestorePurchases",
  walletHelp: "WalletHelp",
} as const;

/** The commands' argument names (MZ hands them to the command as strings). */
export const ARG = { item: "item", amount: "amount", token: "token", statusVariable: "statusVariable" } as const;

/** Each command's arguments, in the header's order. */
export const COMMAND_ARGS: Readonly<Record<string, readonly string[]>> = {
  [COMMAND.unlockItem]: [ARG.item, ARG.statusVariable],
  [COMMAND.tip]: [ARG.amount, ARG.token, ARG.statusVariable],
  [COMMAND.restorePurchases]: [ARG.statusVariable],
  [COMMAND.walletHelp]: [],
};

/** One `@param` or `@arg`: its annotations, rendered in this order. */
interface Decl {
  name: string;
  text: string;
  desc: string;
  type: string;
  /** Annotations between @type and @default (@min, @max, @dir, @require, @option/@value, @on/@off, @parent). */
  more?: [string, string][];
  default?: string;
}

interface Command {
  name: string;
  text: string;
  desc: string;
  args: Decl[];
}

const STATUS_VARIABLE: Decl = {
  name: ARG.statusVariable, text: "Status Variable", type: "variable", default: "0",
  desc: "Gets the status code when the player closes the screen (see Help). 0 / none: not stored.",
};

const PARAMS: Decl[] = [
  { name: PARAM.wallet, text: "Wallet", type: "string", default: "",
    desc: "Your EVM wallet address (0x...): it receives the payments on Ethereum, Arbitrum, Polygon, Base and Arc." },
  { name: PARAM.solanaWallet, text: "Solana Wallet", type: "string", default: "",
    desc: "Optional: your Solana wallet address, to take USDC/USDT on Solana too." },
  { name: PARAM.testnet, text: "Testnet", type: "boolean", more: [["on", "Testnet (no real money)"], ["off", "Mainnet"]], default: "false",
    desc: "ON: test networks with test tokens, no real money. Turn it OFF for the release." },
  { name: PARAM.logo, text: "Logo", type: "file", more: [["dir", "img/scfg/"], ["require", "1"]], default: "",
    desc: "Optional: your logo (a png) in img/scfg/, shown on the payment sheet and the checkout page." },
  { name: PARAM.accent, text: "Accent", type: "string", default: "",
    desc: "Optional: the color of selected options, like #0e7c74." },
  { name: PARAM.items, text: "Items", type: "struct<Item>[]", default: "[]",
    desc: "What players can unlock. Give each item its own Owned Switch." },
  { name: PARAM.notify, text: "Notify", type: "boolean", more: [["on", "Show"], ["off", "Don't show"]], default: "true",
    desc: "Show a short notice when a payment confirms or purchases are restored." },
  { name: PARAM.tipJarMenu, text: "Tip Jar Menu", type: "boolean", more: [["on", "Add"], ["off", "Don't add"]], default: "false",
    desc: `Add a Tip Jar command to the main menu. It opens the tip sheet (${DEFAULT_TIP_AMOUNT}, the first enabled token).` },
  { name: PARAM.tipJarLabel, text: "Tip Jar Label", type: "string", more: [["parent", PARAM.tipJarMenu]], default: DEFAULT_TIP_JAR_LABEL,
    desc: "The Tip Jar command's name in the main menu." },
  { name: PARAM.texts, text: "Texts", type: "struct<Text>[]", default: "[]",
    desc: "Your own wording or language for the player-facing texts, by id. The browser checkout page stays English." },
  { name: PARAM.chains, text: "Chains", type: "multiline_string", default: "",
    desc: "Advanced, JSON list: only these networks, like [\"base\", \"arbitrum\"]. Empty: the defaults." },
  { name: PARAM.tokens, text: "Tokens", type: "multiline_string", default: "",
    desc: "Advanced, JSON list: only these tokens, like [\"USDC\"]. Empty: the defaults." },
  { name: PARAM.methods, text: "Methods", type: "multiline_string", default: "",
    desc: "Advanced, JSON object: payment method options, like {\"manual\": {\"allow_tx_paste\": false}}. Empty: the defaults." },
  { name: PARAM.rpcUrls, text: "RPC URLs", type: "multiline_string", default: "",
    desc: "Advanced, JSON object: your own RPC URLs per network, like {\"base\": [\"https://...\"]}. Empty: the built-in ones." },
  { name: PARAM.confirmations, text: "Confirmations", type: "multiline_string", default: "",
    desc: "Advanced, JSON object: blocks to wait before a payment counts, per EVM network, like {\"base\": 12}. Empty: the built-in ones." },
  { name: PARAM.previousWallets, text: "Previous Wallets", type: "multiline_string", default: "",
    desc: "Advanced, JSON list: wallets you were paid to before (EVM or Solana), like [\"0x...\"], so Restore Purchases still finds those payments." },
  { name: PARAM.customChains, text: "Custom Chains", type: "multiline_string", default: "",
    desc: "Advanced, JSON object: your own EVM networks by key, like {\"optimism\": {\"name\": \"Optimism\", ...}} (see Help). Empty: none." },
];

const STRUCTS: [string, Decl[]][] = [
  ["Item", [
    { name: ITEM_FIELD.id, text: "Id", type: "string", default: "",
      desc: "The item's id for commands and scripts: a-z, 0-9, _ and -, like chapter2." },
    { name: ITEM_FIELD.code, text: "Code", type: "number", more: [["min", "1"], ["max", "99"]], default: "1",
      desc: "1-99, a different one for each item. It is written into players' payments: never change it after release." },
    { name: ITEM_FIELD.price, text: "Price", type: "string", default: "",
      desc: "The price, like 4.99." },
    { name: ITEM_FIELD.token, text: "Token", type: "select", more: [["option", "USDC"], ["value", "USDC"], ["option", "USDT"], ["value", "USDT"]],
      default: "USDC", desc: "The token the price is in." },
    { name: ITEM_FIELD.title, text: "Title", type: "string", default: "",
      desc: "The name players see, like Chapter 2." },
    { name: ITEM_FIELD.minPrice, text: "Min Price", type: "string", default: "",
      desc: "Optional: the lowest amount that still unlocks it (sales; keep the old price here when you raise it). Empty: the price." },
    { name: ITEM_FIELD.ownedSwitch, text: "Owned Switch", type: "switch", default: "0",
      desc: "The plugin keeps this switch ON while the item is owned (and OFF otherwise). Use it for nothing else." },
  ]],
  ["Text", [
    { name: TEXT_FIELD.id, text: "Id", type: "string", default: "",
      desc: "The text's id, like payment_confirmed (the docs list them)." },
    { name: TEXT_FIELD.text, text: "Text", type: "string", default: "",
      desc: "What to show instead. Keep each %s of the original." },
  ]],
];

const COMMANDS: Command[] = [
  { name: COMMAND.unlockItem, text: "Unlock Item", desc: "Show the payment sheet for an item and wait until the player closes it.", args: [
    { name: ARG.item, text: "Item", type: "string", default: "", desc: "The item's Id, like chapter2." },
    STATUS_VARIABLE,
  ] },
  { name: COMMAND.tip, text: "Tip", desc: "Show the tip sheet and wait until the player closes it.", args: [
    { name: ARG.amount, text: "Amount", type: "string", default: "",
      desc: `The suggested tip, like 2.50; the player may change it. Empty: ${DEFAULT_TIP_AMOUNT}.` },
    { name: ARG.token, text: "Token", type: "combo", more: [["option", "USDC"], ["option", "USDT"]], default: "",
      desc: "USDC or USDT. Empty: the first enabled token." },
    STATUS_VARIABLE,
  ] },
  { name: COMMAND.restorePurchases, text: "Restore Purchases", desc: "Show the restore screen (the player pastes a transaction) and wait until it is closed.",
    args: [STATUS_VARIABLE] },
  { name: COMMAND.walletHelp, text: "Wallet Help", desc: "Open the players' guide to wallets in the browser.", args: [] },
];

const HELP = [
  "Stablecoin for Games (SCFG) for RPG Maker MZ.",
  "",
  "Players pay for unlocks and tips in USDC or USDT, straight to your wallet:",
  "with a browser wallet, a phone wallet (QR code) or a manual transfer.",
  "No server and no accounts. Receipts are kept across restarts.",
  "",
  "SETUP",
  "1. Put SCFG.js in js/plugins/ and turn it ON in the Plugin Manager.",
  "2. Wallet: your EVM address (0x...). Solana Wallet: optional.",
  "3. Keep Testnet ON while you test (no real money); turn it OFF to release.",
  "4. Items: one entry per unlock (Id, Code 1-99, Price, Token, Title),",
  "   each with its own Owned Switch to gate your events on the purchase.",
  "",
  "OWNED SWITCHES",
  "The plugin owns those switches: each is ON exactly while its item is owned.",
  "It sets them after New Game, after every Load, when a payment confirms or",
  "purchases are restored, and when the stored receipts are first read.",
  "Don't turn them ON or OFF from events: the plugin puts them back.",
  "",
  "PLUGIN COMMANDS",
  "Unlock Item, Tip and Restore Purchases show the payment sheet (or the",
  "restore screen) and the event waits until the player closes it. Then the",
  "Status Variable holds (also as SCFG.STATUS in scripts):",
  "  0 = not done: the sheet is still open, or a save made while it was open",
  "      was loaded (the purchase itself is kept, and its switch is set)",
  "  1 = paid; Restore Purchases: restored",
  "  2 = already owned (no sheet shown); Restore Purchases: nothing found",
  "  3 = pending: the player chose Continue playing while it confirms",
  "  4 = underpaid",
  "  5 = expired",
  "  6 = cancelled",
  "  7 = failed: the command could not run (the console says why)",
  `Tip: Amount empty = ${DEFAULT_TIP_AMOUNT}, Token empty = the first enabled token.`,
  "Wallet Help opens the players' guide to wallets in the browser.",
  "",
  "TIP JAR",
  "With Tip Jar Menu ON, the main menu gets a command (Tip Jar Label) that",
  `opens the tip sheet (${DEFAULT_TIP_AMOUNT}, the first enabled token).`,
  "",
  "SCRIPT API (window.SCFG)",
  "  SCFG.owns(\"chapter2\")   true once bought (instant, offline)",
  "  SCFG.pay(\"unlock\", { item: \"chapter2\" }).then((r) => r.status).catch(...)",
  "  SCFG.pay(\"tip\", { amount: \"1.00\", token: \"USDC\" }), SCFG.restore()",
  "  SCFG.hasTipped(), SCFG.tipTotal(), SCFG.receipts(), SCFG.onResult",
  "Misused calls throw (configure, addItem) or reject (pay, restore). When",
  "payments are off or no sheet can be shown, pay and restore resolve",
  "status \"failed\" with an error code instead.",
  "An unhandled rejection stops an MZ game (its error screen): always .catch.",
  "In events use the plugin commands: a Script command can't await (a",
  "SyntaxError) and its event does not wait for a .then. Use await only in",
  "your own plugin's async functions.",
  "The logo picker lists png files; a jpg or webp logo is set from a script:",
  "SCFG.configure({ logo: \"mark.webp\", ... }). With Encrypt image files the",
  "plugin reads logo.png_ as MZ does. Keep this file named SCFG.js: MZ finds",
  "its parameters by the file name.",
  "",
  "ADVANCED PARAMETERS (JSON text; empty = the defaults)",
  "  Chains [\"base\", \"arbitrum\"]    Tokens [\"USDC\"]",
  "  Methods {\"manual\": {\"allow_tx_paste\": false}}",
  "  RPC URLs {\"base\": [\"https://...\"]}",
  "  Confirmations {\"base\": 12} (EVM networks; Solana's are fixed)",
  "  Previous Wallets [\"0x...\"]: payments to these are still restored",
  "  Custom Chains: your own EVM networks (USDC/USDT with 6 decimals), each",
  "  {\"<key>\": {\"name\": \"Optimism\", \"chain_id\": 10, \"rpc\": [\"https://...\"],",
  "   \"tokens\": {\"USDC\": \"0x...\"}, \"confirmations\": 10,",
  "   \"explorer_tx\": \"https://.../tx/{}\"}}, optionally native_symbol, fee_hint,",
  "  testnet (true: used only with Testnet ON) and max_log_range. With Chains",
  "  empty they are enabled; with Chains set, list their keys there.",
  "",
  "CONFIGURING FROM A SCRIPT",
  "A plugin listed below SCFG may call SCFG.configure({...}) again from its",
  "own script, before the title screen (payments start once the game has",
  "booted): it replaces the whole configuration, Items included, so add them",
  "again with SCFG.addItem(id, {...}). The Custom Chains stay. An item re-added",
  "under the Id an Items entry has keeps that entry's Owned Switch; an item",
  "only added from a script has no switch (read SCFG.owns(id) instead); an",
  "Items entry the script does not add again is reported and its switch is",
  "left as it is.",
  "With Wallet and Solana Wallet empty, the parameters configure nothing and",
  "the script calls SCFG.addChain and SCFG.configure itself.",
  "",
  "STORAGE",
  "Purchases are kept in save/scfg.rmmzsave (desktop) or the browser's",
  "storage (web), never in save slots: every slot and New Game sees them.",
  "",
  "Docs: https://jrpg.com/stablecoinforgames/docs/",
];

function annotations(tag: "param" | "arg", d: Decl): string[] {
  const lines = [`@${tag} ${d.name}`, `@text ${d.text}`, `@desc ${d.desc}`, `@type ${d.type}`];
  for (const [k, v] of d.more ?? []) lines.push(`@${k} ${v}`);
  lines.push(d.default ? `@default ${d.default}` : "@default");
  return lines;
}

/** A comment block: `/*<opening>` then ` * ` lines, then ` *\/`. */
function block(opening: string, lines: string[]): string {
  for (const line of lines) {
    if (line.includes("*/")) throw new Error(`plugin header line closes the comment: ${line}`);
  }
  return [`/*${opening}`, ...lines.map((l) => (l ? ` * ${l}` : " *")), " */"].join("\n");
}

/** The header blocks, ending with `*\/` and no trailing newline (esbuild adds one after a banner). */
export function renderHeader(): string {
  const lines = [
    "@target MZ",
    `@plugindesc ${PLUGIN_DESC}`,
    "@author Repulse Game Studio",
    "@url https://jrpg.com/stablecoinforgames/",
    "",
    "@help",
    ...HELP,
  ];
  for (const line of HELP) {
    if (line.trimStart().startsWith("@")) throw new Error(`a help line would start an annotation: ${line}`);
  }
  for (const d of PARAMS) lines.push("", ...annotations("param", d));
  for (const c of COMMANDS) {
    lines.push("", `@command ${c.name}`, `@text ${c.text}`, `@desc ${c.desc}`);
    for (const a of c.args) lines.push("", ...annotations("arg", a));
  }
  const out = [block(":", lines)];
  for (const [name, fields] of STRUCTS) {
    const body: string[] = [];
    fields.forEach((d, i) => body.push(...(i > 0 ? [""] : []), ...annotations("param", d)));
    out.push(block(`~struct~${name}:`, body));
  }
  return out.join("\n\n");
}
