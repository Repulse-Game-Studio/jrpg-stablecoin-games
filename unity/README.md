# unity

The C# side of the Unity port, a Preview (desktop and WebGL; the developer guide is `docs/developers/unity/`, the samples are in `Packages/com.repulse.scfg/Samples~/`, and `tools/package.py --engine unity` builds the release tarball). It has the engine-independent payment core (`Scfg.Core`: the port of `scfg_core` including the worker and the checkout, restore and web overlay controllers), the localhost checkout server (`Scfg.Desktop`), the Unity glue (`Scfg.Unity`: the `SCFG` facade, its runner, store, transport and UI Toolkit screens), their tests, the dev project and the drivers for the integration tests, the server security suite and the Linux e2e. The Python core (`plugin/game/python-packages/scfg_core/`) is the reference, the TypeScript port (`rpgmaker/src/core/`) the closest template for the core, and the Godot addon (`godot/addons/scfg/`) the template for the glue. The core replays every conformance vector in `conformance/vectors/`, and the server passes the shared `tests/server_security` suite.

```csharp
using Scfg;

SCFG.Configure(new ScfgOptions { Wallet = "0x…", Testnet = true, Accent = "#7a5cff" });
SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
ScfgPaymentResult r = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
bool owned = SCFG.Owns("chapter2");
```

The facade is `SCFG`, not `Scfg`: C# cannot resolve `Scfg.Configure` while a namespace `Scfg` exists (the name means the namespace).

## Layout

