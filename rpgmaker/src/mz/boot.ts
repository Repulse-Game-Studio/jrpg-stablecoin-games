/**
 * The plugin's boot, run once when MZ's Plugin Manager loads SCFG.js (main.ts calls it).
 *
 * At that moment MZ has registered the plugin parameters but loaded nothing else: `$dataSystem`
 * is null and StorageManager's save keys are not read (on the web `StorageManager.forageKey`
 * would throw). So boot only does what needs neither: it parses the parameters, adds the custom
 * chains, configures and adds the items (pure; a mistake is reported with
 * `console.error("scfg: ...")`, shown over the game in test play, and leaves the plugin disabled,
 * never throwing), puts the one `Scfg` at `window.SCFG`, and installs the hooks.
 * A plugin listed below SCFG runs its script after this, still before the title screen: it may
 * call `SCFG.configure(...)` again, which replaces the configuration and so its Items
 * (it re-adds them with `SCFG.addItem`; the Items parameter's Owned Switches follow the items it
 * re-adds under the same ids). The hooks:
 *
 * - `Scene_Boot.prototype.onDatabaseLoaded` (MZ calls it once the database and the save keys are
 *   loaded) starts reading the stored payment state, and `Scene_Boot.prototype.isReady` waits
 *   for it, as MZ waits for its own global info and config. The title screen and every script
 *   after it then see the player's purchases (`SCFG.owns`).
 * - `SceneManager.updateMain` runs the pump after each frame, then the sheet host's `update()`
 *   (the screens read MZ's input and redraw). The pump also starts the read if a replaced
 *   Scene_Boot never called onDatabaseLoaded (once `$dataSystem` and the save keys are there).
 * - The plugin commands, the owned switches and the Tip Jar menu entry (`installGameHooks`).
 * - Closing (Ren'Py's quit callback `_shutdown`): `SceneManager.terminate` (MZ's exit), the
 *   page's `pagehide` and, on NW.js, its `beforeunload` (actions.ts `watchClosing`) shut the flows
 *   down and flush the save first; a hidden page (`visibilitychange`) and a browser's
 *   `beforeunload` only flush. No NW.js window "close" listener is registered: it
 *   would take the closing over from other plugins' quit prompts.
 */
import { PLUGIN_NAME } from "./plugin_header.ts";
import { parseParams, type ParsedParams } from "./params.ts";
import { watchClosing } from "./actions.ts";
import { installCommands } from "./commands.ts";
import { installTipJarMenu } from "./menu.ts";
import { mz } from "./platform.ts";
import { Scfg, type ScfgHooks } from "./scfg.ts";
import { OwnedSwitches } from "./switches.ts";

const messageOf = (e: unknown): string => (e instanceof Error ? e.message : String(e));

/** Report a boot problem and leave the plugin disabled. */
function bootFailed(scfg: Scfg, e: unknown): void {
  const message = messageOf(e);
  scfg._disable(message);
  scfg._alert(`the plugin parameters are invalid, payments are off: ${message}`);
}

/**
 * Configure `scfg` from the parsed parameters (an error leaves it disabled): the Custom Chains
 * first (`SCFG.addChain`, which must come before configure; they also stay for a script's own
 * configure), then configure and the Items.
 */
export function applyParams(scfg: Scfg, params: ParsedParams): void {
  scfg.notify = params.notify;
  scfg._paramTexts = params.texts;
  for (const [key, opts] of params.customChains) scfg.addChain(key, opts);
  if (params.configure === null) return; // configured from a script, or not at all
  scfg.configure(params.configure);
  for (const item of params.items) {
    scfg.addItem(item.id, { code: item.code, price: item.price, token: item.token, title: item.title, min_price: item.min_price });
  }
}

/** Alias MZ's methods (see the header). Once per game: each call wraps them again. */
export function installHooks(scfg: Scfg): void {
  const g = mz();
  const boot = g.Scene_Boot?.prototype;
  if (boot && typeof boot.onDatabaseLoaded === "function" && typeof boot.isReady === "function") {
    const onDatabaseLoaded = boot.onDatabaseLoaded;
    boot.onDatabaseLoaded = function (this: unknown, ...args: unknown[]) {
      const r = onDatabaseLoaded.apply(this, args);
      scfg._maybeLoad();
      return r;
    };
    const isReady = boot.isReady;
    boot.isReady = function (this: unknown, ...args: unknown[]) {
      const ready = isReady.apply(this, args); // first: it is what calls onDatabaseLoaded
      return ready && scfg.stateLoaded();
    };
  }
  const sm = g.SceneManager;
  const updateMain = sm.updateMain;
  sm.updateMain = function (this: unknown, ...args: unknown[]) {
    const r = updateMain.apply(this, args);
    try {
      scfg.pump();
    } catch (e) {
      scfg._reportError("error in the payment pump:", e);
    }
    scfg._updateScreens();
    return r;
  };
  // MZ's exit (nw.App.quit on NW.js, which sends no close event): after the shutdown and its
  // flush. SceneManager calls terminate on every frame while it exits; it runs once.
  if (typeof sm.terminate === "function") {
    const terminate = sm.terminate;
    let quitting = false;
    sm.terminate = function (this: unknown, ...args: unknown[]) {
      if (quitting) return undefined;
      quitting = true;
      // (a throw there must not become an unhandled rejection: MZ's onReject would show its error screen)
      void scfg.closeForQuit().then(() => terminate.apply(this, args))
        .catch((e: unknown) => scfg._reportError("error while closing the game:", e));
      return undefined;
    };
  }
  watchClosing(scfg._platform, { quit: () => scfg.closeForQuit(), flush: () => scfg._flush() });
}

/**
 * The game-side features of the parameters: the owned switches (switches.ts), the plugin commands
 * (commands.ts; registered even when the parameters are invalid, so an event's command stores
 * "failed" instead of doing nothing) and the Tip Jar menu entry (menu.ts). Each is hooked on its
 * own: one MZ class another plugin replaced does not take the others down.
 */
export function installGameHooks(scfg: Scfg, params: ParsedParams | null): void {
  const hook = (what: string, install: () => void): void => {
    try {
      install();
    } catch (e) {
      scfg._alert(`cannot add the ${what} to the game: ${messageOf(e)}`);
    }
  };
  const switches = new OwnedSwitches(scfg, params?.items ?? []);
  hook("owned switches", () => switches.install());
  hook("plugin commands", () => installCommands(scfg, () => switches.sync()));
  if (params?.tipJarMenu) hook("Tip Jar menu entry", () => installTipJarMenu(scfg, params.tipJarLabel));
}

/** The plugin's boot: `window.SCFG`, configured from the plugin parameters, with the hooks installed. Never throws. */
export function bootPlugin(hooks: ScfgHooks = {}): Scfg {
  const g = mz();
  const scfg = new Scfg(hooks);
  g.SCFG = scfg;
  let params: ParsedParams | null = null;
  try {
    params = parseParams(g.PluginManager.parameters(PLUGIN_NAME));
    applyParams(scfg, params);
  } catch (e) {
    bootFailed(scfg, e);
  }
  try {
    installHooks(scfg);
  } catch (e) {
    scfg._alert(`cannot hook into the game, payments are off: ${messageOf(e)}`);
  }
  installGameHooks(scfg, params);
  return scfg;
}
