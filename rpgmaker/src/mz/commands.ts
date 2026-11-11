/**
 * The plugin commands (registered under "SCFG", declared in plugin_header.ts) and the event
 * interpreter's "scfg" wait mode.
 *
 * Unlock Item, Tip and Restore Purchases run `SCFG.pay()` / `SCFG.restore()` for the event that
 * called them. Each sets its Status Variable to 0, puts the calling Game_Interpreter (`this` of a
 * plugin command in MZ: the map's event, a parallel event's or a common event's own
 * interpreter) in the "scfg" wait mode until its Promise settles, then stores the status code
 * (`SCFG.STATUS`) and refreshes the owned switches. A command that cannot run (a misused call,
 * which rejects; payments off or no screen, which resolve a "failed" result) stores `failed`, and
 * why is reported with console.error. Nothing here leaves a rejection unhandled (MZ's
 * SceneManager.onReject would stop the game).
 * Wallet Help opens the players' wallet guide outside the game and does not wait.
 *
 * Each run belongs to its interpreter, in a WeakMap of this session (nothing is added to the
 * interpreter, which MZ writes into save files): a newer command on the same interpreter makes
 * the older run moot, and a result is stored only into the game it started in (the same
 * `$gameVariables`; a Load or New Game replaces it).
 *
 * SAVE AND LOAD WHILE WAITING. A Promise cannot be saved. A save made while an event waits holds
 * the variable at 0 and the interpreter in the "scfg" wait mode; after that save is loaded, the
 * interpreter has no live run, so it stops waiting on its next update and the event goes on with
 * the variable at 0 ("not done"). The payment itself is not lost: receipts are global, not per
 * save slot, and the owned switches are set after every load (switches.ts).
 */
import { DEFAULT_TIP_AMOUNT } from "./params.ts";
import { ARG, COMMAND, PLUGIN_NAME, WALLET_HELP_URL } from "./plugin_header.ts";
import { mz } from "./platform.ts";
import type { Scfg } from "./scfg.ts";

export { DEFAULT_TIP_AMOUNT, WALLET_HELP_URL };

/**
 * The status codes a command stores in its variable, by result status:
 * `SCFG.STATUS.paid === 1`. Restore Purchases stores restored (1), nothing_found (2), cancelled
 * (6) or failed (7). 0 (`none`): not done yet (the sheet is open) or the command did not finish.
 */
export const STATUS = Object.freeze({
  none: 0, paid: 1, already_owned: 2, pending: 3, underpaid: 4, expired: 5, cancelled: 6, failed: 7,
  restored: 1, nothing_found: 2,
});

/** The Game_Interpreter wait mode of a running command. */
export const WAIT_MODE = "scfg";

const PAYMENT_CODES: Readonly<Record<string, number>> = {
  paid: STATUS.paid, already_owned: STATUS.already_owned, pending: STATUS.pending, underpaid: STATUS.underpaid,
  expired: STATUS.expired, cancelled: STATUS.cancelled,
};
const RESTORE_CODES: Readonly<Record<string, number>> = {
  restored: STATUS.restored, nothing_found: STATUS.nothing_found, cancelled: STATUS.cancelled,
};

/** `status`'s code in `codes`; failed for any other status. */
const codeIn = (codes: Readonly<Record<string, number>>, status: unknown): number =>
  typeof status === "string" && Object.prototype.hasOwnProperty.call(codes, status) ? codes[status]! : STATUS.failed;

/** The code of a pay() result's status (anything else: failed). */
export function paymentCode(status: unknown): number {
  return codeIn(PAYMENT_CODES, status);
}

/** The code of a restore() result's status (anything else, such as "failed": failed). */
export function restoreCode(status: unknown): number {
  return codeIn(RESTORE_CODES, status);
}

/** An argument as MZ hands it over (a string; a missing one is ""), trimmed. */
const argText = (args: unknown, name: string): string => {
  const v = typeof args === "object" && args !== null ? (args as Record<string, unknown>)[name] : undefined;
  return typeof v === "string" ? v.trim() : v === undefined || v === null ? "" : String(v).trim();
};

