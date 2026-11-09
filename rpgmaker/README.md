# rpgmaker

TypeScript port of the payment core (`src/core/`, dependency-free) for RPG Maker MZ. Needs Node 22.18 or newer, which runs `.ts` files directly (type stripping); CI uses Node 24.

    npm ci
    npm run typecheck   # tsc on src/core alone (ES2020, no Node types), then on tests + ../conformance/js
    npm test            # node --test: unit tests and the conformance vector runners

Integration against real local chains reuses the Python fixtures. `test/integration/engine_driver.ts` is a JSON-lines process (one command per stdin line, one `{"ok": ...}` or `{"error": ...}` reply per stdout line) that runs the Engine with the `fetch` transport; `tests/integration/test_js_core.py` drives it through `js_driver.JsEngine`. From the repository root, with `anvil` and `solana-test-validator` on `PATH` (and `npm ci` done here):

    uv run pytest -m integration tests/integration/test_js_core.py

It is skipped when `node` (22.18 or newer), `rpgmaker/node_modules`, `anvil` or `solana-test-validator` is missing. With `SCFG_REQUIRE_JS=1` (set in CI) a missing or too old `node` or missing `node_modules` fails the run instead.

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

Core files use erasable TypeScript only (no enums, namespaces, parameter properties or decorators) and name every relative import with its `.ts` extension.
