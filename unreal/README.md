# unreal

The C++ port of the payment core (`ScfgCore`), the start of the Unreal port. It is in development: there is no Unreal glue, UI or release yet, and nothing here includes an Unreal header. What exists is the engine-independent core in plain C++20, its tests and the driver for the integration tests. The Python core (`plugin/game/python-packages/scfg_core/`) is the reference, and the C# port (`unity/`) is the closest template. The core replays every conformance vector in `conformance/vectors/`.

## Layout

| Path | Contents |
|---|---|
| `Plugins/Scfg/Source/ScfgCore/` | The core: `Public/Scfg/*.h` and `Private/*.cpp`, one pair per Python module (`Config.h` is `config.py`, `BackendsSolana.h` is `backends/solana.py`, `SolanaTx.h` is `solana/tx.py`). Each file's first comment names its Python twin, or says "No Python twin" for the C++ machinery (`Result.h`, `Co.h`, `BigInt.h`, `Json.h`, `Py.h`, `Dec.h` and the like). C++20 standard library only: no exceptions, no RTTI, no threads |
| `core-tests/` | The test binary `scfg_core_tests`: per-module tests (`*Tests.cpp`), the lints, the locale reruns and the Tier 1 and Tier 2 conformance runners (`support/Tier1.cpp` with its adapters in `tier1/`, `support/Tier2.cpp`). It uses an in-house runner (`support/Test.h`), not a test framework |
| `core-driver/` | `scfg_core_driver`, the JSON-lines driver that `tests/integration/test_cpp_core.py` runs against Anvil and solana-test-validator (protocol: `rpgmaker/test/integration/engine_driver.ts`), with a test-only blocking POSIX HTTP transport |
| `CMakeLists.txt`, `CMakePresets.json` | The build (CMake 3.25+, Ninja): the core library, the test binary, the driver, the compile-only checks, and one ctest test per test group |

## Development

Needs CMake 3.25 or newer, Ninja and a C++20 compiler (g++ 13, or Clang with libc++ for the `clang-libcxx` preset).

```bash
cd unreal
cmake --preset release && cmake --build --preset release && ctest --preset release   # -O2, assertions kept
cmake --preset asan && cmake --build --preset asan && ctest --preset asan            # AddressSanitizer + UBSan, -funsigned-char
cmake --preset clang-libcxx && cmake --build --preset clang-libcxx && ctest --preset clang-libcxx   # clang++ -stdlib=libc++

# one group or one test while iterating (ctest runs one test per group)
build/release/scfg_core_tests --list
build/release/scfg_core_tests --group EngineTests
build/release/scfg_core_tests --test Tier1.amounts
build/release/scfg_core_tests --test Tier2.smoke/evm_unlock_paid

# the driver against local chains (anvil / solana-test-validator on PATH), from the repository root
uv run pytest -m integration tests/integration/test_cpp_core.py
```

Every preset builds with `-Werror`, and no ctest group may fail or skip. A new test group must be added to `SCFG_TEST_GROUPS` in `CMakeLists.txt`, or `RunnerTests.EveryGroupIsRegistered` fails. `RunnerTests.NothingLeftOut` checks that no vector, topic or test is filtered out.

Environment:

- `SCFG_VECTORS` overrides the vectors directory the tests read (default: `conformance/vectors/`).
- `SCFG_REQUIRE_LOCALES=1` (set in CI) makes `LocaleTests` fail when `tr_TR.UTF-8` or `de_DE.UTF-8` is not installed. Without it, the reruns fall back to another installed locale with a decimal comma.
- `SCFG_REQUIRE_CPP=1` (set in CI) makes the `test_cpp_core.py` twins fail, rather than skip, when the toolchain is missing or unusable. `tests/integration/cpp_driver.py` builds the driver once per session into `unreal/build/integration`. It takes cmake from `PATH` or `~/.local/bin` and needs 3.25 or newer. The compiler is `$CXX` when its program resolves, else `c++`, `g++` or `clang++`. It builds with Ninja, or with make when there is no Ninja. Without one of these the twins skip. A configure or compile failure of the code itself always fails. The EVM and Arc twins also need `anvil`, which CI installs.

## Error model

