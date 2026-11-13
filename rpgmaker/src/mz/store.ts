/**
 * The player's payment state in MZ's storage: global to the install, never inside a save
 * slot. `StorageManager.saveObject("scfg", ...)` keeps it in `save/scfg.rmmzsave` on desktop
 * (NW.js) and under the localforage key `rmmzsave.<gameId>.scfg` in a browser, beside the global
 * info and the config, as MZ stores its own. The twin of Godot's store.gd and of Ren'Py's
 * `persistent.scfg_pending` / `persistent.scfg_receipts`.
 *
 * The document is `{"version": 1, "pending": {...}, "receipts": {...}}`, the Engine's two stores
 * as they are (conformance/schema/state.json), the same records as every engine.
 *
 * `load()` follows MZ's own reading steps (`loadZip`, `zipToJson`, `jsonToObject`) so it can tell
 * apart what went wrong:
 * - nothing stored (`StorageManager.exists` is false): a first launch, empty stores;
 * - stored but not readable right now: `loadZip` rejected (on NW.js the file read threw, such as
 *   EACCES, EBUSY or EIO; on the web localforage failed). Not corrupt: `{error: "unreadable"}`, and
 *   the caller must not save over it this run;
 * - corrupt: it cannot be decompressed or parsed, or is not that document (another version, a
 *   store that is not an object), or, on NW.js, the file is empty (MZ's reader then says
 *   "Savefile not found" although the file exists). It is copied to `scfg_corrupt_<n>` (the first
 *   free n, so a later corruption never replaces an earlier copy) and removed, and the load gives
 *   empty stores with a warning that names it. `setAside(why)` does the same for a document that
 *   read fine but that the core refuses (see persist.ts). When that copy cannot be written, the
 *   document is unreadable instead: starting empty would let the next save replace it with
 *   nothing kept anywhere.
 * - on NW.js, MZ's `saveToLocalFile` renames `scfg.rmmzsave` to `scfg.rmmzsave_` before writing
 *   and unlinks the `_` file at the start of the next save. A crash in between leaves only the `_`
 *   file: it is renamed back (with MZ's `fsReadFile` / `fsRename`) before reading. A crash while MZ
 *   writes leaves a truncated state file beside the backup: the state file is kept aside and the
 *   backup read instead. A backup that cannot be read or renamed back is unreadable, so the next
 *   save never unlinks it. When both exist and the state file reads fine, it is the newer one.
 *
 * `save(doc)` is `StorageManager.saveObject`, which on NW.js writes beside the old file and
 * replaces it, and on the web is one localforage write. It rejects when the save failed.
 */
import { mz } from "./platform.ts";

export const STORE_KEY = "scfg";
export const STATE_VERSION = 1;

/** The Engine's two stores. */
export interface StateDoc {
  pending: Record<string, any>;
  receipts: Record<string, any>;
}

export type LoadResult =
  | (StateDoc & { warning: string | null })
  | { error: "unreadable"; detail: string };

const isStore = (v: unknown): v is Record<string, any> => typeof v === "object" && v !== null && !Array.isArray(v);

export const messageOf = (e: unknown): string => {
  if (e instanceof Error) {
    const code = (e as { code?: unknown }).code;
    return typeof code === "string" && !e.message.includes(code) ? `${e.message} (${code})` : e.message;
  }
  return String(e);
};

/** A JSON-only deep copy (state records are JSON data by design). */
export const plainCopy = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T;

const empty = (warning: string | null = null): LoadResult => ({ pending: {}, receipts: {}, warning });

/** What keeping a document aside did: the message for the developer, and whether a copy exists. */
export interface Aside {
  message: string;
  kept: boolean;
  /** Where the copy is (as `where()` names it), or null when none was made. */
  keptAs: string | null;
}

export class ScfgStore {
  readonly key: string;
  /** What `load()` last read, as MZ stores it: what `setAside()` keeps. */
  private _raw: unknown = undefined;

  constructor(key: string = STORE_KEY) {
    this.key = key;
  }

  private sm(): any {
    return mz().StorageManager;
  }

  /** Where a save name lives, for messages: the file on NW.js, the localforage key on the web. */
  where(name: string = this.key): string {
    const sm = this.sm();
    try {
      return sm.isLocalMode() ? String(sm.filePath(name)) : `localforage key ${sm.forageKey(name)}`;
    } catch {
      return name;
    }
  }

  load(): Promise<LoadResult> {
    return this._read(true);
  }

  /** One reading of the state; `useBackup`: a corrupt state file may be replaced by MZ's backup of it (once). */
  private async _read(useBackup: boolean): Promise<LoadResult> {
    const sm = this.sm();
    this._raw = undefined;
    let exists: boolean;
    try {
      exists = !!sm.exists(this.key);
    } catch (e) {
      return { error: "unreadable", detail: `cannot read ${this.where()}: ${messageOf(e)}` };
    }
    if (!exists) {
      let recovered: boolean;
      try {
        recovered = this._recoverBackup();
      } catch (e) {
        return { error: "unreadable", detail: `cannot move ${this.where()}_ back to ${this.where()}: ${messageOf(e)}` };
      }
      if (!recovered) return empty();
    }
    let zip: unknown;
    try {
      zip = await sm.loadZip(this.key);
    } catch (e) {
      if (e instanceof Error && /savefile not found/i.test(e.message)) {
        let still = true;
        try {
          still = !!sm.exists(this.key);
        } catch {
          // keep `still`: the file was there a moment ago
        }
        if (!still) return empty(); // removed in between
        return this._corrupt("", "is empty", useBackup);
      }
      return { error: "unreadable", detail: `cannot read ${this.where()}: ${messageOf(e)}` };
    }
    if (zip === null || zip === undefined) return empty(); // web: the key went away in between
    let json: unknown;
    try {
      json = await sm.zipToJson(zip);
    } catch {
      return this._corrupt(zip, "cannot be decompressed", useBackup);
    }
    let doc: unknown;
    try {
      doc = await sm.jsonToObject(json);
    } catch {
      return this._corrupt(zip, "is not valid JSON", useBackup);
    }
    let why = "";
    if (!isStore(doc)) why = "is not a JSON object";
    else if (doc["version"] !== STATE_VERSION) why = `has an unknown version ${JSON.stringify(doc["version"]) ?? "undefined"}`;
    else if (!(isStore(doc["pending"]) && isStore(doc["receipts"]))) why = "has no pending/receipts objects";
    if (why !== "") return this._corrupt(zip, why, useBackup);
    this._raw = zip;
    const d = doc as Record<string, any>;
    return { pending: d["pending"], receipts: d["receipts"], warning: null };
  }

