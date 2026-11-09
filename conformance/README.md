# Conformance vectors

Language-neutral JSON recorded from the Python reference core (`scfg_core`). The JS and GDScript
cores replay these files and must agree with them. Vectors are generated, never hand-written.

## Regenerate / check

    python tools/gen_vectors.py            # writes conformance/vectors/
    python tools/gen_vectors.py --check    # exits 1 if anything is stale, missing or extra

Generation is deterministic: running twice yields byte-identical files.

## Format rules

- UTF-8, written as `json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False) + "\n"`.
- No JSON number may have `|n| >= 2**53` (Godot and JS read numbers as floats). The serializer
  raises on an oversized int. See "Integer encodings" below for what is a string and what is a number.
- Integral floats are written as ints; non-integral floats raise (clocks use whole seconds).
- `Decimal` values are written as `str(d)` (for example `"4.990000"`; zero is `"0.000000"`; `units_to_decimal` is fixed at 6 decimals).
- Determinism: entropy from `random.Random(<scenario id>)`, no wall clock, no absolute paths,
  no unsorted set or dict ordering.

## Layout

- `python/scfg_conformance/` the generator package: `canonical.py` (serializer), `vectors.py`
  (`SOURCES` registry), `tier1*.py` (function tables), `scenarios/` and `scenario.py` (Tier 2
  recorder), `simnode.py` (simulated JSON-RPC nodes), `replay.py` (the Python runner),
  `coverage.py`, `state_schema.py`.
- `tests/` tests for the harness itself (`uv run pytest conformance/tests`; also part of `uv run pytest`).
- `vectors/` generated output; every file is registered in `vectors.SOURCES` under its path
  relative to this directory, including `messages.json` (message ids and English text).
- `schema/state.json` and `coverage.json`, described below.

## Tier 1: pure functions

`vectors/tier1/<topic>.json` is `{"tier": 1, "topic": str, "cases": [...]}`. A case is
`{"fn": str, "in": {...}, "out": ...}`, or `{"fn", "in", "error": <code>}` when the function must fail.
Config, item and payment validation is compared by **error code, never message text**. A port calls its own
function for `fn` with `in` and compares the result to `out`. The GDScript runner
(`godot/tests/test_tier1.gd`) currently replays `keccak` and `ed25519`; the other topics are for the M1 to M3 ports.

Encodings: see "Integer encodings" and "Error encodings" below.

## Integer encodings

- **Tier 1 and harness-authored fields** (function inputs and outputs, `entropy` values, `setup`): big
  values (uint256, token units, field elements) are decimal strings, and byte strings are lowercase hex
  without `0x` in fields named `*_hex`.
- **Tier 2 `returns` and `snapshot`** are the Engine's own values, typed as in `schema/state.json`. Their
  ints (block numbers, log indexes, timestamps, `units`) are plain JSON numbers, guaranteed below 2**53 by the
  serializer; amounts are `Decimal` strings (`"4.990000"`). Do not expect decimal strings for every integer there.