The core is built without exceptions, as UBT builds it. Every function that can raise in Python returns a `Result<T>` (`Result.h`) holding either its value or an `Err` (`Err.h`). `Status` is `Result<void>`, and `scfg::ok()` is Python's `return None`. The caller propagates the error with `SCFG_TRY(T x, f())`, `SCFG_TRY_VOID(f())` or, in a coroutine, `SCFG_CO_TRY(T x, co_await f())`, or handles it where Python has an `except`. `Result`, `Co` and `Future` are `[[nodiscard]]`. Reading the alternative a `Result` does not hold (`value()` of an error) is a programming error: it prints what happened and aborts.

`Err::kind` is the Python class name, and `is_a()` follows Python's class tree (`Err.cpp`, after `err.gd`'s table). `except ValueError` is `e.is_a("ValueError")`, which also holds for a `ConfigError`, and a `CancelledError` is a `BaseException` that `except Exception` does not catch. The tree has Python's built-ins, the decimal signals, and `errors.py`'s classes with their stable codes (`ConfigError(code)`, `RestoreError(code)`). Also: `RpcError` keeps the JSON-RPC code and the URL, and `TransportError` keeps the HTTP status. An unknown kind given to `Err::make` or `is_a` is recorded in `errors::misuse()`, which the tests check is empty. Tier 1's `"invalid"` expectation passes only for a `ValueError` or `TypeError` that is not a `ConfigError`.

Inputs stay loosely typed as in Python. `build_config`, `build_chain`, `add_item` and `Engine::start_session` take a `Json` dict of Python's keyword arguments, and an unknown key is a `TypeError`. JSON integers are `int64_t` until they need more, then `BigInt` (unbounded, as Python's `int`). Floats are `double`. Dicts keep insertion order.

## Async rules

Python's core is generators: a function that needs the network yields a `Request`. Here such a function returns `Co<T>` (`Co.h`), and the injected `coio::Transport` hands out a `Future<Json>` that the host completes later through its `Promise` (an HTTP callback, a later tick). The core never performs a request, never blocks and never schedules anything.

