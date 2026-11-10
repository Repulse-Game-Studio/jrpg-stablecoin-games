/**
 * Entry of the shim's browser build (`dist/test/shim.js`, an IIFE): installs the shim on `window`
 * and registers the parameters of the page's `$plugins` (js/plugins.js), as MZ's
 * `PluginManager.setup` does before the plugin scripts run. The page then loads SCFG.js itself.
 * The control handle is `window.mzShim`, for test drivers.
 *
 * Saves (TEST ONLY): MZ keeps a browser game's saves in localforage (IndexedDB), which outlives
 * a reload of the tab. The shim's StorageManager needs a synchronous backend, so in a page its
 * saves go to the page's `localStorage` under PAGE_PREFIX + the localforage key
 * (`mzshim:rmmzsave.<gameId>.<name>`): a reload, or a second tab of the same origin, sees what the
 * game stored, as with MZ. Each Playwright browser context starts with an empty localStorage, so
 * tests stay independent. Where localStorage cannot be used (blocked site data) the saves stay in
 * memory for the page's life.
 *
 * MZ's index.html also loads js/libs/localforage.min.js, whose `localforage` the plugin asks in a
 * web test play whether the browser storage works (`localforage.ready()`); the shim keeps saves in
 * its own storage backend, so a page without a real localforage gets one that has started.
 */
import { installShim, MemoryStorage, type StorageBackend } from "./index.ts";

/** Where the shim's saves sit in the page's localStorage. */
export const PAGE_PREFIX = "mzshim:";

/** A StorageBackend on `localStorage`, every key under PAGE_PREFIX. Not part of MZ. */
export class PageStorage implements StorageBackend {
  readonly store: Storage;

  constructor(store: Storage) {
    this.store = store;
  }

  keys(): string[] {
    const out: string[] = [];
    for (let i = 0; i < this.store.length; i++) {
      const k = this.store.key(i);
      if (k !== null && k.startsWith(PAGE_PREFIX)) out.push(k.slice(PAGE_PREFIX.length));
    }
    return out.sort();
  }

  has(key: string): boolean {
    return this.store.getItem(PAGE_PREFIX + key) !== null;
  }

  read(key: string): string | null {
    return this.store.getItem(PAGE_PREFIX + key);
  }

  write(key: string, text: string): void {
    this.store.setItem(PAGE_PREFIX + key, text);
  }

  remove(key: string): void {
    this.store.removeItem(PAGE_PREFIX + key);
  }
}

/** The page's localStorage when it works here, else null. */
function pageStore(w: any): Storage | null {
  try {
    const s: Storage | undefined = w.localStorage;
    if (!s) return null;
    s.setItem(PAGE_PREFIX + "probe", "1");
    s.removeItem(PAGE_PREFIX + "probe");
    return s;
  } catch {
    return null;
  }
}

const w = globalThis as any;
const store = pageStore(w);
w.mzShim = installShim(w, {
  plugins: Array.isArray(w.$plugins) ? w.$plugins : [],
  storage: store !== null ? new PageStorage(store) : new MemoryStorage(),
});
w.localforage ??= { ready: () => Promise.resolve() };
