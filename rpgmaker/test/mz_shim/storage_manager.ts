/**
 * StorageManager (rmmz_managers.js) and JsonEx (rmmz_core.js), from MZ's documented behaviour:
 * Promise-based `saveObject`/`loadObject`, a synchronous `exists`, and `remove`, with save files
 * `save/<name>.rmmzsave` on NW.js ("local mode") and localforage keys `rmmzsave.<gameId>.<name>`
 * in a browser.
 *
 * Boot order, as MZ: on the web `exists` answers from `_forageKeys`, a cache of every localforage
 * key that is empty until `updateForageKeys()` completes (Scene_Boot.create starts it; each save and
 * remove refreshes it). `forageKey` reads `$dataSystem.advanced.gameId`, so before
 * `DataManager.loadDatabase()` has loaded the database it throws, as MZ's does.
 *
 * NW.js saves keep MZ's `fs*` helper names, on the backend with its keys as paths (see
 * saveToLocalFile for the `<file>_` backup).
 *
 * Differences from MZ, on purpose: the data is stored as JSON text (MZ deflates it with pako); the
 * backend is injected (memory or a directory, see node_storage.ts); after a save or remove the key
 * refresh completes before the returned Promise resolves (MZ starts it alongside the write;
 * unverified timing). Unverified: what MZ's `loadObject` does for a missing save (here: rejects on
 * NW.js, resolves null on the web).
 */
import type { Ctx, StorageBackend } from "./context.ts";

/** A StorageBackend in memory (the default). */
export class MemoryStorage implements StorageBackend {
  readonly data = new Map<string, string>();

  keys(): string[] {
    return [...this.data.keys()];
  }

  has(key: string): boolean {
    return this.data.has(key);
  }

  read(key: string): string | null {
    return this.data.get(key) ?? null;
  }

  write(key: string, text: string): void {
    this.data.set(key, text);
  }

  remove(key: string): void {
    this.data.delete(key);
  }
}

/**
 * JsonEx: JSON that keeps the classes of game objects. MZ tags an instance with its constructor's
 * name under "@" (MV used "@c") and refuses nesting at `maxDepth` (100) levels; so does the shim.
 * MZ's other details (its "@a" arrays) are not reproduced.
 */
export function createJsonEx(ctx: Ctx) {
  const g = ctx.g;
  const JsonEx = { maxDepth: 100 } as { maxDepth: number; stringify(o: unknown): string; parse(json: string): any };
  const encode = (value: unknown, depth: number): unknown => {
    if (depth >= JsonEx.maxDepth) throw new Error("Object too deep"); // JsonEx._encode
    if (value === null || typeof value !== "object") return value;
    if (Array.isArray(value)) return value.map((v) => encode(v, depth + 1));
    const out: Record<string, unknown> = {};
    const name = Object.getPrototypeOf(value)?.constructor?.name;
    if (name && name !== "Object") out["@"] = name;
    for (const [k, v] of Object.entries(value)) out[k] = encode(v, depth + 1);
    return out;
  };
  const decode = (value: unknown): unknown => {
    if (value === null || typeof value !== "object") return value;
    if (Array.isArray(value)) return value.map(decode);
    const obj = value as Record<string, unknown>;
    for (const k of Object.keys(obj)) obj[k] = decode(obj[k]);
    const name = obj["@"];
    if (typeof name === "string") {
      delete obj["@"];
      const cls = g[name];
      if (typeof cls === "function") Object.setPrototypeOf(obj, cls.prototype);
    }
    return obj;
  };
  // JsonEx.stringify(object)
  JsonEx.stringify = (object) => JSON.stringify(encode(object, 0));
  // JsonEx.parse(json)
  JsonEx.parse = (json) => decode(JSON.parse(json));
  return JsonEx;
}