- `build_solana_payment` returns `transaction` as base64 (the core's own format, not a `*_hex` field), and
  its balances as decimal strings.
- Arc's 18-decimal native values never appear as numbers: they live inside `response_text` (hex `data`), and
  the Engine floors them to 6-decimal units before they reach `returns` or `snapshot`. A port needs big-integer
  arithmetic to parse them (`evm/arc_large_*` pins values above 2**63).

## Error encodings

- **Tier 1** `"error": "<code>"`: either a ConfigError code of the form `<subject>.<problem>` (always
  contains a dot), compared by code and never by message; or the literal `"invalid"`, meaning any
  non-ConfigError rejection (Python `ValueError` or `TypeError`): the reference rejected the input, the exact
  exception class is not part of the contract.
- **Tier 2** `raises` is `null` or one of
  `{"type": "ConfigError", "code": "<subject>.<problem>"}`,
  `{"type": "RestoreError", "code": "invalid_hash" | "tx_not_found" | "tx_reverted" | "tx_not_a_payment" | "network_unavailable"}`,
  or `{"type": "other"}` (any other exception; opaque, no code, and declared with `s.expect("other")` when authoring).
- The same bad input can be `"invalid"` in Tier 1 yet `{"type": "other"}` in Tier 2 (different layers reject
  it with a plain exception); both mean "rejected, class unspecified".

## Tier 2: Engine scenarios

`vectors/tier2/<scenario id>.json`, recorded by running `scfg_core.engine.Engine` against simulated
JSON-RPC nodes (`python/scfg_conformance/simnode.py`) and replayed without them
(`python/scfg_conformance/replay.py`, the template for the JS and GDScript runners).

    {"tier": 2, "id": "smoke/evm_unlock_paid", "covers": ["tests/unit/test_x.py::test_y", ...],
     "setup": {"clock": 1700000000, "config": {build_config kwargs}, "custom_chains": [{build_chain kwargs}],
               "items": [{add_item kwargs incl. item_id}], "pending": {...}, "receipts": {...}},
     "steps": [{"at": int, "action": str, "args": {...},
                "entropy": [{"fn": "token_hex" | "randbelow" | "token_bytes", "n": int, "value": str | int}],
                "calls": [{"url", "method", "params", "response_text"} |
                          {"url", "method", "params", "transport_error": {"message", "status"}}],
                "returns": ..., "raises": null | {"type": "ConfigError" | "RestoreError", "code"} | {"type": "other"},
                "internal_errors": int,
                "snapshot": {"pending", "receipts", "owned", "tip_total", "unready"}}]}

A runner, per vector:

- Builds the config (`build_chain` for each custom chain, `build_config`, then `add_item` in order) and an
  Engine with `setup.pending` / `setup.receipts`. One integer clock drives the Engine and every RPC client
  (cooldowns included); before each step it is set to `at`.
- Answers each request from the step's `calls` as a **multiset** keyed by `(url, method, params)` (the
  JSON-RPC `id` is ignored): any order, each recorded call used once, a request made twice needs two
  recorded copies, and identical requests take their recorded copies **in recorded order**. `params`
  match by JSON value equality: objects ignore key order, arrays are ordered, strings compare exactly,
  numbers compare by value (`1` equals `1.0`, which matters for Godot's float-only numbers), and a boolean
  never equals a number (`true` is not `1`). The Python runner compares
  `json.dumps(params, sort_keys=True)`. That is the same rule for everything the Python core sends: it
  never sends floats, so the only case where it could differ (`1` vs `1.0`) cannot arise. `response_text` is parsed with the port's JSON reader; a
  `transport_error` is raised as the transport's failure (`invalid JSON response` is one: parsing belongs
  to the transport). An unrecorded request, an unused recorded call, a draw that differs from `entropy`
  (by `fn` and `n`, in order) or an unused draw fails the step.
- Feeds `entropy` to the core's randomness (`token_bytes` values are hex).
- Runs the action with `args`: `start_session(kind, chain, **rest)`, `build_solana_payment(sid, account)`,
  `report_hint(sid, tx)`, `set_ui_status(sid, status)`, `discard_session(sid)`, `tick()`, `reverify()`,
  `restore_lookup(tx)`, `restore_apply(from_step, choices)`. `restore_apply` applies the candidates recorded
  as `returns` of step `from_step` (`transfer.class` is `Transfer` or `TransferLog`); `choices` keys are
  candidate indexes as strings. A float argument is written `{"float": "<repr>"}`.
- Compares `raises`, then `returns` (null when it raised), then `internal_errors` (crashes the Engine
  swallowed while stepping one session; their type is never recorded), then `snapshot` (`owned` sorted,
  `tip_total` the Decimal as a string, `unready` sorted `[chain, token]` pairs).

