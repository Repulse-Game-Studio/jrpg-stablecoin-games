/**
 * Every player-facing text of the RPG Maker plugin, by id, in English, and how a game replaces
 * them. The twin of Godot's `tr()` catalogue (godot/addons/scfg/translations/scfg.pot): the same
 * strings, here keyed by id so a game can translate them without touching the plugin.
 *
 * ID SCHEME (snake_case: `^[a-z][a-z0-9_]*$`; a test pins it):
 *   - checkout messages keep the core's own keys, the ones in conformance/vectors/messages.json
 *     (`invalid_hash`, `tx_reverted`, ...; what `CheckoutController.message()` returns);
 *   - the restore screen's messages (`RESTORE_MESSAGES`) take the prefix `restore_`
 *     (`restore_tx_reverted`), since some keys read differently there;
 *   - the flows' notifications: `payment_confirmed_item`, `payment_confirmed`, `purchases_restored`,
 *     `copy_failed`;
 *   - everything the sheet shows is named after what it says or where it sits (`cancel`,
 *     `how_to_pay`, `qr_step_scan`, `evm_extension_sub`, ...).
 *   Only `%s` takes arguments (in order; `%%` is a percent sign), also for numbers.
 *
 * OVERRIDE ORDER, strongest first: `SCFG.texts[id]` (script), the "Texts" plugin parameter, the
 * built-in English below. An id nobody defines shows as itself; in test play it throws, since it
 * is a typo of the developer's. `tools/check_text_ids.py` checks these ids against the shared
 * vectors, the Godot template and the ids the sheet code asks for.
 */
import { CHECKOUT_MESSAGES, RESTORE_MESSAGES } from "../core/index.ts";

/** The flows' notifications (Ren'Py's renpy.notify strings). */
const NOTICES: Record<string, string> = {
  payment_confirmed_item: "Payment confirmed: %s. Thank you!",
  payment_confirmed: "Payment confirmed. Thank you for your support!",
  purchases_restored: "Purchases restored.",
  copy_failed: "Couldn't copy: your browser blocked the clipboard.",
};

/** The sheet's own strings (Ren'Py's scfg_screens.rpy, as Godot's tr() calls). */
const SHEET: Record<string, string> = {
  // amount and item
  choose_amount: "Choose an amount",
  or_enter_your_own: "Or enter your own",
  amount: "Amount",
  tip: "Tip",
  summary_line: "%s: %s %s on %s",
  // choosing how to pay
  how_to_pay: "How do you want to pay?",
  which_network: "Which network holds your %s?",
  fees_lowest: "lowest fees",
  fees_low: "low fees",
  fees_high: "high fees",
  phone_wallet: "Phone wallet",
  scan_with_phone_wallet: "Scan a code with your phone wallet",
  browser_wallet: "Browser wallet",
  manual_transfer: "Manual transfer",
  send_from_exchange: "Send from an exchange or any wallet",
  evm_extension_sub: "MetaMask, Rabby or another wallet extension",
  evm_browser_sub: "MetaMask, Rabby or another wallet in this browser",
  solana_extension_sub: "Phantom, Solflare or another wallet extension",
  solana_browser_sub: "Phantom, Solflare or another wallet in this browser",
  evm_opens_browser_note: "Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions.",
  solana_opens_browser_note: "Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions.",
  evm_opens_here_note: "Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions.",
  solana_opens_here_note: "Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions.",
  // paying
  preparing_payment: "Preparing the payment...",
  qr_step_open: "1. Open your wallet app",
  qr_step_scan: "2. Scan the code",
  qr_step_approve: "3. Approve %s",
  open_wallet_app: "Open wallet app",
  open_wallet: "Open wallet",
  approve_in_wallet: "Approve %s on %s in your wallet.",
  wallet_will_ask: "Your wallet will ask for %s. The last digits identify your payment and add less than 1 cent.",
  wallet_changes_amount: "If your wallet changes the amount, the game still finds the payment.",
  wallet_changes_amount_manual: "If your wallet changes the amount, use Manual transfer and paste the transaction hash.",
  open_checkout_browser: "Open checkout in browser",
  reopen_checkout_page: "Reopen checkout page",
  reopen_wallet_window: "Reopen wallet window",
  finish_in_browser: "Finish the payment on the checkout page in your browser.",
  finish_in_wallet_window: "Finish the payment in the wallet window.",
  to_address: "To address",
  token_contract: "Token contract",
  token_mint: "Token mint",
  send_exact_amount: "Send exactly this amount on %s only. Sending on another network may lose your funds.",
  send_exact_amount_solana: "Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds.",
  copy: "Copy",
  copy_hash: "Copy hash",
  copy_signature: "Copy signature",
  paste: "Paste",
  i_sent_it: "I've sent it",
  sent_from_exchange_paste: "Sent from an exchange? Paste the %s:",
  paste_hash_help: "Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history.",
  your_hash: "Your %s:",
  word_tx_hash: "transaction hash",
  word_signature: "signature",
  check: "Check",
  // waiting and result
  waiting_for_payment: "Waiting for your payment on %s...",
  tx_received_waiting: "Transaction received. Waiting for it to be included on %s...",
  confirming: "Confirming on %s... %s/%s",
  checking_blockchain: "Checking the blockchain...",
  already_paid_note: "If you already paid, the game will keep checking for 24 hours and unlock it automatically.",
  keep_playing_note: "You can keep playing: the game keeps checking in the background and finishes the purchase by itself.",
  continue_playing: "Continue playing",
  keep_as_tip: "Keep as a tip",
  paid_title: "Payment confirmed. Thank you!",
  payment_too_small: "Payment too small",
  underpaid_note: "The amount received was less than the price. Please contact the developer.",
  payment_window_closed: "The payment window has closed",
  payment_screen_inactive: "This payment screen is no longer active. Any payment you made is still being checked in the background.",
  view_on_explorer: "View on explorer",
  // restore
  restore: "Restore",
  restore_purchases: "Restore purchases",
  restored: "Restored:",
  purchases_on_device: "Purchases on this device",
  keep_this_to_restore: "Keep this %s to restore your purchase on another device:",
  nothing_to_restore: "Nothing new to restore. These purchases are already on this device.",
  unmatched_payment_prompt: "Payment of %s %s on %s. What was it for?",
  screen_inactive: "This screen is no longer active.",
  saving: "Saving...",
  set: "Set",
  // developer lines (test play only)
  dev_mode_paying_to: "DEV MODE: paying to %s",
  dev_mode_paying_to_testnet: "DEV MODE: paying to %s (testnet)",
  dev_solana_cant_receive: "Your Solana wallet can't receive %s yet: receive any amount once.",
  // accessibility
  qr_code: "QR code",
  // navigation
  back: "Back",
  next: "Next",
  continue: "Continue",
  cancel: "Cancel",
  close: "Close",
};

