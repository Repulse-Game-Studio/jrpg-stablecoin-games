# unity

The C# port of the payment core (`Scfg.Core`), the start of the Unity port. It is in development: there is no Unity glue, UI or release yet, only the engine-independent core, its tests and the driver for the integration tests. The Python core (`plugin/game/python-packages/scfg_core/`) is the reference, and the TypeScript port (`rpgmaker/src/core/`) is the closest template. The core replays every conformance vector in `conformance/vectors/`.

## Layout

| Path | Contents |
|---|---|
| `Packages/com.repulse.scfg/Runtime/Core/` | The core, one C# file per Python module (`Config.cs` is `config.py`, `Solana/Tx.cs` is `solana/tx.py`). Each file's header comment names its Python twin. netstandard2.1 BCL only, `LangVersion` 9.0, no `UnityEngine`, no NuGet package. The package ID `com.repulse.scfg` is a placeholder |
| `Packages/com.repulse.scfg/Runtime/link.xml` | What the IL2CPP linker must keep: `Scfg.Core` and `Scfg.Desktop` whole, and `RNGCryptoServiceProvider`. Unity ignores a `link.xml` inside a package, so the package's Editor code must hand it to the linker (not written yet) |
| `core/` | `Scfg.Core.csproj`, which compiles the package's `Runtime/Core/` for the .NET build |
| `core-tests/` | NUnit tests: Tier 1 and Tier 2 conformance runners, per-module tests (`WorkerTests` with its fakes in `WorkerFakes.cs`: a manual `FakeScheduler` and a counting engine stand-in; `CheckoutTests`, `CheckoutStepsTests` and `CheckoutSolanaTests` with theirs in `CheckoutFakes.cs`: fake EVM and Solana JSON-RPC nodes behind `ITransport`, an immediate and a deferred submitter, fake pages), the lints, the culture reruns and the single-thread runs |
| `core-driver/` | A net8.0 console app, the JSON-lines driver that `tests/integration/test_cs_core.py` runs against Anvil and solana-test-validator (protocol: `rpgmaker/test/integration/engine_driver.ts`) |
| `Scfg.sln` | The solution holding the three projects above |

## Development

Needs the .NET 8 SDK. If it is installed in `~/.dotnet` (as the dotnet-install script does) and not on `PATH`, put it there first.

```bash
export PATH=$HOME/.dotnet:$PATH DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

dotnet test unity/Scfg.sln -c Release                   # all tests; none may fail or skip
dotnet build unity/core/Scfg.Core.csproj -warnaserror   # the core compiles without warnings
dotnet build unity/core-driver -c Release               # the integration driver (the pytest twins build it themselves)

# the driver against local chains (anvil / solana-test-validator on PATH)
uv run pytest -m integration tests/integration/test_cs_core.py
```

`SCFG_VECTORS` overrides the vectors directory the tests read. `tests/integration/cs_driver.py` looks for `dotnet` on `PATH`, then in `$DOTNET_ROOT`, then in `~/.dotnet`. Without one the twins skip; `SCFG_REQUIRE_CS=1` (set in CI) makes that a failure. The EVM and Arc twins need `anvil`, which CI installs.

## State file

`StateFile` reads and writes the player's payment state, `{"version": 1, "pending": {...}, "receipts": {...}}` (the Engine's two stores, in the frozen format of `conformance/schema/state.json`). `StateSchema` is that schema as a C# table, and a test compares it with the file. `StateFile.Parse(text)` gives the stores, or a `Why` (Godot's `store.gd` texts: not JSON, not an object, an unknown version, no pending/receipts objects, or the first pending record that `StateFile.CheckPending` refuses, with `state_schema.py`'s problem text). A file with a `Why` is refused whole. A receipt record that `StateFile.CheckReceipts` refuses does not refuse the file: the receipts are kept as they are, because the Engine's reverify removes malformed receipts and reports each one (`tier2/reverify/malformed_entries_are_removed`), and `StateLoad.ReceiptProblems` lists them for the glue to log. `StateFile.Serialize` writes the stores in insertion order.

The Tier 2 runner checks every vector's stores against the state file before the first step and after each one: they must pass `StateFile.Check` and come back from `Parse(Serialize(...))` strictly equal (a long is not a double). Only the receipts a vector's own setup stores malformed are excused.

## Error model