`schema/state.json` freezes the receipt and pending-session format (`state_schema.check`), and
`coverage.json` maps every test of the required unit-test files to the vectors that twin it
(`coverage.check`; a Tier 2 vector's `covers` must agree with it).

Scenario authors: an Engine crash the Engine swallowed (`internal_errors > 0`) or a raise other than
`ConfigError`/`RestoreError` (`{"type": "other"}`) fails generation unless the scenario declared it for that
one action with `s.expect("internal_error")` / `s.expect("other")`. The declaration is authoring metadata
and is not written to the vector.

## Adding a scenario

1. Write a function in `python/scfg_conformance/scenarios/<group>.py` decorated with
   `@scenario("<group>/<name>", covers=(...))`. It drives the Engine through the `Scenario` helper
   (`s.config`, `s.item`, `s.start_session`, `s.tick`, ... and the simulated nodes from `simnode.py`)
   and may `assert` on what it recorded.
2. `covers` lists the Python unit tests the scenario twins, as `covers("<module>", "<name>", ...)` (`tests/unit/test_<module>.py::test_<name>`).
   Add the same pairing to `coverage.json` (the vector id is `tier2/<group>/<name>`).
3. Run `python tools/gen_vectors.py`, review the new file, commit the generator change and the vector together.
   CI runs `--check`, so a stale, missing or extra vector fails the build.

Every test in a file listed in `coverage.json` must map to vectors or be marked
`{"python_only": "why no port can share it"}`. Use `python_only` only for behaviour no port can observe
through the vector surface: Python object aliasing and exception classes, defensive branches no Engine path
reaches, the default stderr hook, and the checkout server and UI (excluded from conformance). A reason is
required and is read by reviewers; a test that can be twinned must be twinned.

## Entropy and clock

Vectors never read the wall clock or the OS random source. The clock is the integer `at` of each step
(whole seconds). Randomness (`token_hex`, `randbelow`, `token_bytes`) is recorded per step as `entropy`
and a runner must feed exactly those draws, in order. At generation time entropy comes from
`random.Random(<scenario id>)`, so output is deterministic.

## Notes for port authors

- **Response ids.** `response_text` is the raw text Python received and echoes Python's JSON-RPC request ids.
  Ports must not check response ids against their own request ids.
- **Duplicates and equality.** Identical calls in one step are consumed in recorded order. Params equality is
  JSON equality: numbers compare by value, a boolean is never a number.
- **Key order.** Iterate setup `pending` and `receipts` in the file's (sorted) key order; the recorder
  sorts them in `Scenario.initial()`, so recording ran in that order too. Some results depend on it
  (for example the removed keys that `reverify` returns are compared in order).
- **Float inputs.** `{"float": "<repr>"}` expresses a float that must be rejected or handled as a float. It appears only in
  the Tier 1 item and payment inputs (`items.json`, `payments.json`, built by `tier1_config.py`) and as
  a Tier 2 step argument (`replay.py` understands it; no committed scenario needs it today). `amounts.json` uses
  `{"type": "float"}` for `parse_amount`, and the runner builds the float.
- **Units.** Units fed to `units_to_decimal`, `format_units` and `decode` are decimal integer strings
  (`^-?\d+$`). The invalid `decode` cases (`"abc"`, `""`, `"1.5"`) are meant to be rejected.
- **Readiness contract.** `tier2/solana/readiness_crash` pins that a `getAccountInfo` result without `value`
  raises a contained error (counted in `internal_errors`, nothing marked unready) and is not read as a missing account.
- **Big file.** `tier2/solana/scan_far_behind.json` is about 0.5 MB by necessity: `SIG_PAGE` and
  `MAX_SIG_PAGES` are module constants, so reaching the page bound takes that many recorded signatures.
- **Reference quirks pinned as behaviour.** These are not bugs to fix in a port, and a port must not "improve" them:
  - `parse_amount("1_000")` is `1000.00` and Unicode whitespace around a number is stripped (`str.strip`).
  - `compute_ownership` `tip_total` is a Python `Decimal` sum: the result carries the most fraction digits
    among the summed amounts (`"1.5"` plus `"0"` is `"1.5"`, `"1.000000"` stays six digits) and no tips gives
    `"0"`. Ports must not format it with fixed decimals.
  - `units_to_decimal` inputs beyond 28 significant digits are deliberately absent: Python's default
    `Decimal` precision rounds them and the result is not a stable contract.
  - `parse_transfer_log` accepts short `data` (`"0x01"` is 1).
  - `normalize_pubkey` strips whitespace, `decode_fixed` does not.
  - Solana hints expire after `SOLANA_HINT_TIMEOUT_S = 180` seconds, EVM hints after `HINT_TIMEOUT_S = 1800`.
  - A Decimal zero at 6 decimals is `"0.000000"`.
- **QR.** The longest-URI QR input assumes a 64-character label; the Python code has no such cap.
- **GDScript runner.** Run `godot --headless --path godot --import` once per fresh checkout (it registers the
  `class_name` globals; re-run after adding one). Every test method must end with `return T.done()`: a runtime
  script error only aborts the erroring function, so the runner fails any method that does not return `true`.
