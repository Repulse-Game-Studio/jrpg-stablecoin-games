/**
 * The glue's mirror of the payment state and when it reaches storage: Ren'Py's
 * `_sync_persistent` and Godot's `_sync` / `_save`, around an `ScfgStore`.
 *
 * - `load()` reads the store once (the glue calls it when MZ's database and save keys are ready,
 *   see boot.ts). A corrupt document was already kept aside by the store; a document that reads
 *   fine but that the core still refuses at start (`new Worker()` throws; not expected, since
 *   `computeOwnership` skips malformed entries, so this only guards an unforeseen throw) is kept
 *   aside the same way. Either is reported, and the state starts empty. A document that exists
 *   but cannot be read right now, or one that cannot be kept aside, sets `storeError`: nothing is
 *   ever saved this run (payments do not start; the next launch reads it again).
 * - `sync(snap)` mirrors a Worker snapshot. New or changed receipts, a session added or removed,
 *   and a session's `state` or `hint_tx` changing are written at once (closing the window or the
 *   tab can end the game any moment); other pending changes such as scan progress (`cursor`) at
 *   most every PENDING_SAVE_INTERVAL seconds. `flushIfDue()`, every frame, writes what is still
 *   not stored (scan progress, a save that failed) once the interval has passed.
 * - Saves never overlap: one is in flight at a time, and a save asked for meanwhile writes the
 *   latest state when it is done.
 * - `replace(doc)` (clearLocalData) before the state is read waits for the read: the read state
 *   is then replaced and saved, so memory and storage agree and the old receipts never come back
 *   (nothing is written before the read; over a state that cannot be read, nothing at all).
 */
import { computeOwnership } from "../core/index.ts";
import { messageOf, plainCopy, type Aside, type LoadResult, type ScfgStore, type StateDoc } from "./store.ts";

export const PENDING_SAVE_INTERVAL = 10.0;

export interface MirrorHooks {
  /** Seconds, monotonic (the save interval). */
  clock: () => number;
  /** An error for the developer (console.error). */
  reportError(heading: string, e?: unknown): void;
  /** A setup or storage problem the developer must see: console.error, and on screen in test play. */
  alert(message: string): void;
  /** The receipts changed, or were read for the first time. */
  receiptsChanged(): void;
}

/** Deep equality of JSON values (object key order does not matter, as Python's dict ==). */
export function sameJson(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (Array.isArray(a)) {
    const bb = b as unknown[];
    return a.length === bb.length && a.every((v, i) => sameJson(v, bb[i]));
  }
  const ka = Object.keys(a);
  const ob = b as Record<string, unknown>;
  if (ka.length !== Object.keys(ob).length) return false;
  return ka.every((k) => Object.prototype.hasOwnProperty.call(ob, k) && sameJson((a as Record<string, unknown>)[k], ob[k]));
}

/** What must reach storage at once: which sessions exist, their state and tx hint. */
export function pendingKey(pending: Record<string, any>): Record<string, unknown> {
  const key: Record<string, unknown> = {};
  for (const [sid, r] of Object.entries(pending)) {
    const v = typeof r === "object" && r !== null && !Array.isArray(r) ? [r["state"] ?? null, r["hint_tx"] ?? null] : r;
    Object.defineProperty(key, sid, { value: v, enumerable: true, writable: true, configurable: true });
  }
  return key;
}

/** The keep-aside reason for a state the core refused with `e`. */
const unusable = (e: unknown): string => `cannot be used (${e instanceof Error ? `${e.name}: ${e.message}` : String(e)})`;

/** Why the core cannot start from these stores, or null. */
function refusal(doc: StateDoc): string | null {
  try {
    computeOwnership(doc.receipts, new Map()); // what Engine.snapshot() runs; items do not change whether it throws
    return null;
  } catch (e) {
    return unusable(e);
  }
}

const emptyDoc = (): StateDoc => ({ pending: {}, receipts: {} });

export class StateMirror {
  readonly store: ScfgStore;
  /** The mirrored state; null until `load()` finished. */
  doc: StateDoc | null = null;
  /** Set when the stored state exists but could not be read: never saved over this run. */
  storeError: string | null = null;
  /** `doc` has changes that are not stored yet. */
  dirty = false;
  /** When the last save was started (`clock` seconds). */
  lastSave = -Infinity;

  private readonly hooks: MirrorHooks;
  private _loading: Promise<void> | null = null;
  private _saving: Promise<void> | null = null;
  private _saveQueued = false;
  private _aside: Promise<void> | null = null;
  private _gen = 0; // counts changes of `doc`
  /** replace() asked for before the state was read: applied once it is. */
  private _replaceOnLoad: StateDoc | null = null;

  constructor(store: ScfgStore, hooks: MirrorHooks) {
    this.store = store;
    this.hooks = hooks;
  }

  /** The state has been read (and nothing is being kept aside). */
  get loaded(): boolean {
    return this.doc !== null && this._aside === null;
  }

