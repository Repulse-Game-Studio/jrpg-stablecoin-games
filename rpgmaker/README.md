# rpgmaker

TypeScript port of the payment core (`src/core/`, dependency-free) and the RPG Maker MZ plugin glue (`src/mz/`). Needs Node 22.18 or newer, which runs `.ts` files directly (type stripping); CI uses Node 24.

    npm ci
    npm run typecheck   # tsc: src/core alone (ES2020, no DOM, no Node types); tests + ../conformance/js (Node types);
                        # src/mz + the MZ shim (ES2020 + DOM, no Node types); their tests (DOM + Node types)
    npm run build       # esbuild -> dist/js/plugins/SCFG.js, plus dist/test/shim.js and dist/test/harness.js
    npm test            # node --test: unit tests, the conformance vector runners, the bundle checks

`npm run build` (`build.mjs`, esbuild pinned in the lockfile) writes `dist/js/plugins/SCFG.js`: the MZ header from `src/mz/plugin_header.ts`, then one unminified ES2020 IIFE of `src/mz/main.ts`. The shared checkout page (`../plugin/game/scfg/web/*`) and `src/mz/ui/*.css` are embedded as strings with esbuild's text loader through the virtual module `scfg:assets`; the glue reads them from `src/mz/assets.ts`, which tests fill from disk (`test/mz_assets.ts`). `dist/` is not committed; `node build.mjs --outdir DIR` builds elsewhere (the bundle tests build twice and compare).

The glue's tests run on an MZ stand-in, `test/mz_shim/` (written from MZ's public plugin API, no KADOKAWA code, unverified against the real runtime; see its README). `test/harness/index.html` is a blank-canvas page that loads the shim's browser build, `SCFG.js` and a boot script, in MZ's order: serve this folder after `npm run build` and open `/test/harness/index.html` (`?test` for a test play).

The plugin parameters and commands are declared once, in `src/mz/plugin_header.ts` (parameter keys from `src/mz/params.ts`); `src/mz/commands.ts` registers the commands (Unlock Item, Tip, Restore Purchases, Wallet Help) with their interpreter wait mode and `SCFG.STATUS` codes, `src/mz/switches.ts` keeps the items' owned switches in sync and `src/mz/menu.ts` adds the optional Tip Jar menu entry. `demo/` is the asset-free demo kit to copy into a new MZ project (`demo/README.md`), checked by `test/demo_kit.test.ts`.

Integration against real local chains reuses the Python fixtures. `test/integration/engine_driver.ts` is a JSON-lines process (one command per stdin line, one `{"ok": ...}` or `{"error": ...}` reply per stdout line) that runs the Engine with the `fetch` transport; `tests/integration/test_js_core.py` drives it through `js_driver.JsEngine`. From the repository root, with `anvil` and `solana-test-validator` on `PATH` (and `npm ci` done here):

    uv run pytest -m integration tests/integration/test_js_core.py

It is skipped when `node` (22.18 or newer), `rpgmaker/node_modules`, `anvil` or `solana-test-validator` is missing. With `SCFG_REQUIRE_JS=1` (set in CI) a missing or too old `node` or missing `node_modules` fails the run instead.

The plugin end to end, on the shim against local chains (from the repository root, `anvil` / `solana-test-validator` on `PATH`):

    uv run python tools/run_rpgmaker_web_e2e.py [--family arc]              # web: the built SCFG.js in Chromium
    uv run python tools/run_rpgmaker_desktop_e2e.py [--family arc|solana]   # desktop (NW.js) path, in Node

The web run serves this folder and plays `test/harness/e2e.html` (the demo kit's `js/plugins.js`, with the runner's test-only parameter override `window.scfgE2EConfig` applied by `test/harness/e2e_config.ts`, never by SCFG.js): the real DOM sheet and checkout overlay, driven by real clicks and held keys, with a mock browser wallet; the shim keeps a web game's saves in the page's `localStorage`, so they outlive the reload step. The desktop run (Ruling M2-5: no NW.js here) is `test/e2e/desktop_driver.ts`, a JSON-lines process with `SCFG-REPLY ` replies: the real glue on the shim as an NW.js game (saves in a temporary game folder), the real localhost checkout page and Node https transport, and a headless sheet host that presses the sheet's buttons by name; the browser checkout it opens is paid in Playwright.