- No threads, no clocks and no randomness of its own (the lints). The Engine's clock and its `Entropy` are injected. The only exceptions are `Rpc.cpp`'s default monotonic clock and `Entropy.cpp`'s default system source.
- `Co<T>` starts eagerly: the body runs inside the call until its first real suspension, and a finished callee resumes its caller by symmetric transfer. `co_await` of a `Co<T>` gives the `Result<T>` it `co_return`ed. Destroying an unfinished `Co` destroys its frame, which is cancellation.
- `Job<T>` keeps a root coroutine alive for a caller outside coroutines (the driver, the tests, the future glue), as Python's `run_sync` drives a generator. Destroying the `Job` cancels the root.
- A `Promise` destroyed without being completed completes its `Future` with `CancelledError`. Completing it twice is a programming error.
- Coroutine parameters are taken by value. A reference, `string_view`, span or pointer would dangle after the first suspension (`LintTests.CoroutineParamsByValue`).
- No coroutine lambdas, in the core or the tests (ruling R8, `LintTests.NoCoroutineLambdas`). Clang miscompiles some of them (llvm/llvm-project#91983), so coroutines are named functions with default-constructible promises.
- `Co<void>` ends with `co_return scfg::ok();`, since flowing off the end of a coroutine is undefined.
- Teardown order: destroy every `Job` (and every `Co` outside one) before the objects its coroutines use: the Engine, the RPC clients and the transport. A transport destroyed first drops its promises, and each one resumes its waiter with `CancelledError` inside the destructor, on objects that are being torn down.
- g++ 13.3 has an internal compiler error on a braced initializer list inside a `co_await`'s operand. Build the argument in a local first (`Json params = Json::list({a, b}); co_await call(m, params);`).

## Lints

`core-tests/LintTests.cpp` scans every file under `Plugins/Scfg/Source/ScfgCore/` and fails with `path:line: rule (why)`. Its rule tables are the source of truth. The file-level checks are in `core-tests/support/Lint.cpp`. Comments and the contents of string and char literals are ignored. `RulesBite` and `FileChecksBite` feed every rule an offending and a clean input. The rules are:

| Group | Forbidden |
|---|---|
| Exceptions and RTTI | `throw`, `try`, `catch`, `std::exception`, `dynamic_cast`, `typeid` |
| Threads and clocks | `<thread>`, `<mutex>`, `<future>`, `<condition_variable>` and the other thread headers, `std::thread`, `std::async`, `sleep` and relatives, `<chrono>`, `<ctime>`, the `std::chrono` clocks, `time()`, `rand()`, `<random>`, `random_device` |
| Locale | `<locale>`, the iostream and stringstream headers, `<regex>`, `<cctype>` and the `is*`/`to*` character functions, `<cstdio>`, `to_string`, `sto*`, `strto*`, `ato*`, `printf`, `scanf`, `puts`, `setlocale` and relatives. Python's string rules come from `scfg::py` instead |
| Portable types | `long` (32 bits on Windows), `__int128` (not on MSVC), `wchar_t` and `std::wstring`; every header needs `#pragma once` |
| Unity build | `namespace {` and `static` at namespace scope in a `.cpp`: Unreal's unity build puts every `.cpp` in one translation unit, so file-local names go in a named `detail` namespace |
| Coroutines | A `Co<T>` parameter that is a reference, view or pointer; a coroutine lambda (the tests too) |
| Files | An include that is neither standard C++20 nor `Scfg/<x>.h`; a first comment that names no existing Python twin (or "No Python twin"); two basenames that differ only in case |

A file is exempt from a line rule only through `kWaivers` in `LintTests.cpp` (ruling R3), which names the file and the one rule it needs, for example `PyFloat.cpp` with `printf` and `strto*` for the shortest float repr.

Two compile-only targets check the core as Unreal will build it:

- `scfg_core_poisoned` compiles every source and every public header on its own, with `core-tests/support/UnrealMacroPoison.h` force-included. That header defines the plain words that Unreal and the Windows headers `#define` (`check`, `ERROR`, `TEXT` and others) as poison tokens, so a core name that spells one fails to compile.
- `scfg_core_unity` compiles the whole core as one translation unit, as UBT's unity build does.

## Locale reruns and the deferred replay

- `LocaleTests` replays Tier 1 (every topic) and Tier 2 (every vector) again under `tr_TR.UTF-8`, for the dotted and dotless i, and under `de_DE.UTF-8`, for the decimal comma (`core-tests/support/Locales.h`). It sets `setlocale` and `std::locale::global`, and counts what it replayed. Without either locale it falls back to another installed one with a decimal comma. Under `SCFG_REQUIRE_LOCALES=1` a missing one fails the test.
- `Tier2Deferred` replays every Tier 2 vector a second time with a transport that completes requests only from the runner's pump loop, one per turn, as Unreal's HTTP callbacks complete them on a later tick. Every networked step must suspend and resume correctly from the pump. A step that leaves a request unanswered fails. This plays the role of the C# port's `Tier2SingleThreadTest`.

## Known divergences from Python

Deliberate or accepted differences. None is reachable with valid, schema-conforming data. Python here is the reference's 3.9: messages follow CPython 3.9 where 3.12 changed them.

| Area | C++ | Python |
|---|---|---|
| Deep JSON from a node | A response nested more than 1000 levels deep is a `ValueError`, so the transport reports `TransportError("invalid JSON response")` | `RecursionError`, which escapes `UrllibTransport` (it is not a `ValueError`) |
| Huge amounts | `amounts::decode` of 10\*\*32 units or more is `InvalidOperation`, at any size, and restore skips it as one internal error | `decimal.Overflow` from about 10\*\*1000006 units, which `restore_lookup`'s `except InvalidOperation` does not catch. A uint256 never reaches it |
| Non-ASCII hashes | `tx_hash`, `block_hash`, receipt keys and `get_block_hash` are lowered with `py::lower_for_ascii`: exact for ASCII and for any comparison with ASCII text, but non-ASCII letters other than U+0130 and U+212A keep their case. A hostile node's non-ASCII hash can only fail safe | `str.lower()` |
| Transfer log topics | A dict as topic[1] or topic[2] is `TypeError: unhashable type: 'slice'` | The same on 3.9; 3.12 raises `KeyError` |
| Explorer templates | `py::format1` refuses format specs, indexing and attribute access. An `explorer_tx` whose only `{}` is inside an escaped `{{}}` and which also has a field such as `{:>5}` or `{.x}` is `chain.bad_explorer_tx_braces` | Accepts the first, or raises `AttributeError` for `{.x}` |
| Text colour | `text_on` computes the WCAG contrast with `std::pow` in doubles. A libm whose `pow` differs from CPython's in the last ulp could flip only an exact tie | CPython's `**` |
| Hand-built tokens | A `Token` built without `Token::make`, with `log_decimals < decimals`, is a `ValueError` when a transfer is scaled | Every `Token` runs `__post_init__`, which refuses it. One that skipped it would divide by a float |
| Entropy | `token_bytes` / `token_hex` with a negative `n` give no bytes. Every caller passes a constant | `ValueError` |
| Session order | `list_sessions` refuses list `created_at` keys, and puts a NaN after every number | Orders lists lexicographically. A NaN's place depends on which pairs timsort compares |
| Session extra fields | `create_session` with a truthy `extra` that is not a dict is a `TypeError` | Depends on the iterable: a list of pairs is merged, a str is the `ValueError` of `dict.update`, an int a `TypeError` |
| Engine stores | `Engine`'s `pending` and `receipts` must be None, falsy or dicts; anything else aborts, as a constructor cannot return an `Err`. A backend's `advance` given a non-dict store is a `TypeError` (the Engine never passes one) | `dict(x)` also takes a list of pairs, else `TypeError` or `ValueError`. A non-dict store is the `AttributeError` of `.get` |
| Unset `on_error` | `_advance` and `check_ready` report a contained error through `report()`: `on_error`, or `print_error` when it is unset | Call `self.on_error(e)` directly, so a handler set to None afterwards is a `TypeError` |
| RPC error codes | A JSON-RPC error code beyond int64 is kept as no code (it equals none the core compares with); the message is kept | Keeps the int, and `str(e)` is `"<code>: <message>"` |
| HTTP status | `Err::status` is empty for a status beyond `int`; the message keeps every digit | Keeps the int |
| Comparisons | `py::compare` refuses to order two lists. The core only ever compares a stored value with a number | Orders lists item by item |
| Solana account order | A NaN `accountIndex` is ordered last | Its place depends on the set's hash order |
| Transfer equality | `Transfer::operator==` compares fields with Python's `==`, so a NaN `log_index` never equals itself | Tuple comparison checks identity first, so the same NaN object equals itself |
| Error messages | The class always matches Python's, and the vectors compare only classes and codes. Some texts differ. A `sorted()` or `list_sessions` `TypeError` may name a different pair of types. Two `TypeError`s lack CPython's `scfg_core.config.` / `scfg_core.payments.` prefix. A keyword that repeats a positional argument says "unexpected keyword argument". Adjacent lone surrogates are reported as one position, and a `UnicodeEncodeError` is reported as its base class `ValueError` | CPython's own texts and classes |
| QR segments | `QrSegment::make` takes `std::vector<bool>`, so a bit other than 0 or 1 cannot be passed. Of the internal `assert`s, only the reachable codeword-count check is ported, as an `AssertionError` | Takes any ints, and fails later in `bytearray` |
| JSON values (programming errors) | `deep_copy` copies a list or dict met twice twice, and a cycle aborts `dumps` and `deep_copy`. `py::repr` prints an unassigned code point as it is | `deepcopy`'s memo keeps the aliasing; `dumps` raises `ValueError` on a cycle; `repr` escapes unassigned code points |
| Restore candidates read back (runner and driver) | `candidate_from_json` types the candidate's fields (strs, ints, a bool), except a Solana transfer's `sender`, `tx_hash` and `log_index`, which carry any value. A wrong type is a `TypeError` | The dataclass carries any value |
| Integration driver (test-only) | `PosixHttpTransport`'s timeout bounds the whole request. A malformed status line, a short body or a bad Content-Length is a `TransportError`. A blank input line gets no reply (as in the C# driver) | The socket timeout applies to each blocking operation. `http.client` raises its own exceptions, which `UrllibTransport` does not catch |