export function createStorageManager(ctx: Ctx) {
  const g = ctx.g;
  const later = <T>(fn: () => T): Promise<T> => new Promise((resolve, reject) => {
    try {
      resolve(fn());
    } catch (e) {
      reject(e);
    }
  });
  return {
    _forageKeys: [] as string[],
    _forageKeysUpdated: false,

    // StorageManager.isLocalMode(): NW.js keeps saves in files.
    isLocalMode(): boolean {
      return g.Utils.isNwjs();
    },
    // StorageManager.saveObject(saveName, object) -> Promise
    saveObject(saveName: string, object: unknown): Promise<void> {
      return this.objectToJson(object).then((json) => this.jsonToZip(json)).then((zip) => this.saveZip(saveName, zip));
    },
    // StorageManager.loadObject(saveName) -> Promise of the object
    loadObject(saveName: string): Promise<any> {
      return this.loadZip(saveName).then((zip) => this.zipToJson(zip)).then((json) => this.jsonToObject(json));
    },
    // StorageManager.objectToJson(object)
    objectToJson(object: unknown): Promise<string> {
      return later(() => g.JsonEx.stringify(object));
    },
    // StorageManager.jsonToObject(json)
    jsonToObject(json: string): Promise<any> {
      return later(() => g.JsonEx.parse(json));
    },
    // StorageManager.jsonToZip(json): MZ deflates; the shim keeps the text.
    jsonToZip(json: string): Promise<string> {
      return Promise.resolve(json);
    },
    // StorageManager.zipToJson(zip): a missing web save reads as "null".
    zipToJson(zip: string | null): Promise<string> {
      return Promise.resolve(zip === null ? "null" : zip);
    },
    // StorageManager.saveZip(saveName, zip)
    saveZip(saveName: string, zip: string): Promise<void> {
      return this.isLocalMode() ? this.saveToLocalFile(saveName, zip) : this.saveToForage(saveName, zip);
    },
    // StorageManager.loadZip(saveName)
    loadZip(saveName: string): Promise<string | null> {
      return this.isLocalMode() ? this.loadFromLocalFile(saveName) : this.loadFromForage(saveName);
    },
    // StorageManager.exists(saveName): synchronous
    exists(saveName: string): boolean {
      return this.isLocalMode() ? this.localFileExists(saveName) : this.forageExists(saveName);
    },
    // StorageManager.remove(saveName): synchronous on NW.js (returns nothing), a Promise on the web
    remove(saveName: string): Promise<void> | undefined {
      return this.isLocalMode() ? this.removeLocalFile(saveName) : this.removeForage(saveName);
    },
    // StorageManager.saveToLocalFile(saveName, zip). Modelled (the shim's code, not MZ's): the
    // current file becomes "<file>_" while the new one is written, then "<file>_" is dropped; a
    // failed write restores it and rejects. `shim.crashNextSave()` stops right after the move.
    saveToLocalFile(saveName: string, zip: string): Promise<void> {
      const target = this.filePath(saveName);
      const previous = `${target}_`;
      return later((): Promise<void> | void => {
        this.fsUnlink(previous);
        this.fsRename(target, previous);
        if (ctx.crashpoint()) return new Promise<void>(() => {}); // the game is gone: never settles
        let written = false;
        try {
          ctx.failpoint("save");
          this.fsWriteFile(target, zip);
          written = true;
        } finally {
          if (written) {
            this.fsUnlink(previous);
          } else {
            this.fsUnlink(target);
            this.fsRename(previous, target);
          }
        }
      }) as Promise<void>;
    },
    // StorageManager.loadFromLocalFile(saveName): rejects when the file is missing
    loadFromLocalFile(saveName: string): Promise<string> {
      return later(() => {
        ctx.failpoint("load");
        const data = ctx.storage.read(this.filePath(saveName));
        if (data === null) throw new Error("Savefile not found");
        return data;
      });
    },
    // StorageManager.localFileExists(saveName)
    localFileExists(saveName: string): boolean {
      return ctx.storage.has(this.filePath(saveName));
    },
    // StorageManager.removeLocalFile(saveName)
    removeLocalFile(saveName: string): undefined {
      ctx.failpoint("remove");
      this.fsUnlink(this.filePath(saveName));
      return undefined;
    },
    // StorageManager.fsMkdir(path): nothing to do (the backend has no directories)
    fsMkdir(_path: string): void {},
    // StorageManager.fsRename(oldPath, newPath): only when oldPath exists
    fsRename(oldPath: string, newPath: string): void {
      const data = ctx.storage.read(oldPath);
      if (data === null) return;
      ctx.storage.write(newPath, data);
      ctx.storage.remove(oldPath);
    },
    // StorageManager.fsUnlink(path): only when it exists
    fsUnlink(path: string): void {
      if (ctx.storage.has(path)) ctx.storage.remove(path);
    },
    // StorageManager.fsReadFile(path): the text, or null when the file does not exist
    fsReadFile(path: string): string | null {
      return ctx.storage.read(path);
    },
    // StorageManager.fsWriteFile(path, data)
    fsWriteFile(path: string, data: string): void {
      ctx.storage.write(path, data);
    },
    // StorageManager.saveToForage(saveName, zip): then refreshes the key cache
    saveToForage(saveName: string, zip: string): Promise<void> {
      const key = this.forageKey(saveName);
      return Promise.resolve().then(() => {
        ctx.failpoint("save");
        ctx.storage.write(key, zip);
      }).then(() => this.updateForageKeys()).then(() => undefined);
    },
    // StorageManager.loadFromForage(saveName): null when missing (localforage.getItem)
    loadFromForage(saveName: string): Promise<string | null> {
      return Promise.resolve().then(() => {
        ctx.failpoint("load");
        return ctx.storage.read(this.forageKey(saveName));
      });
    },
    // StorageManager.forageExists(saveName): from the key cache
    forageExists(saveName: string): boolean {
      return this._forageKeys.includes(this.forageKey(saveName));
    },
    // StorageManager.removeForage(saveName): then refreshes the key cache
    removeForage(saveName: string): Promise<void> {
      const key = this.forageKey(saveName);
      return Promise.resolve().then(() => {
        ctx.failpoint("remove");
        ctx.storage.remove(key);
      }).then(() => this.updateForageKeys()).then(() => undefined);
    },
    // StorageManager.updateForageKeys(): reload the key cache (localforage.keys()); Promise of 0
    updateForageKeys(): Promise<number> {
      this._forageKeysUpdated = false;
      return Promise.resolve().then(() => {
        this._forageKeys = ctx.storage.keys();
        this._forageKeysUpdated = true;
        return 0;
      });
    },
    // StorageManager.forageKeysUpdated(): Scene_Boot waits for it
    forageKeysUpdated(): boolean {
      return this._forageKeysUpdated;
    },
    // StorageManager.fileDirectoryPath(): MZ's is <game folder>/save/; the shim's is relative to the backend.
    fileDirectoryPath(): string {
      return "save/";
    },
    // StorageManager.filePath(saveName)
    filePath(saveName: string): string {
      return this.fileDirectoryPath() + saveName + ".rmmzsave";
    },
    // StorageManager.forageKey(saveName): needs the database ($dataSystem); a TypeError before it loads
    forageKey(saveName: string): string {
      return "rmmzsave." + g.$dataSystem.advanced.gameId + "." + saveName;
    },
  };
}