| Path | Contents |
|---|---|
| `Packages/com.repulse.scfg/Runtime/Core/` | The core, one C# file per Python module (`Config.cs` is `config.py`, `Solana/Tx.cs` is `solana/tx.py`). Each file's header comment names its Python twin. netstandard2.1 BCL only, `LangVersion` 9.0, no `UnityEngine`, no NuGet package. |
| `Packages/com.repulse.scfg/Runtime/Desktop/` | Assembly `Scfg.Desktop` (not built for WebGL): `LocalServer.cs`, the localhost checkout server (`CheckoutServer : ICheckoutServer`, twin of `local_server.py` by way of Godot's non-blocking `local_server.gd`), on non-blocking `System.Net.Sockets` served from `Pump()` |
| `Packages/com.repulse.scfg/Runtime/Unity/` | Assembly `Scfg.Unity`, the only one with `UnityEngine`: the `SCFG` facade (`SCFG.cs`, `SCFG.Flows.cs`, `SCFG.Persistence.cs`, the twin of Godot's `scfg.gd`), its typed options and results, `ScfgRunner` (the hidden MonoBehaviour that pumps it every frame), `UnityScheduler` (the Worker's timer on unscaled real time), `UnityWebRequestTransport`, `ScfgStore` (`persistentDataPath/scfg/state.json`, Godot's `store.gd`), `ScfgDesktop` (the localhost page's factory), `SCFG.Web.cs` and `Web/` (WebGL builds: `ScfgWeb.jslib`, the player's calls into its page; `ScfgWebPage`, the checkout page shown over the game, Godot's `web_page.gd`; `ScfgWebMirror`, the receipts' copy in `localStorage`, Godot's `web_mirror.gd`) and `UI/` (the screen contract `IScfgSheetHost` in `ScfgScreens.cs`, the default host, the payment sheet, the restore screen, the QR view, the toast and the text table `ScfgUiTexts`). `Resources/ScfgWeb/` holds the checkout page as `.bytes` TextAssets (copies of `plugin/game/scfg/web/` made by `tools/sync_web_page.py`), `Resources/ScfgUI/` the USS, the theme and the PanelSettings asset |
| `Packages/com.repulse.scfg/Runtime/link.xml` | What the IL2CPP linker must keep: `Scfg.Core` and `Scfg.Desktop` whole, and `RNGCryptoServiceProvider`. Unity ignores a `link.xml` inside a package, so `Editor/ScfgLinkXml.cs` (an `IUnityLinkerProcessor`) hands it to the linker of every player build |
| `Packages/com.repulse.scfg/Editor/` | Assembly `Scfg.Editor`: `ScfgLinkXml`, `ScfgSettingsEditor` (the SCFG Settings inspector with its Validate button, and the build step that preloads the asset) and `ScfgMaintenance` (remakes the PanelSettings asset; maintainers only) |
| `Packages/com.repulse.scfg/Samples~/` | The Showcase (a one-screen tour of the API) and Demo (a small game) samples, each a script and a scene, listed in `package.json`. `tools/check_unity_samples.py --unity` copies them into the dev project, compiles them and opens their scenes (`Project/Assets/Maintenance/Editor/ScfgSamplesCheck.cs`) |
| `Packages/com.repulse.scfg/Documentation~/`, `README.md`, `LICENSE.md` | The Package Manager's documentation page, the package's README and a copy of the repository's LICENSE (`tools/package.py` checks it) |
| `Packages/com.repulse.scfg/Tests/` | Unity Test Framework assemblies: `Editor/` (edit mode: the facade over a fake EVM node with the frames pumped by hand, the store, the scheduler, the linker hook, a core smoke test) and `Runtime/` (play mode: the transport against a local socket server, the runner, `await`, the real localhost page, the screens on a real panel driven by navigation events, the domain-reload reset; `ScfgFakes.cs` holds the fakes both use) |
| `Project/` | The dev project (Unity 6000.6.9f1, `ProjectSettings/ProjectVersion.txt`), which references the package by path and lists it as testable. `Assets/E2E/` is the e2e's driver (assembly `Scfg.E2E`, idle unless `SCFG_E2E_CONFIG` is set or, in a WebGL player, the page sets `window.scfgE2EConfig`; `Plugins/ScfgE2E.jslib` is its page side) and its build script (Linux and WebGL players) |
| `core/` | `Scfg.Core.csproj`, which compiles the package's `Runtime/Core/` for the .NET build |
| `desktop/` | `Scfg.Desktop.csproj`, which compiles the package's `Runtime/Desktop/` against `core/` |
| `core-tests/` | NUnit tests: Tier 1 and Tier 2 conformance runners, per-module tests (`WorkerTests` with its fakes in `WorkerFakes.cs`: a manual `FakeScheduler` and a counting engine stand-in; `CheckoutTests`, `CheckoutStepsTests`, `CheckoutSolanaTests` and `RestoreFlowTests` with theirs in `CheckoutFakes.cs`: fake EVM and Solana JSON-RPC nodes behind `ITransport`, an immediate and a deferred submitter, fake pages; `OverlayTests` with a fake `IOverlayUi`; `LocalServerTests` over real loopback sockets with a fake clock), the lints, the culture reruns and the single-thread runs |
| `core-driver/` | A net8.0 console app, the JSON-lines driver that `tests/integration/test_cs_core.py` runs against Anvil and solana-test-validator (protocol: `rpgmaker/test/integration/engine_driver.ts`) |
| `server-driver/` | A net8.0 console app, the JSON-lines driver that runs `Scfg.Desktop.CheckoutServer` for the shared localhost server security suite `tests/server_security/` (protocol: `rpgmaker/test/integration/server_driver.ts`); it pumps the server about every millisecond and reads stdin on a thread of its own (it is not package code) |
| `Scfg.sln` | The solution holding the five .NET projects above |

## Development

Needs the .NET 8 SDK. If it is installed in `~/.dotnet` (as the dotnet-install script does) and not on `PATH`, put it there first.

```bash
export PATH=$HOME/.dotnet:$PATH DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

dotnet test unity/Scfg.sln -c Release                   # all tests; none may fail or skip
dotnet build unity/core/Scfg.Core.csproj -warnaserror   # the core compiles without warnings
dotnet build unity/desktop/Scfg.Desktop.csproj -warnaserror   # so does the localhost server
dotnet build unity/core-driver -c Release               # the integration driver (the pytest twins build it themselves)

# the driver against local chains (anvil / solana-test-validator on PATH)
uv run pytest -m integration tests/integration/test_cs_core.py

# the C# server against the shared localhost server security suite (builds unity/server-driver itself)
uv run pytest tests/server_security -m cs_server
```

`SCFG_VECTORS` overrides the vectors directory the tests read. `tests/integration/cs_driver.py` and `tests/server_security/server_handles.py` look for `dotnet` on `PATH`, then in `$DOTNET_ROOT`, then in `~/.dotnet`. Without one the twins and the `cs_server` tests skip; `SCFG_REQUIRE_CS=1` (set in CI) makes that a failure, and a driver build that fails is always a failure. The EVM and Arc twins need `anvil`, which CI installs. The `cs_server` tests are part of the default `uv run pytest` run, next to the Python server's; CI's cs-core job runs them alone with `SCFG_REQUIRE_CS=1`. Both drivers target net8.0 with `RollForward` `Major`, so they also run where only a newer .NET runtime is installed (CI's unit job and release gate use the runner image's preinstalled dotnet). The pytest runs build `core/` and `desktop/` in Release, so a manual `dotnet test -c Release` at the same time can collide on their `obj/` folders: run one at a time.

## Unity

The editor tests and the e2e need a Unity editor of 6.3 LTS or newer (the package declares `"unity": "6000.3"`, the oldest supported LTS; the dev project is on 6000.6.9f1, and CI is to test both 6.3 LTS and 6000.6; so far only 6000.6.9f1 has compiled it) installed with Unity Hub, plus the Linux Build Support (IL2CPP) module for the IL2CPP e2e (Hub → Installs → the editor's gear → Add modules). Close the project in the editor first: one project opens in one editor at a time. Each run takes a minute or more (the first import several).

```bash
UNITY=~/Unity/Hub/Editor/6000.6.9f1/Editor/Unity

# import / compile (writes the package's .meta files; commit new ones; tools/check_unity_meta.py checks them)
"$UNITY" -batchmode -nographics -quit -projectPath unity/Project -logFile /tmp/scfg-import.log

# the Unity Test Framework suites, headless
"$UNITY" -batchmode -nographics -projectPath unity/Project -runTests -testPlatform EditMode -testResults /tmp/em.xml -logFile /tmp/em.log
"$UNITY" -batchmode -nographics -projectPath unity/Project -runTests -testPlatform PlayMode -testResults /tmp/pm.xml -logFile /tmp/pm.log

# the Linux e2e: builds the player, then plays it against a local chain (anvil or solana-test-validator on PATH)
uv run python tools/run_unity_e2e.py --unity "$UNITY" --backend mono --family solana
uv run python tools/run_unity_e2e.py --unity "$UNITY" --backend il2cpp --family evm

# the WebGL e2e: builds the WebGL player (or --player DIR), serves it, plays it in Playwright Chromium
uv run python tools/run_unity_web_e2e.py --unity "$UNITY" --family solana   # or evm, arc (anvil)
```

Both test runs exit 0 when every test passed (2 when a test failed) and write NUnit XML. The play-mode suite runs headless too: UI Toolkit lays out, focuses and dispatches events under `-nographics`, it only draws nothing. The e2e player also runs with `-batchmode -nographics`; `SCFG_E2E_WINDOWED=1` opens a window (under `xvfb-run` when installed). The player redirects its stdout into its log, so its driver answers on a named pipe the runner makes (`SCFG_E2E_OUT`). The WebGL e2e clicks the sheet with the browser's mouse and keyboard, pays in the in-page checkout overlay with the keyboard (Tab and Enter), opens the explorer link, pastes into the hash field (and is refused without the clipboard permission), reloads the tab, deletes state.json from IndexedDB to check the `localStorage` copy, floods the facade's transport to check the download cap, and blocks `indexedDB.open` to check the not-persistent answer.

### WebGL

A WebGL build has no localhost server (`Scfg.Desktop` excludes WebGL): the browser checkout is the shared checkout page shown over the game (`OverlayCheckout` on `ScfgWebPage`), and "Browser wallet" is offered only when the page has a wallet for the chain and every page script ran (a Content-Security-Policy without `'unsafe-eval'` fails them, which development builds warn about). While an overlay is shown the package sets `WebGLInput.captureAllKeyboardInput` to false, so the page gets the keys, and puts the game's own value back once no overlay is shown. Links open with `window.open` (noopener,noreferrer), Copy and Paste use the browser's clipboard (Paste answers a frame later; a refusal shows `paste_blocked` or the `copy_failed` toast). After every save the package asks the page to write `persistentDataPath` to IndexedDB (Unity does it by itself only with `config.autoSyncPersistentDataPath`), development builds warn when Unity's IndexedDB database did not open (blocked site data, a partitioned iframe), and the receipts get a second copy in `localStorage` under `scfg:<productName>:<sha256 of ScfgOptions.WebStorageId, else of the product name>`, Godot's key scheme: every Unity WebGL build on one origin shares one IndexedDB database and writes back the whole tree it loaded, so another game there can delete `state.json`. On 6000.6 a chunk refused by a `DownloadHandlerScript` does not stop the browser's fetch, so `UnityWebRequestTransport.Pump()` (every frame) aborts a download past the cap. Unity's loader logs `[UnityCache] Response is served without Content-Length header` for every chunked RPC answer; that warning is Unity's.

### The facade

`SCFG` is a static facade with typed options (`ScfgOptions`, `ScfgItem`, `ScfgChain`, `ScfgPayRequest`) that become the core's keyword arguments, so the core validates them and reports the ConfigError codes the vectors pin. Misuse never throws: it logs `scfg: …` with `Debug.LogError` and `Configure` / `AddItem` / `AddChain` return the code, `Pay` / `Restore` resolve `Failed` with it as `Error`. `Pay` returns an `Awaitable<ScfgPaymentResult>` (awaitable once, as every `Awaitable`) or takes a callback; the semantics follow Godot and Ren'Py: `AlreadyOwned` at once, keep playing resolves `Pending` and the later result goes to the callback or `PaymentResolved`, quitting with a screen open resolves without delivering anything.

The first `Configure` in play mode creates a hidden `ScfgRunner` (`DontDestroyOnLoad`, `HideAndDontSave`). Its `Update` pumps the facade in Godot's order: the scheduler, the screens closed since the last frame, the Worker's outcomes, every open and kept-playing controller (`Poll()` pumps its localhost page), the restore controllers, then the save. Every timeout runs on `Time.realtimeSinceStartupAsDouble`, so payments keep being watched at `Time.timeScale = 0`. `Application.quitting` and the `SubsystemRegistration` reset (Enter Play Mode Options with domain reload off) close every controller, stop the Worker, end requests in flight and flush the state; every static of the facade lives in one `State` object that the reset replaces.

### The screens

`SCFG.SheetHost` (an `IScfgSheetHost`) shows the flows; the default gives each screen its own `UIDocument` on the package's PanelSettings (sort order 1000), never touching the game's Canvas, TextMeshPro or prefabs. A custom host returns an `IScfgScreen` that raises `Closed` with `cancel`, `keep_playing` or `finish`, and binds to `ScfgCheckoutController` / `ScfgRestoreController` (the core controller plus the actions that leave the game). Restyle the default screens with a style sheet set as `ScfgDefaultSheetHost.SheetStyle` (`.scfg-screen { --scfg-accent: …; --scfg-ink: …; }`). Focus is wired by hand on `NavigationMoveEvent`, which UI Toolkit's own input and the Input System's UI module both send: up and down move through the controls in visual order, left and right within a row, and the ends stay put. The texts are English only, in `ScfgUiTexts` keyed by RPG Maker's ids; `tools/check_text_ids.py` keeps them in step.

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

`RestoreController` (`RestoreFlow.cs`, twin of `restore_flow.py`) is the state behind the restore screen (`input -> checking -> (choose ->) saving -> done`), driven the same way through a `SubmitFn` and `Poll()`. A failed job's `RestoreError` code becomes `Error`; any other failure, a cancelled job or a result of the wrong type is `failed`. `RestoreFlow.Messages` is read-only, and a test compares it with the `MESSAGES` of `restore_flow.py` and `restore_flow.gd`.

`OverlayCheckout` (`Overlay.cs`, twin of `overlay.py`) is the `ICheckoutServer` of a web build: the checkout page shown over the game's own page through an `IOverlayUi` (the glue's bridge), pumped every frame with an injected clock (default a `Stopwatch`). It checks what the page hands back as `overlay.ts` does: a `Take()` that is not a list is no messages, a request id is a `long` or an integral double within ±(2^53 - 1) (never a bool), a reused id cancels the older build, nothing is called or answered after `Stop()`, a `BuildTx` that throws or returns null and a job that ends without a `JsonObject` are 503. The status is pushed when it changed by Python's `==` (`Py.Eq`: `1 == 1.0 == True`, dicts whatever their key order).

## Lints

`core-tests/LintTests.cs` scans every file under `Runtime/Core/` and `Runtime/Desktop/` and fails with `path:line: pattern (why)`. The rule tables in that file are the source of truth. Comments and string literals are ignored, and the holes of interpolated strings are scanned. The rules are:

| Group | Forbidden |
|---|---|
| Unity | `using UnityEngine` |
| Threads | The threads and timers listed under "Async rules" |
| Reflection | `System.Reflection`, `dynamic`, `Activator`, `GetType().Get...`, `System.Text.Json`, `Newtonsoft`, `record` types (IL2CPP stripping, and `IsExternalInit` on netstandard2.1) |
| Static state | A `static` field that is not `readonly` or `const`, a `static readonly` field of a mutable collection type (`List<`, `Dictionary<`, `HashSet<`, `IList<`, an array, `JsonObject`, ...), a static property with a setter, a get-only static property of a mutable collection type, and a `static readonly` field or get-only static property of a read-only type (`IReadOnly*`, `IEnumerable`) initialized with an array or collection literal that is not wrapped (`AsReadOnly`, `ReadOnlyCollection`, `ReadOnlyDictionary`; one-line declarations only). With Unity's domain reload off statics survive between play sessions, so the public tables are `IReadOnlyList` / `IReadOnlyDictionary` over a `ReadOnlyCollection` / `ReadOnlyDictionary` |
| I/O (`Runtime/Core/` only) | `System.Net`, `Socket`, and file I/O (`File`, `Directory`, `FileStream`, `StreamReader`, `StreamWriter`, ...): the core also runs in WebGL builds, its transport is injected and the store lives in the glue |
| Culture | `Trim*`, `char.IsDigit` / `IsWhiteSpace` / `IsLetter` and relatives, `ToLower` / `ToUpper`, `CompareTo`, `String.Compare` and `StartsWith` / `EndsWith` / `IndexOf` without `StringComparison.Ordinal`, `ToString()` and `String.Format` without `CultureInfo.InvariantCulture`, `Parse` / `TryParse` of numeric types without it, `\d` and `\s` in a `Regex`. Python's string rules come from `Py` instead |

`Runtime/Desktop/` has one more group, Sockets: no `*Async(` call, no `Begin*`/`End*` (APM) socket call, no `TcpListener`, `TcpClient` or `NetworkStream`, no `Thread.Sleep`, `Poll` only with a zero timeout, no `Socket.Select`, `Connect`, `Disconnect`, `SendTo`, `ReceiveFrom` or `SendFile`, no linger (`LingerState`, `SocketOptionName.Linger`), no DNS (`Dns.`, `GetHostEntry`, `GetHostAddresses`, `DnsEndPoint`), `Blocking` set only to `false` (an object initializer too), `Close()` without a timeout, `Receive`/`Send` only with the `out SocketError` overload; and every `new Socket(` or `.Accept(` of a file is assigned to a variable that a later `<variable>.Blocking = false;` sets (a textual check: it does not follow branches or aliases).

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
| Checkout start | A start job that succeeds with anything but a session (`JsonObject`) is `start_failed` in `Poll`, as `checkout.gd` does; only a stand-in submit can produce one | Takes the value as the session and fails later |
| Checkout start | A cancelled start job (`Worker.Submit` after `Stop`) is `start_failed` in `Poll`: `Job.Exception()` returns its `CancelledError` | `poll()` raises `CancelledError` from `fut.exception()` |
| Recorded data (test runner) | Malformed recorded data throws `ReplayError` and is not added to the problem list | Not applicable |