function build(): Readonly<Record<string, string>> {
  const out: Record<string, string> = Object.create(null);
  const add = (id: string, english: string): void => {
    if (id in out) throw new Error(`scfg: duplicate text id ${id}`);
    out[id] = english;
  };
  for (const [id, english] of Object.entries(SHEET)) add(id, english);
  for (const [id, english] of Object.entries(NOTICES)) add(id, english);
  for (const [id, english] of Object.entries(CHECKOUT_MESSAGES)) add(id, english);
  for (const [id, english] of Object.entries(RESTORE_MESSAGES)) add("restore_" + id, english);
  return Object.freeze(out);
}

/** The built-in English by id (see the id scheme above). */
export const DEFAULT_TEXTS: Readonly<Record<string, string>> = build();

/** The two override layers a game can supply. */
export interface TextLayers {
  /** `SCFG.texts` */
  script?: Record<string, string> | null;
  /** The "Texts" plugin parameter */
  params?: Record<string, string> | null;
}

/** Python's `text % args` for `%s` and `%%` only. */
function format(text: string, args: unknown[]): string {
  let i = 0;
  return text.replace(/%[%s]/g, (m) => (m === "%%" ? "%" : i < args.length ? String(args[i++]) : m));
}

const own = (o: Record<string, string> | null | undefined, id: string): string | undefined =>
  o && Object.prototype.hasOwnProperty.call(o, id) && typeof o[id] === "string" ? o[id] : undefined;

/**
 * The text `id` for the player: script override, else the parameter's, else the built-in English,
 * with `args` in the `%s`s. An unknown id is shown as itself, or throws when `strict` (test play).
 */
export function resolveText(layers: TextLayers, id: string, args: unknown[], strict: boolean): string {
  const found = own(layers.script, id) ?? own(layers.params, id) ?? own(DEFAULT_TEXTS, id);
  if (found === undefined) {
    if (strict) throw new Error(`scfg: unknown text id ${JSON.stringify(id)}`);
    return id;
  }
  return format(found, args);
}

/**
 * `text(id, ...args)`: the text through the game's `SCFG` (its overrides apply), else the built-in
 * English. Code that has its `Scfg` calls `scfg.text` directly.
 */
export function text(id: string, ...args: unknown[]): string {
  const scfg = (globalThis as any).SCFG;
  return scfg && typeof scfg.text === "function" ? scfg.text(id, ...args) : resolveText({}, id, args, false);
}