The sheet's DOM tests and the checkout server's security suite (from the repository root, after `npm ci` and Playwright's Chromium):

    uv run pytest tests/rpgmaker_ui                                  # the payment sheet and restore screen in headless Chrome, on the shim
    SCFG_REQUIRE_JS=1 uv run pytest tests/server_security            # the Node server next to the Python (and, with SCFG_GODOT, the GDScript) one
    uv run python tools/check_text_ids.py --check                    # the sheet's text ids against the shared messages and the Godot template

Releasing and the docs: `uv run python tools/package.py --engine rpgmaker` runs `npm run build` and writes `dist/scfg-rpgmaker-mz-<version>.zip` (`js/plugins/SCFG.js`, `img/scfg/README.txt`, `scfg-README.md` joined from `docs/developers/rpgmaker/`, `scfg-LICENSE.txt`, `scfg-NOTICES.txt` with the Nayuki and Keccak notices). It refuses when `VERSION` in `src/mz/version.ts` differs from `scfg_core.__version__`, or when the built plugin header does not show it (MZ has no version annotation: `@plugindesc ... v<version>` and the first `@help` line, both generated from `VERSION`). The plugin is tested on the shim only, not yet on real RPG Maker MZ; the docs say so. CI (`.github/workflows/test.yml`) runs all of the above, and builds twice and compares to keep `SCFG.js` reproducible.

## Real-MZ campaign checklist

Everything above runs on the shim. When the plugin is first run in the real RPG Maker MZ editor and runtime
(desktop NW.js and a web deployment), check these, each written from MZ's public documentation or from memory and
not yet seen in the real thing:

1. **Unhandled errors.** MZ's `SceneManager.onError` / `onReject` (window `error` and `unhandledrejection`) stop the
   game with the error screen, as the shim models it. Check that a script's `SCFG.pay(...).then(...)` with no
   `.catch` on payments off or a sheet host showing nothing resolves `failed` and the game goes on, that a Script
   event command with `await` is the SyntaxError the docs describe, and that nothing in a full payment (sheet,
   desktop page, web overlay with a real wallet extension) reaches the error screen.
2. **Encrypted deployment.** With "Encrypt image files", `img/scfg/logo.png_` is read and decrypted through
   `Utils.hasEncryptedImages()` and `Utils.decryptArrayBuffer()` (their existence and the file format are modelled in
   the shim from memory), on desktop and web; a missing logo in a release only warns.
3. **Minimised or hidden window.** The pump runs on MZ's frames (`requestAnimationFrame`): check that payment status
   freezes while the NW.js window is minimised or covered and catches up on return, with nothing lost.
4. **Node CA bundle and proxies.** RPC through Node `https` on NW.js; check the fetch retry (Ruling 12) behind a
   company proxy and with an antivirus/company root certificate, and that plain networks never use it.
5. **NW.js / Chromium version.** The sheet uses flexbox `gap` (Chromium 84, NW.js 0.47): check the NW.js MZ 1.0 and
   the current MZ ship, and the sheet's look on the oldest.
6. **`process.mainModule`.** `platform.ts gameDir()` finds the game folder from `process.mainModule.filename`;
   check it on NW.js for a deployed game and for test play from the editor.
7. **`nw.Shell` / `nw.Clipboard`.** "Open checkout in browser", "View on explorer", Wallet Help, Copy and Paste go
   through `nw.Shell.openExternal` and `nw.Clipboard.get()`: check each on Windows, macOS and Linux.
8. **Common events.** A plugin command in a common event called from a map event runs on MZ's child interpreter
   (`setupChild`, command117), which the shim does not model: check that it waits and stores its status.
9. **Plugin header.** The editor's reading of the generated annotations (`@type file` with `@dir img/scfg/` and
   `@require 1`, the `struct<Item>` list, `multiline_string` JSON parameters, `@command` / `@arg`), and that the
   file must be named `SCFG.js` for its parameters to be found.
