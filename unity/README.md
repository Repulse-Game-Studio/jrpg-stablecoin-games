# unity

The C# port of the payment core (`Scfg.Core`), the start of the Unity port. It is in development: there is no Unity glue, UI or release yet, only the engine-independent core, its tests and the driver for the integration tests. The Python core (`plugin/game/python-packages/scfg_core/`) is the reference, and the TypeScript port (`rpgmaker/src/core/`) is the closest template. The core replays every conformance vector in `conformance/vectors/`.

## Layout

| Path | Contents |
|---|---|
| `Packages/com.repulse.scfg/Runtime/Core/` | The core, one C# file per Python module (`Config.cs` is `config.py`, `Solana/Tx.cs` is `solana/tx.py`). Each file's header comment names its Python twin. netstandard2.1 BCL only, `LangVersion` 9.0, no `UnityEngine`, no NuGet package. The package ID `com.repulse.scfg` is a placeholder |
| `core/` | `Scfg.Core.csproj`, which compiles the package's `Runtime/Core/` for the .NET build |
| `core-tests/` | NUnit tests: Tier 1 and Tier 2 conformance runners, per-module tests, the lints, the culture reruns and the single-thread run |
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

## Error model

Exceptions follow Python's class tree (`Errors.cs`): `ValueError`, `TypeError`, `KeyError`, `IndexError`, `ConfigError(Code)`, `RestoreError(Code)`, `ArithmeticError`, `OverflowError` and the decimal errors. Tier 1's `"invalid"` expectation passes only for a `ValueError` or `TypeError` that is not a `ConfigError`; any other exception fails the case.

Inputs stay loosely typed as in Python: `Config.BuildConfig`, `Config.BuildChain`, `Config.AddItem` and `Engine.StartSession` take a `JsonObject` of Python's keyword arguments, and an unknown key is a `TypeError`. JSON integers are `long` (`BigInteger` beyond it) and floats are `double`.

## Async rules

The core runs on Unity's main thread, so it never creates a thread or waits.

- No threads and no timers: no `Task.Run`, `Task.Delay`, `Task.Yield`, `ConfigureAwait`, `Thread`, `ThreadPool`, `Timer`, `Parallel` and no `async void`.
- `Task<T>` is only a promise (a `TaskCompletionSource` the caller completes). The transport returns one; the core awaits it and never blocks on it (`.Result`, `.Wait()`).
- The clock is injected (`RpcClient.Clock`, the Engine's and Watcher's clocks); the core does not read the system time itself. Entropy is injected too (`IEntropy`).

## Lints

`core-tests/LintTests.cs` scans every file under `Runtime/Core/` and fails with `path:line: pattern (why)`. The rule tables in that file are the source of truth. Comments and string literals are ignored, and the holes of interpolated strings are scanned. The rules are:

| Group | Forbidden |
|---|---|
| Unity | `using UnityEngine` |
| Threads | The threads and timers listed under "Async rules" |
| Reflection | `System.Reflection`, `dynamic`, `Activator`, `GetType().Get...`, `System.Text.Json`, `Newtonsoft`, `record` types (IL2CPP stripping, and `IsExternalInit` on netstandard2.1) |
| Culture | `Trim*`, `char.IsDigit` / `IsWhiteSpace` / `IsLetter` and relatives, `ToLower` / `ToUpper`, `CompareTo`, `String.Compare` and `StartsWith` / `EndsWith` / `IndexOf` without `StringComparison.Ordinal`, `ToString()` and `String.Format` without `CultureInfo.InvariantCulture`, `Parse` / `TryParse` of numeric types without it, `\d` and `\s` in a `Regex`. Python's string rules come from `Py` instead |

The lints cannot see `$"{n}"` formatting a non-string with the current culture: interpolate only strings, and put `ToString(CultureInfo.InvariantCulture)` inside the hole.

To waive a `ToString()` or `Format` line that is culture-free (an integer, say), end it with `// culture-ok: <reason>`. A marker without a reason does not count.

## Culture reruns and the single-thread test

- `CultureTests` replays Tier 1 and Tier 2 again under `tr-TR` (dotted and dotless i) and `de-DE` (the decimal comma). A culture that does not take effect, such as in globalization-invariant mode, is itself a failure.
- `Tier2SingleThreadTest` runs the Engine under a single-threaded `SynchronizationContext` with transports that answer with incomplete tasks, which only a pump loop on the test thread completes. Every transport call and continuation must be on that thread, and the core must never block.

## Known divergences from Python

Deliberate or accepted differences. None is reachable with valid, schema-conforming data.

| Area | C# | Python |
|---|---|---|
| Decimal exponent | `Dec` clamps an integer exponent at ±2^30, so `stored_amount("0E-1000000000000000000")` prints `0E-1073741824` | Keeps the exponent |
| Large integers | Some fields are `long`-bounded: `LogIndex`, `BlockNumber`, a Solana slot (2^63 gives `OverflowException` or `OverflowError`, not the Python result), a receipt `log_index` | Unbounded |
| Non-finite numbers | `Json.Loads` accepts `NaN`, `Infinity` and `-Infinity` as doubles (the TypeScript port rejects them) | Accepts them |
| Malformed node data (Solana) | A string `accountIndex`, a non-string `owner` or a non-integer `accountIndex` keeps a receipt that Python drops | Drops it |
| Non-string stored values | A non-string `kind`, `item`, `token` or `chain` in a stored session makes `Expire` throw out of `Tick`, or loops `internal_error` | Carries the value |
| Tips | `PreparePayment` with a non-string `title` or `item` on a tip is a `TypeError` | Accepted |
| Config | `confirmations` of 2**70 is a `ValueError`, raised before `config.no_usable_token` | `ConfigError` |
| Config | A non-dict `methods`, `rpc_urls` or `confirmations` is a `TypeError` | `AttributeError` |
| Digit limit | `Py.Int` of a 5000-digit string with a bad character gives the invalid-literal message | The digit-limit message |
| Stored sessions | A non-dict session value is a `KeyError` | `TypeError` |
| Transaction size | `Tx` casts to byte unchecked: more than 255 accounts wraps | `ValueError` |
| Recorded data (test runner) | Malformed recorded data throws `ReplayError` and is not added to the problem list | Not applicable |