  /** NW.js only: is MZ's backup of the state file (`<file>_`) there? False without MZ's `fs*` helpers. Throws when it cannot be read. */
  private _hasBackup(): boolean {
    const sm = this.sm();
    if (!sm.isLocalMode() || typeof sm.fsReadFile !== "function" || typeof sm.fsRename !== "function") return false;
    return sm.fsReadFile(String(sm.filePath(this.key)) + "_") !== null;
  }

  /** Rename MZ's backup (`<file>_`) back to the state file. Throws when it cannot. */
  private _restoreBackup(): void {
    const sm = this.sm();
    const file = String(sm.filePath(this.key));
    sm.fsRename(file + "_", file);
  }

  /**
   * NW.js only: the state file is missing but MZ's backup of it is there, left by a crash in the
   * middle of a save. Renames it back and returns true; false when there is none. Throws when it
   * cannot be read or renamed.
   */
  private _recoverBackup(): boolean {
    if (!this._hasBackup()) return false;
    this._restoreBackup();
    return !!this.sm().exists(this.key);
  }

  /**
   * The state file is corrupt (`why`): keep it aside. On NW.js, when MZ's backup of it is there (a
   * crash while MZ wrote the file leaves a truncated file beside the last good one), the backup is
   * renamed back and read instead, through the same checks; a corrupt backup is kept aside too.
   * Nothing that cannot be kept aside, and no backup that cannot be read, is ever left for a save
   * to replace or unlink: those are unreadable.
   */
  private async _corrupt(zip: unknown, why: string, useBackup: boolean): Promise<LoadResult> {
    const aside = await this._keepAside(zip, why);
    if (aside.keptAs === null) return { error: "unreadable", detail: aside.message };
    const backup = `${this.where()}_`;
    const unusable = (what: string, e: unknown): LoadResult => ({
      error: "unreadable",
      detail: `the payment state ${this.where()} ${why} (it was kept as ${aside.keptAs}), and MZ's backup of it ${backup} `
        + `${what}: ${messageOf(e)}`,
    });
    if (!useBackup) return empty(aside.message);
    try {
      if (!this._hasBackup()) return empty(aside.message);
    } catch (e) {
      return unusable("cannot be read", e);
    }
    try {
      this._restoreBackup();
    } catch (e) {
      return unusable("cannot be moved back", e);
    }
    const note = `the payment state ${this.where()} ${why} (it was kept as ${aside.keptAs}); MZ's backup of it ${backup}, `
      + "left when a save was cut short, was read instead";
    const again = await this._read(false);
    if ("error" in again) return { error: "unreadable", detail: `${note}, but ${again.detail}` };
    return { ...again, warning: again.warning === null ? note : `${note}, but ${again.warning}` };
  }

  /**
   * The document `load()` just read is unusable after all (`why`): keep it aside as for a corrupt
   * one. Returns the message naming where it was kept, and whether a copy was made (when not,
   * the caller must treat the document as unreadable and never save over it).
   */
  async setAside(why: string): Promise<Aside> {
    const raw = this._raw;
    this._raw = undefined;
    return this._keepAside(raw, why);
  }

  private async _keepAside(zip: unknown, why: string): Promise<Aside> {
    const sm = this.sm();
    const source = this.where();
    let kept: string | null = null;
    let failure = "nothing was read";
    if (zip !== undefined) {
      try {
        let n = 1;
        while (sm.exists(`${this.key}_corrupt_${n}`)) n++;
        const aside = `${this.key}_corrupt_${n}`;
        await sm.saveZip(aside, zip);
        kept = aside;
        // remove() is synchronous on NW.js and a Promise on the web
        await Promise.resolve().then(() => sm.remove(this.key));
      } catch (e) {
        // a copy that was made is kept; the next save replaces the original
        failure = messageOf(e);
      }
    }
    if (kept === null) {
      return { kept: false, keptAs: null, message: `the payment state ${source} ${why}, and it could not be kept aside (${failure})` };
    }
    return {
      kept: true, keptAs: this.where(kept),
      message: `the payment state ${source} ${why}; starting with no receipts or pending payments (it was kept as `
        + `${this.where(kept)}). Players can bring purchases back with Restore.`,
    };
  }

  async save(doc: StateDoc): Promise<void> {
    this._raw = undefined;
    // a copy: MZ's JsonEx.stringify tags the objects it is given in place
    await this.sm().saveObject(this.key, {
      version: STATE_VERSION, pending: plainCopy(doc.pending), receipts: plainCopy(doc.receipts),
    });
  }
}