10. **Demo kit.** `demo/data/Map001.json`, `MapInfos.json` and `js/plugins.js` open in the editor as the README says
    (event command codes, plugin command parameter format, switch and variable numbering).
11. **Two games on one origin.** Two web deployments served from one origin (one host, two paths) with different
    `advanced.gameId` values keep separate `rmmzsave.<gameId>.scfg` entries in localforage and never see each
    other's receipts (the shim models MZ's `StorageManager.forageKey`); check where the editor sets `advanced.gameId`
    for a new project and whether a copied project keeps it.

## Using the core (for M2)

Import from `src/core/index.ts`; anything it does not export is internal. The Engine is not re-entrant: only the `Worker` touches it, one command at a time, and the game loop talks to the Worker.

```ts
import {
  CheckoutController, Engine, FetchTransport, RestoreController, Worker, addItem, backendFor, buildConfig, preparePayment,
} from "./src/core/index.ts";

const config = buildConfig({ wallet: "0x…" });
addItem(config, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
const transport = new FetchTransport();
const monotonic = () => performance.now() / 1000; // seconds; the core itself is ES2020 and has no monotonic clock
const engine = new Engine(config, {
  transport,
  pending, receipts,                              // the game's persisted records (copied in)
  // RPC cooldowns must not jump with the wall clock: build each chain's client with a monotonic one.
  rpc_factory: (chain) => backendFor(chain).makeRpc(chain, transport, monotonic),
});
const worker = new Worker(engine);
worker.start();

const ctl = new CheckoutController({
  config: engine.config,
  spec: preparePayment(engine.config, "unlock", { item: "chapter2" }),
  submit: (fn) => worker.submit(fn),             // every engine call goes through the worker
  unready: () => worker.snapshot.unready,        // routes that cannot take a payment right now
  server_factory: (args) => myPageServer(args),  // optional: the browser checkout page
});
const restoreCtl = new RestoreController({ submit: (fn) => worker.submit(fn) });

// Every frame:
for (const outcome of worker.drainOutcomes()) ctl.onOutcome(outcome); // each open CheckoutController
ctl.poll();                                      // and restoreCtl.poll() for an open RestoreController
const { owned, tip_total, pending: p, receipts: r } = worker.snapshot; // detached copies to persist and show
```

- `submit` returns a `Job`; controllers poll it themselves, so call `poll()` on every open controller every frame. `drainOutcomes()` hands each watcher outcome out once: give it to every open `CheckoutController` (each ignores other sessions). `RestoreController` has no `onOutcome`.
- `ServerArgs.on_tx(tx_hash, sender)`: the page server first runs the page's message through `parseTxMessage` and passes its two results, so the hash is already normalised; a message it rejects never reaches `on_tx`.
- Inject the RPC clock through `rpc_factory` as above. Without it the Engine's `clock` (wall clock, `Date.now()`) drives the endpoint cooldowns; Python's default is `time.monotonic`.
- Never `await worker.runOnce()` (or a `Job`'s promise) from inside a command passed to `submit`: the command runs inside that very run, so the await waits on itself and deadlocks. Return the value instead, or submit a follow-up command.
- Token amounts are the branded `Units` bigint: get them from the core (`decimalToUnits`, a session's `units` via `asUnits(BigInt(rec.units))`), never from a raw number.
- `config.items`, `config.custom_chains`, `registry(config)` and `enabledChains(config)` are `Map`s: item ids and chain keys may be integer-like (`"10"`), and a Map keeps Python's insertion order where a plain object would list those keys first.
- Python's `decimal` signals are their own classes, not `ValueError`s: `decode` of 10**32 units or more (so a restore of such a transfer) throws `InvalidOperation`, and a stored tip beyond Decimal's range makes `snapshot()` (`computeOwnership`) throw `Overflow`, both `ArithmeticError`s as in Python.

Core files use erasable TypeScript only (no enums, namespaces, parameter properties or decorators) and name every relative import with its `.ts` extension.
