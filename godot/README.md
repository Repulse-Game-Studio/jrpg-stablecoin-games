# godot

Pure-GDScript port of the payment core for Godot 4.3 and newer (`addons/scfg/core/`), its headless test runner (`tests/`) and the JSON-lines driver for the integration tests (`tests/integration/`). This directory is a minimal dev project; the Godot glue (autoload, sheet, page server, web bridge) comes with M4.

The Python core (`plugin/game/python-packages/scfg_core/`) is the reference. Each Python module has one GDScript file at the same relative path (`scfg_core/solana/tx.py` is `addons/scfg/core/solana/tx.gd`, `class_name ScfgSolanaTx`), with the Python function and constant names. It replays every conformance vector in `conformance/vectors/` on 4.3 and on the latest stable Godot. No GDExtension, no C#, no editor-only API.

## Development

```bash
# a verified Godot binary (SHA-512 checked); prints its path
uv run --no-project python tools/fetch_godot.py --version 4.3-stable --dest /tmp/godot
GODOT=/tmp/godot/Godot_v4.3-stable_linux.x86_64

# once per checkout, after adding a class_name, and after switching Godot versions
# (it registers the class_name globals in godot/.godot/)
$GODOT --headless --path godot --import

# every test_*.gd in tests/, Tier 1 and Tier 2 vectors included (about a minute)
$GODOT --headless --path godot --script res://tests/run.gd -- --vectors="$PWD/conformance/vectors"
# the same, failing closed as CI does (see "The runner")
uv run --no-project python tools/run_godot_tests.py --godot $GODOT

# benchmarks after the tests (medians against the M0 budgets; a miss is reported, not failed)
$GODOT --headless --path godot --script res://tests/run.gd -- --vectors="$PWD/conformance/vectors" --bench

# the core against local Anvil and solana-test-validator (from the repository root, with
# anvil and solana-test-validator on PATH); skipped when SCFG_GODOT is unset
SCFG_GODOT=$GODOT uv run pytest -m integration tests/integration/test_gd_core.py
```

## The runner

`tests/run.gd` loads each `test_*.gd` directly in the tests directory (`--tests=<res:// dir>` picks another), runs its `test_*` methods one at a time, awaits coroutine tests, and prints `PASS`/`FAIL`/`SKIP` lines and the summary `<n> passed, <n> failed, <n> skipped`. Tests use `T` from `tests/t.gd` and must end with `return T.done()`: a runtime script error only aborts the erroring function, so a method that does not return `true` fails. A test that made no checks fails, a skip never counts as a pass, and a coroutine test pending after `--test-timeout` seconds (default 600) fails the run.

The exit code is 0 only for at least one pass and no failure. Godot itself can still exit 0 when `run.gd` cannot compile (for example a project that was never imported). `tools/run_godot_tests.py` therefore also requires the summary line with `n > 0` passed and 0 failed, and it kills and fails a run that outlasts `--timeout` seconds (default 900). CI's `godot-core` job runs it on 4.3-stable and 4.7.2-stable.

## The error model

GDScript has no exceptions. A function that Python can raise from returns **its value or an `ScfgErr`** (`addons/scfg/core/err.gd`), and its doc comment carries `## @fallible`.

- `kind` is the Python exception class name. `is_a(kind)` follows Python's class tree, so `except ValueError` is `e.is_a("ValueError")`, which also holds for a ConfigError.
- `code` is the ConfigError/RestoreError string code or the RpcError int code, else `null`. `message` is Python's `str(exc)`. `status` is the HTTP status of a TransportError.
- Every caller checks: `var v: Variant = f(); if v is ScfgErr: return v`. It can also handle the error where Python has an `except`.
- A Godot runtime error is never part of the contract. A coroutine that should have produced a value but returned `null` was aborted, and a session step treats that as a contained failure, like Python's `except Exception`.
- Constructors cannot fail, so a fallible class is built with `create()`, which returns the object or its `ScfgErr`. `ScfgEngine.new` keeps the error in `init_error`.
- Never use `assert()` for anything a caller can trigger: export builds strip it.

## Coroutine rules

- A function that does network I/O, directly or through a callee, is a coroutine, and every caller `await`s it. A missing `await` hands the caller a function state instead of the value.
- On `ScfgEngine` the coroutines are `start_session`, `build_solana_payment`, `tick`, `reverify` and `restore_lookup`. Every other method is plain.
- The transport is injected: any `RefCounted` with a coroutine `post_json(url, payload, timeout) -> Variant` (see `coio.gd`). The real one is `ScfgHttpTransport.new(host)`, whose `HTTPRequest` children live under `host`, a Node that must already be in the scene tree. A `SceneTree` script's root enters the tree only after `_initialize`, so await one frame before the first request.
- The Engine is not re-entrant. Only the single-writer `ScfgWorker` calls it, one command at a time, and the game reads its detached snapshots. A command must not `await` `run_once()`, which would deadlock.
- A lambda that awaits makes only the lambda a coroutine, not the function around it.

## Lints

`tests/test_lints.gd` scans `addons/scfg/core/**.gd` on every run. Its header documents the exact rules and known limits.

