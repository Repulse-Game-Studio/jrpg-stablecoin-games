/**
 * Owned switches: each Items entry's "Owned Switch" is kept equal to
 * `SCFG.owns(id)`. The plugin owns those switches and is the only one that should write them: it
 * sets them (ON or OFF, only when they differ):
 *
 * - after `DataManager.setupNewGame` (New Game, and the game Scene_Boot sets up),
 * - after `DataManager.extractSaveContents` (every Load: a save made before the purchase, or
 *   after a purchase that was later cleared),
 * - whenever the receipts change (`SCFG.onReceiptsChanged`: a payment confirmed mid-map, a
 *   restore, clearLocalData()), which also fires once when the stored receipts are first read,
 * - after each plugin command stored its status (commands.ts).
 *
 * A game event that turns an owned switch OFF is overruled at the next of these. Purchases are
 * global, not per save slot, so every slot sees them. A switch number used by two items is a
 * plugin-parameter error (params.ts). Nothing is written before the stored receipts are read, nor
 * while the plugin is disabled.
 */
import type { ParsedItem } from "./params.ts";
import { mz } from "./platform.ts";
import type { Scfg } from "./scfg.ts";

export class OwnedSwitches {
  /** [item id, switch number] of every item with an owned switch. */
  readonly pairs: readonly [string, number][];
  private readonly _scfg: Scfg;
  private readonly _warned = new Set<string>();

  constructor(scfg: Scfg, items: readonly ParsedItem[]) {
    this._scfg = scfg;
    this.pairs = items.filter((i) => i.ownedSwitch > 0).map((i) => [i.id, i.ownedSwitch]);
  }

  /** Set every owned switch to whether its item is owned (when the game objects exist). */
  sync(): void {
    const s = this._scfg;
    const g = mz();
    const switches = g.$gameSwitches;
    if (this.pairs.length === 0 || !switches || !g.$dataSystem || s._disabled !== null || !s.stateLoaded()) return;
    const count = Array.isArray(g.$dataSystem.switches) ? g.$dataSystem.switches.length : Infinity;
    for (const [id, n] of this.pairs) {
      if (n >= count) {
        if (this._firstTime(`switch:${id}`)) {
          s._warn(`item ${id}'s owned switch ${n} is not in the database (it has ${count - 1} switches); it is never set`);
        }
        continue;
      }
      let owned: boolean;
      try {
        owned = s.owns(id);
      } catch (e) {
        if (this._firstTime(`owns:${id}`)) s._reportError(`cannot set item ${id}'s owned switch:`, e);
        continue;
      }
      if (switches.value(n) !== owned) switches.setValue(n, owned);
    }
  }

  /** Alias MZ's New Game and Load steps and follow the receipts (once per game). */
  install(): void {
    if (this.pairs.length === 0) return;
    const g = mz();
    const dm = g.DataManager;
    const sync = (): void => {
      try {
        this.sync();
      } catch (e) {
        this._scfg._reportError("cannot set the owned switches:", e);
      }
    };
    const setupNewGame = dm.setupNewGame;
    dm.setupNewGame = function (this: unknown, ...args: unknown[]) {
      const r = setupNewGame.apply(this, args);
      sync();
      return r;
    };
    const extractSaveContents = dm.extractSaveContents;
    dm.extractSaveContents = function (this: unknown, ...args: unknown[]) {
      const r = extractSaveContents.apply(this, args);
      sync();
      return r;
    };
    this._scfg.onReceiptsChanged(sync);
  }

  /** True the first time `key` is asked about: each problem is reported once. */
  private _firstTime(key: string): boolean {
    if (this._warned.has(key)) return false;
    this._warned.add(key);
    return true;
  }
}