/** A variable argument's number; 0 (none) for "", "0" or anything that is not a number. */
const variableOf = (args: unknown): number => {
  const v = argText(args, ARG.statusVariable);
  return /^\d+$/.test(v) ? Number(v) : 0;
};

/**
 * pay("tip", ...) options of the Tip command and the Tip Jar: the suggested amount (empty:
 * DEFAULT_TIP_AMOUNT) and the token (empty: the first enabled token).
 */
export function tipOptions(scfg: Scfg, amount = "", token = ""): { amount: string; token: string | null } {
  return { amount: amount === "" ? DEFAULT_TIP_AMOUNT : amount, token: token !== "" ? token : scfg._config?.tokens[0] ?? null };
}

interface Run {
  settled: boolean;
}

/**
 * Register the commands and alias `Game_Interpreter.prototype.updateWaitMode` (once per game).
 * `changed` runs after a code is stored (the owned-switch refresh).
 */
export function installCommands(scfg: Scfg, changed: () => void): void {
  const g = mz();
  const runs = new WeakMap<object, Run>();

  /** Start one command run for `interpreter`; its code goes to the status variable once `launch` settles. */
  const start = (interpreter: unknown, label: string, args: unknown, launch: () => Promise<number>): void => {
    const variableId = variableOf(args);
    const variables = g.$gameVariables;
    const store = (code: number): void => {
      if (variableId > 0 && variables && g.$gameVariables === variables) variables.setValue(variableId, code);
    };
    store(STATUS.none);
    const run: Run = { settled: false };
    const owner = typeof interpreter === "object" && interpreter !== null ? interpreter : null;
    if (owner !== null) runs.set(owner, run);
    if (owner !== null && typeof (owner as any).setWaitMode === "function") (owner as any).setWaitMode(WAIT_MODE);
    let promise: Promise<number>;
    try {
      promise = launch();
    } catch (e) {
      promise = Promise.reject(e);
    }
    void promise
      .catch((e: unknown) => {
        scfg._reportError(`${label} failed:`, e);
        return STATUS.failed;
      })
      .then((code) => {
        run.settled = true;
        if (owner !== null && runs.get(owner) !== run) return; // a newer command of that interpreter took over
        try {
          store(code);
          changed();
        } catch (e) {
          // never an unhandled rejection: MZ's SceneManager.onReject would stop the game
          scfg._reportError("cannot store the status or refresh the owned switches:", e);
        }
      });
  };

  const pm = g.PluginManager;
  pm.registerCommand(PLUGIN_NAME, COMMAND.unlockItem, function (this: unknown, args: unknown) {
    start(this, "Unlock Item", args, async () => paymentCode((await scfg.pay("unlock", { item: argText(args, ARG.item) })).status));
  });
  pm.registerCommand(PLUGIN_NAME, COMMAND.tip, function (this: unknown, args: unknown) {
    start(this, "Tip", args, async () => {
      const opts = tipOptions(scfg, argText(args, ARG.amount), argText(args, ARG.token));
      return paymentCode((await scfg.pay("tip", opts)).status);
    });
  });
  pm.registerCommand(PLUGIN_NAME, COMMAND.restorePurchases, function (this: unknown, args: unknown) {
    start(this, "Restore Purchases", args, async () => restoreCode((await scfg.restore()).status));
  });
  pm.registerCommand(PLUGIN_NAME, COMMAND.walletHelp, function () {
    try {
      scfg._openUrl(WALLET_HELP_URL);
    } catch (e) {
      scfg._reportError("Wallet Help failed:", e);
    }
  });

  // MZ's updateWaitMode answers whether the current wait mode still holds; the "scfg" one holds
  // while this interpreter's own run is unsettled (none: a loaded save, see the header)
  const proto = g.Game_Interpreter.prototype;
  const updateWaitMode = proto.updateWaitMode;
  proto.updateWaitMode = function (this: any, ...rest: unknown[]) {
    if (this._waitMode !== WAIT_MODE) return updateWaitMode.apply(this, rest);
    const run = runs.get(this);
    if (run !== undefined && !run.settled) return true;
    runs.delete(this);
    this._waitMode = "";
    return false;
  };
}