  get loadStarted(): boolean {
    return this._loading !== null;
  }

  /** Read the stored state, once; later calls return the same promise. Never rejects. */
  load(): Promise<void> {
    if (this._loading === null) this._loading = this._load();
    return this._loading;
  }

  private async _load(): Promise<void> {
    let got: LoadResult;
    try {
      got = await this.store.load();
    } catch (e) {
      got = { error: "unreadable", detail: `cannot read ${this.store.where()}: ${messageOf(e)}` };
    }
    if ("error" in got) {
      this._unreadable(got.detail);
      this.doc = emptyDoc();
    } else {
      if (got.warning !== null) this.hooks.alert(got.warning);
      let doc: StateDoc;
      let why: string | null;
      try {
        doc = { pending: plainCopy(got.pending), receipts: plainCopy(got.receipts) };
        why = refusal(doc);
      } catch (e) {
        doc = emptyDoc();
        why = `cannot be copied (${messageOf(e)})`;
      }
      if (why !== null) {
        doc = emptyDoc();
        this._asideDone(await this.store.setAside(why));
      }
      this.doc = doc;
    }
    const wanted = this._replaceOnLoad;
    this._replaceOnLoad = null;
    if (wanted !== null && this.storeError === null) this.replace(wanted); // (an unreadable state: never saved over)
    this.hooks.receiptsChanged();
  }

  /** Report a keep-aside; one that made no copy leaves the state unreadable for this run. */
  private _asideDone(aside: Aside): void {
    if (aside.kept) this.hooks.alert(aside.message);
    else this._unreadable(aside.message);
  }

  /** The stored state exists but cannot be read (`detail`): reported, and never saved over this run. */
  private _unreadable(detail: string): void {
    this.storeError = detail;
    this.hooks.alert(`cannot read the stored receipts: ${detail}. Payments are off until the game is started again; `
      + "nothing is saved over it.");
  }

  /**
   * The core refused the loaded state when the Engine was built from it (`error`): keep it aside
   * as corrupt and start empty. `loaded` is false until that is done. When no copy can be made,
   * `storeError` is set instead (nothing is saved this run).
   */
  setAside(error: unknown): Promise<void> {
    if (this._aside === null) {
      const had = this.doc !== null && Object.keys(this.doc.receipts).length > 0;
      this._aside = (async () => {
        try {
          this._asideDone(await this.store.setAside(unusable(error)));
        } finally {
          this.doc = emptyDoc();
          this._gen++;
          this._aside = null;
          if (had) this.hooks.receiptsChanged();
          if (this.dirty) this.save();
        }
      })();
    }
    return this._aside;
  }

  /** Mirror a Worker snapshot (see the header). */
  sync(snap: StateDoc): void {
    if (this.doc === null) return;
    const receiptsChanged = !sameJson(this.doc.receipts, snap.receipts);
    const pendingChanged = !sameJson(this.doc.pending, snap.pending);
    if (!(receiptsChanged || pendingChanged)) return;
    const sessionsChanged = !sameJson(pendingKey(this.doc.pending), pendingKey(snap.pending));
    // the snapshot's stores are detached copies that nobody changes
    this.doc = { pending: snap.pending, receipts: snap.receipts };
    this._gen++;
    this.dirty = true;
    if (receiptsChanged || sessionsChanged || this.hooks.clock() - this.lastSave >= PENDING_SAVE_INTERVAL) this.save();
    if (receiptsChanged) this.hooks.receiptsChanged();
  }

  /** Replace the state and save it at once (clearLocalData); before the state is read, once it is. */
  replace(doc: StateDoc): void {
    if (this.doc === null) {
      this._replaceOnLoad = doc;
      return;
    }
    this.doc = doc;
    this._gen++;
    this.dirty = true;
    this.save();
  }

  /** Every frame: what is not stored yet (scan progress, a failed save) is written once the interval passed. */
  flushIfDue(): void {
    if (this.dirty && this.hooks.clock() - this.lastSave >= PENDING_SAVE_INTERVAL) this.save();
  }

  /** Write `doc` (one save at a time; see the header). Never over a state that could not be read. */
  save(): void {
    if (this.storeError !== null || this.doc === null || this._aside !== null) return;
    if (this._saving !== null) {
      this._saveQueued = true;
      return;
    }
    this.lastSave = this.hooks.clock();
    const gen = this._gen;
    this._saving = this.store.save(this.doc).then(
      () => {
        if (gen === this._gen) this.dirty = false;
      },
      (e) => this.hooks.reportError("cannot save the payment state:", e),
    ).then(() => {
      this._saving = null;
      if (this._saveQueued) {
        this._saveQueued = false;
        this.save();
      }
    });
  }

  /** Resolves once nothing is being loaded, kept aside or saved (tests; the flush at quit). */
  async idle(): Promise<void> {
    if (this._loading !== null) await this._loading;
    for (let busy = this._aside ?? this._saving; busy !== null; busy = this._aside ?? this._saving) await busy;
  }
}
