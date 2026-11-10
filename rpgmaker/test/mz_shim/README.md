# MZ shim

A stand-in for the RPG Maker MZ runtime, used by the SCFG plugin's tests: `node:test` runs the
glue against it in Node, and the harness page (`test/harness/`) loads its browser build
(`dist/test/shim.js`, from `npm run build`) for the Playwright DOM tests.

- It is written from MZ 1.x's **public plugin API** as documented and widely used by plugins:
  class, method and property names, signatures, key codes, the input timing, the save locations.
- It contains **no KADOKAWA code**: nothing from `rmmz_*.js` was copied.
- It is **unverified against the real MZ runtime**. Where real behaviour was uncertain, the module
  comment says so. Known uncertain points:
  - `StorageManager.loadObject` for a missing save: here it rejects on NW.js and resolves `null` on
    the web.
  - After a web save or remove, the key cache refresh finishes before the returned Promise resolves
    (MZ starts it alongside the write).
  - `Utils.RPGMAKER_VERSION` is a placeholder ("1.9.0").
  - NW.js saves go through MZ's `StorageManager.fs*` helpers (`fsMkdir`, `fsRename`, `fsUnlink`,
    `fsReadFile`, `fsWriteFile`) as MZ 1.x's `saveToLocalFile` does: the old `<name>.rmmzsave` is
    renamed to `<name>.rmmzsave_` while the new file is written, put back when the write fails, and
    unlinked when it succeeded (and at the start of the next save). The helpers work on the storage
    backend with its keys as paths (`save/scfg.rmmzsave`); `fsMkdir` does nothing. The helper names
    and that order are from memory of MZ 1.x, not checked against a real build.
  - `Graphics` page-to-canvas scaling is fixed at 1.
  - Errors: MZ's `SceneManager.run` -> `initialize` -> `setupEventHandlers` listens for the window's `error` and
    `unhandledrejection`; `onReject` hands the reason to `onError`, which prints it, stops the game loop
    (`SceneManager.stop` -> `Graphics.stopGameLoop`) and shows the error screen (`Graphics.printError`). The
    shim does the same in its own code (scene_manager.ts): the loop stops for good, `shim.errors` /
    `mzShim.errors` hold what stopped it, `shim.frame()` throws from then on, and `resetShim()` throws it, so an
    afterEach fails the test (`shim.takeErrors()` takes an expected one). In Node there is no window: the
    shim turns the process's `unhandledRejection` and `uncaughtExceptionMonitor` events into those window
    events (node:test also fails the test by itself). The member names and that chain are from memory of
    MZ 1.x; MZ's other handlers there (F5, F8, unload) are not modelled.
  - Encrypted deployments ("Encrypt image files"): `Scene_Boot.onDatabaseLoaded` -> `setEncryptionInfo` hands
    `$dataSystem.hasEncryptedImages`, `hasEncryptedAudio` and `encryptionKey` to `Utils.setEncryptionInfo`;
    `Utils.hasEncryptedImages()` answers from them and `Utils.decryptArrayBuffer(buffer)` turns a shipped
    `<file>_` back into the file. The routine is the shim's own code for the format as publicly described: a
    16-byte header (`"RPGMV"`, zero padding, a version: `ENCRYPTION_HEADER`), then the file with its first 16
    bytes XORed with the 16 bytes of the 32-hex-digit key; a wrong header throws. That format, the header bytes
    and those member names are UNVERIFIED against a real deployment. `encryptForDeployment(bytes, key)` (test
    only, not MZ) makes such a file; the `encryption` option sets the database's settings.

`Input._shouldPreventDefault` prevents 8, 9, 33, 34 and 37-40, as MZ (Home and End are not
prevented).

The browser build puts a minimal `localforage` (only `ready()`, resolved) on a page that has none,
standing in for the library MZ's index.html loads; the saves themselves go through the shim's
StorageManager. In a page those saves are kept in `localStorage` under `mzshim:` + the localforage
key (`mzshim:rmmzsave.<gameId>.<name>`; test only, `browser.ts`), so like MZ's IndexedDB saves they
outlive a reload of the tab (the web e2e's reload step relies on it). Each Playwright browser context
starts empty; without a usable localStorage the saves stay in memory.

## Boot order

As in MZ, a plugin script runs **before** the game boots, so at plugin-load time:

1. `installShim()` = the engine's scripts are loaded: `$dataSystem` and every `$game*` object are
   `null`; `StorageManager._forageKeys` is empty and `forageKeysUpdated()` false. `opts.plugins`
   parameters are registered (as `PluginManager.setup`).
2. The plugin script runs (the harness page's `SCFG.js`; in Node, the glue under test).
   `DataManager.isDatabaseLoaded()` is false; `StorageManager.forageKey()` throws a TypeError (it
   reads `$dataSystem.advanced.gameId`), so on the web `StorageManager.exists()` throws too.
3. Boot (`SceneManager.run(Scene_Boot)` in a page, `await bootShim()` / `await shim.boot()` in
   Node): `Scene_Boot.create` starts `DataManager.loadDatabase()` (`$dataSystem` arrives on a later
   macrotask, as MZ's XHR loads) and `StorageManager.updateForageKeys()`; `Scene_Boot.isReady()` is
   false until both are done, then `onDatabaseLoaded()`; `start` calls `DataManager.setupNewGame()`
   and goes to `Scene_Map` (MZ shows the title first).

On the web `StorageManager.exists()` answers from the key cache, so it is false for an existing save
until `updateForageKeys()` has completed; every save and remove refreshes the cache. On NW.js
`exists()` checks the file.

## Simplifications

Each mirrored member carries a comment naming the MZ class and member it imitates. Everything else
is simplified on purpose and said so in the module comments: no rendering, audio, events on the
map, walking animation, title screen or battle; the database is only `$dataSystem`, built from the
shim's options; save data is stored as JSON text (MZ compresses it); JsonEx tags class instances
with `"@"` and refuses `maxDepth` (100) levels of nesting as MZ, but MZ's `"@a"` arrays are not
reproduced.

Use:

```ts
import { installShim, resetShim } from "../../test/mz_shim/index.ts";

const shim = installShim(globalThis, { options: ["test"], plugins: [{ name: "SCFG", status: true, parameters: {} }] });
// load the plugin under test here ($dataSystem is still null, as in MZ)
await bootShim();   // Scene_Boot -> new game -> Scene_Map
shim.keyDown(13);   // MZ key code: OK
shim.frame();       // SceneManager.updateMain() once
resetShim();        // in afterEach: removes the globals and listeners
```

Options (`ShimOptions` in `context.ts`): `nwjs` (desktop: save files instead of localforage keys),
`mobile`, `options` (`Utils.isOptionValid`, e.g. `["test"]`), `switches`/`variables` (database
size), `encryption` (`{images, audio, key}`: an encrypted deployment's database settings), `storage` (a `StorageBackend`; `node_storage.ts` has `DirStorage` for a temp dir),
`plugins`, `document`/`window`/`navigator`, `width`/`height`. The handle also offers
`boot`, `keyUp`, `pointer` (mouse events), `gamepads` (what `navigator.getGamepads()` returns in
Node), `failNext("save" | "load" | "remove")` and `crashNextSave()` (the next NW.js save stops
after the rename, as a hard crash would: only `<name>.rmmzsave_` is left and that save's Promise
never settles). `gameTitle` sets `$dataSystem.gameTitle` (default "Shim Game"). `errors`, `stopped`
and `takeErrors()`: what stopped the game (see Errors above).