- **Error propagation:** every call to an `@fallible` function must be checked with `is ScfgErr` within the next two lines, returned directly from a fallible function, or waived with `# err-ok: <reason>`. A function that builds or returns an `ScfgErr` must itself be `@fallible`.
- **No unawaited coroutines:** a call to a known coroutine (found by its body, a `## @coroutine` tag, or an override of one) must be awaited on the same line.
- **No asserts** in core code.

## Numbers and JSON

`int` holds everything Python stores in state; the frozen schema keeps those values below 2^53. `ScfgBigInt` holds anything that can pass 2^63 (uint256 log values, Arc's raw 18-decimal values, field elements). `JSON.parse_string` turns every number into a `float`, so a single value from JSON is converted with `ScfgPy.json_int`, and a whole document (persisted state, which the Engine normalises itself, the vectors, driver commands) is read as Python's `json.load` would read it with `ScfgPy.from_json`. A request never carries a float where Python sends an int. Don't write float literals of 2^53 or more, because the GDScript parser misreads them. `ScfgDec` reproduces CPython's `str(Decimal)` forms (`"4.990000"`, `"1E+1"`).

## M4 contracts (glue must honour)

The core leaves these to the Godot glue (autoload, sheet, page server, web bridge). Each one is a rule the M4 code must follow, not something the core checks.

- **Keep the transport's host alive while a request is in flight.** When `ScfgHttpTransport`'s host Node leaves the tree or is freed mid-request, the waiting coroutine resumes synchronously inside the exit-tree notification: the next `add_child` fails with engine errors and the RPC client cools endpoints that were healthy. Stop the Worker (`stop()`) and let its in-flight run finish before freeing the host, or keep the host for the whole session. A follow-up fix is planned (finish deferred on leaving the tree, plus a "host gone" kind that is not a TransportError, with a test).
- **The page's callbacks do not keep the controller alive.** `on_tx`, `get_status` and `build_tx`, which `ScfgCheckoutController` hands to the page through `server_factory`, are bound to the RefCounted controller. They do not hold a reference to it. Call `close()` (or `cancel()` / `finish()`) before releasing a checkout controller, and `stop()` its page; the page checks `is_valid()` before calling any of them.
- **`server_factory`'s page `start()` must not suspend.** The controller calls it without `await`, and so are `stop()`, `reopen()` and `pump()`.
- **Every injected Callable is synchronous except the `fn` given to `submit`.** That covers the Engine's `rpc_factory`, `on_error` and `clock`, and the checkout controller's `server_factory`, `unready` and `browser_wallet`. `submit` itself returns its ScfgJob at once, while `fn(engine)` may be a coroutine because the Worker awaits it. The core calls every other Callable without `await`, so one that awaits hands it a function state instead of its value.
- **Build the Worker from `ScfgEngine.create()`.** `ScfgEngine.new()` keeps a bad config's TypeError in `init_error`, and every method, `snapshot()` included, then returns it. So `worker.snapshot` would be an ScfgErr. Use `create()` and handle its error, or check `worker.snapshot is ScfgErr`.
- **`restore_apply` choices are keyed by int candidate index.** A screen or bridge that holds string keys (JSON, an HTML form) converts them first: `ScfgRestore` finds no choice under a `"0"` key and silently skips that candidate.
- **Sum stored or player-supplied amounts with `ScfgDec.add_checked`.** The plain `add`/`sub`/`mul` skip Python's decimal.Overflow check and are only for operands known to be small.
- **Await a direct `.call()` on a lambda that awaits.** The coroutine lint only follows named functions, so `await f.call(...)` on an awaiting lambda is not checked.
- **State loaded from JSON is normalised by the Engine.** Godot's JSON reader turns every number in `user://scfg/state.json` into a float. `ScfgEngine` passes its `pending` and `receipts` options through `ScfgPy.from_json`, which turns each integral float below 2^53 back into an int (`units`, `log_index`, `block`, `match.value`...), so pass the parsed state straight in. Other JSON the glue feeds the core (bridge messages, page posts) needs `ScfgPy.from_json` too.

## Integration driver

`tests/integration/engine_driver.gd` uses the protocol of the TypeScript driver (`rpgmaker/test/integration/engine_driver.ts`). It reads one JSON command per stdin line and writes one `{"ok": …}` or `{"error": {"type", "code", "message"}}` reply per command. The ops are `init` (with a registry override that patches `ScfgChains.MAINNET`/`TESTNET`), the Engine actions, `snapshot`, `set_receipt` and `clear_receipts`. Results are encoded like the Tier 2 `returns` (`tests/tier2.gd`).

Godot prints its banner on stdout, so each reply line starts with `SCFG-REPLY `. `tests/integration/gd_driver.py` (`GdEngine`) ignores every other line. It skips when `SCFG_GODOT` is unset or not executable, and fails instead with `SCFG_REQUIRE_GD=1` (set in CI). `tests/integration/test_gd_core.py` holds the GDScript twins of the 14 tests in `test_js_core.py`.

stdin is read with `OS.read_string_from_stdin`. Between commands it blocks the main loop, when nothing is in flight; while a command awaits HTTP, the loop runs. Godot 4.3 returns a line in pieces of up to 1023 bytes with the newline kept. 4.4 and later strip the newline and take a buffer size, so the driver passes 1 MiB there, and `GdEngine` refuses longer commands. Commands are sent as ASCII JSON because 4.3 reads stdin as Latin-1.