Exceptions follow Python's class tree (`Errors.cs`): `ValueError`, `TypeError`, `KeyError`, `IndexError`, `RuntimeError`, `ConfigError(Code)`, `RestoreError(Code)`, `ArithmeticError`, `OverflowError`, the decimal errors and `concurrent.futures`' `CancelledError` and `InvalidStateError` (a `Job`'s). Tier 1's `"invalid"` expectation passes only for a `ValueError` or `TypeError` that is not a `ConfigError`; any other exception fails the case.

Inputs stay loosely typed as in Python: `Config.BuildConfig`, `Config.BuildChain`, `Config.AddItem` and `Engine.StartSession` take a `JsonObject` of Python's keyword arguments, and an unknown key is a `TypeError`. JSON integers are `long` (`BigInteger` beyond it) and floats are `double`.

## Async rules

The core runs on Unity's main thread, so it never creates a thread or waits.

- No threads and no timers: no `Task.Run`, `Task.Delay`, `Task.Yield`, `ConfigureAwait`, `Thread`, `ThreadPool`, `Timer`, `Parallel` and no `async void`.
- `Task<T>` is only a promise (a `TaskCompletionSource` the caller completes). The transport returns one; the core awaits it and never blocks on it (`.Result`, `.Wait()`).
- Clocks: `RpcClient.Clock` defaults to `Rpc.MonotonicSeconds` (a `Stopwatch`, as Python uses `time.monotonic`), and the backends' `MakeRpc` pass no clock, so RPC cooldowns run on that default. The Engine's and Watcher's clocks are injected, and so is the Tier 2 runner's. Entropy is injected too (`IEntropy`).
- The single writer is `Worker` (`Worker.cs`, twin of `worker.py`): the game submits commands, `Func<Engine, Task<object?>>` (`Commands.Sync` / `Commands.Of` build them from plain or async methods; `SubmitFn` is the delegate the controllers take), and polls the `Job` it gets back, Python's `Future` (`Done`, `Result`, `Exception`, `Cancel`, `AddDoneCallback`). A `Job` is not a `Task`: whoever wants to await one builds it from `AddDoneCallback`, so a failed job nobody looks at is never an unobserved `Task` exception. A done callback that throws goes to the job's sink (the Engine's `OnError` for a worker's jobs) and the other callbacks still run.
- One pass at a time: `RunOnce` runs the queued commands, reverifies on the first pass, ticks and replaces the `Snapshot`. Its in-flight state is set before any command runs, so a command, `Wake`, `Submit` or `RunOnce` from inside a pass adds exactly one follow-up pass (a command must not await `RunOnce`: deadlock). A command never runs inside a tick. The pass drains the queue until it is empty, so callbacks that keep resubmitting commands that complete synchronously freeze the frame; resubmit from a later frame. A faulted `RunOnce` Task is marked observed, so one nobody awaits is never logged.
- Delayed work goes through an injected `IScheduler` (`Schedule(fn, seconds)` / `Cancel(handle)`, Unity's glue implements it on its frame loop); the core never creates a timer. The loop re-arms after each timer-driven run with `Interval()` (default 15 s), or 0 while commands are queued, and hands a failed run to `OnError`.
- IL2CPP and WebGL: nothing resolves a type by name. SHA-256 is the core's own (`Sha256`, as `SHA256.Create()` looks its implementation up through `CryptoConfig`), and `SystemEntropy` draws from one `RNGCryptoServiceProvider` it constructs directly (`SystemEntropy.Fill` hands out its bytes). `System.Security.Cryptography` appears only in `Sessions.cs`.

## Checkout

`CheckoutController` (`Checkout.cs`, twin of `checkout.py`) is the state behind the payment screen: the UI reads its properties and calls its methods from buttons and calls `Poll()` every frame, which pumps the browser checkout and picks up the session start. It reaches the Engine only through the `SubmitFn` it is given (`worker.Submit`), with `Commands.Of` / `Commands.Sync`, and gets the watcher's outcomes through `OnOutcome` (from `worker.DrainOutcomes()`). A start that fails with `RpcUnavailable` is `network_unavailable`, any other failure (a cancelled job included) `start_failed`, and a server factory or `Start()` that throws `browser_unavailable`. The browser checkout is an `ICheckoutServer` (`CheckoutServer.cs`) built from `ServerArgs`; its `session_info` has exactly the keys of `checkout.py`'s `start_browser`. `Checkout.FinalStates`, `HintVerdicts`, `StepOrder` and `Messages` are read-only tables, and a test compares `Messages` with `conformance/vectors/messages.json`.

## Lints

`core-tests/LintTests.cs` scans every file under `Runtime/Core/` and fails with `path:line: pattern (why)`. The rule tables in that file are the source of truth. Comments and string literals are ignored, and the holes of interpolated strings are scanned. The rules are:

| Group | Forbidden |
|---|---|
| Unity | `using UnityEngine` |
| Threads | The threads and timers listed under "Async rules" |
| Reflection | `System.Reflection`, `dynamic`, `Activator`, `GetType().Get...`, `System.Text.Json`, `Newtonsoft`, `record` types (IL2CPP stripping, and `IsExternalInit` on netstandard2.1) |
| Static state | A `static` field that is not `readonly` or `const`, a `static readonly` field of a mutable collection type (`List<`, `Dictionary<`, `HashSet<`, `IList<`, an array, `JsonObject`, ...), a static property with a setter, a get-only static property of a mutable collection type, and a `static readonly` field or get-only static property of a read-only type (`IReadOnly*`, `IEnumerable`) initialized with an array or collection literal that is not wrapped (`AsReadOnly`, `ReadOnlyCollection`, `ReadOnlyDictionary`; one-line declarations only). With Unity's domain reload off statics survive between play sessions, so the public tables are `IReadOnlyList` / `IReadOnlyDictionary` over a `ReadOnlyCollection` / `ReadOnlyDictionary` |
| Culture | `Trim*`, `char.IsDigit` / `IsWhiteSpace` / `IsLetter` and relatives, `ToLower` / `ToUpper`, `CompareTo`, `String.Compare` and `StartsWith` / `EndsWith` / `IndexOf` without `StringComparison.Ordinal`, `ToString()` and `String.Format` without `CultureInfo.InvariantCulture`, `Parse` / `TryParse` of numeric types without it, `\d` and `\s` in a `Regex`. Python's string rules come from `Py` instead |

The lints cannot see `$"{n}"` formatting a non-string with the current culture: interpolate only strings, and put `ToString(CultureInfo.InvariantCulture)` inside the hole.

To waive a `ToString()` or `Format` line that is culture-free (an integer, say), end it with `// culture-ok: <reason>`; to waive a static-state line (a private lookup table that is only read), end it with `// static-ok: <reason>`. A marker without a reason does not count, and each marker waives only its own group.

The chain registries `Chains.Mainnet` / `Chains.Testnet` are read-only. The integration driver and the tests point an entry at a local node through the `internal` `Chains.Override(testnet, chain)` and restore the built-in tables with `Chains.ResetRegistries()` (`Runtime/Core/AssemblyInfo.cs` makes them visible to `Scfg.Core.Tests` and `Scfg.Core.Driver` only).

## Culture reruns and the single-thread test

- `CultureTests` replays Tier 1 and Tier 2 again under `tr-TR` (dotted and dotless i) and `de-DE` (the decimal comma). A culture that does not take effect, such as in globalization-invariant mode, is itself a failure.
- `Tier2SingleThreadTest` runs the Engine under a single-threaded `SynchronizationContext` with transports that answer with incomplete tasks, which only a pump loop on the test thread completes. Every transport call and continuation must be on that thread, and the core must never block. `WorkerTests.TheWorkerRunsOnOneThreadAndNeverBlocks` does the same for the worker's timer-driven loop (commands, done callbacks and the re-arm).

## Known divergences from Python

Deliberate or accepted differences. None is reachable with valid, schema-conforming data.

| Area | C# | Python |
|---|---|---|
| Decimal exponent | `Dec` clamps an integer exponent at ±2^30, so `stored_amount("0E-1000000000000000000")` prints `0E-1073741824` | Keeps the exponent |
| Large integers | Some fields are `long`-bounded: `LogIndex`, `BlockNumber`, a Solana slot, a receipt `log_index` and `block`, a stored session's `cursor` and `start_block` and its match's `block` and `log_index`, a chain's `chain_id`, `confirmations` and `max_log_range`. Beyond a long is an `OverflowError` (`Ints.ToLong`), not the Python result | Unbounded |
| Non-finite numbers | `Json.Loads` accepts `NaN`, `Infinity` and `-Infinity` as doubles (the TypeScript port rejects them) | Accepts them |
| Malformed node data (Solana) | A string `accountIndex`, a non-string `owner` or a non-integer `accountIndex` keeps a receipt that Python drops | Drops it |
| Non-string stored values | A non-string `kind`, `item`, `token` or `chain` in a stored session makes `Expire` throw out of `Tick`, or loops `internal_error`. Unreachable from a state file: `StateFile.Parse` refuses a file whose pending records do not match `conformance/schema/state.json` | Carries the value |
| Tips | `PreparePayment` with a non-string `title` or `item` on a tip is a `TypeError` | Accepted |
| Config | `confirmations` of 2**70 is an `OverflowError`, raised before `config.no_usable_token` | `ConfigError` |
| Config | A non-dict `methods`, `rpc_urls` or `confirmations` is a `TypeError` | `AttributeError` |
| Digit limit | `Py.Int` of a 5000-digit string with a bad character gives the invalid-literal message | The digit-limit message |
| Stored sessions | A non-dict session value is a `KeyError` | `TypeError` |
| Transaction size | `Tx` casts to byte unchecked: more than 255 accounts wraps | `ValueError` |
| Recorded data (test runner) | Malformed recorded data throws `ReplayError` and is not added to the problem list | Not applicable |
