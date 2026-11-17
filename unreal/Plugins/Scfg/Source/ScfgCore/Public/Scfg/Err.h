// Twin of scfg_core/errors.py, plus Python's built-in exception classes: a Python exception as a value.
//
// The core never throws or catches, as the lints check: it builds with or without exceptions (Unreal's editor
// builds turn them on, its game builds and the CMake build off). Every function Python can raise from returns a
// Result<T> (Result.h) holding either its value or an Err, and the caller propagates it (SCFG_TRY) or handles it
// where Python has an `except`. `kind` is the Python class name and is_a() follows Python's class tree (err.gd's
// BASES table), so `except ValueError` is `e.is_a("ValueError")`, which also holds for a ConfigError.
#pragma once

#include "Scfg/Export.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg {

struct SCFGCORE_API Err {
    // The Python exception class name: a kind in Err.cpp's BASES table.
    std::string kind;
    // `str(exc)` in Python; for a KeyError the key's repr (`'chain'`), for an RpcError the node's own message
    // (Python's `err.message`; its str() is `<code>: <message>`).
    std::string message;
    // ConfigError and RestoreError: the stable string code (`chains.empty`, `tx_not_found`); else empty.
    std::string code;
    // RpcError: the JSON-RPC error code.
    std::optional<int64_t> rpc_code;
    // TransportError: the HTTP status, or none when no answer arrived.
    std::optional<int> status;
    // RpcError: the endpoint that answered with the error (Python's `err.url = url`); else empty.
    std::string url;

    // Python's isinstance(e, <k>). False when either kind is unknown, which is also recorded in
    // errors::misuse() (a typo such as "ValueEror" must not silently answer).
    bool is_a(std::string_view k) const;

    // `Kind: message`, `Kind(code): message` or `Kind(status=503): message` (err.gd's text, for print_error and
    // test output; not Python's str(exc)).
    std::string str() const;

    // A new error of any kind in the table; an unknown kind is recorded in errors::misuse() (ruling R5).
    static Err make(std::string k, std::string msg);
    static Err value_error(std::string msg);
    static Err type_error(std::string msg);
    // msg is the key's repr, already formatted (ruling R4).
    static Err key_error(std::string msg);
    static Err index_error(std::string msg);
    static Err overflow_error(std::string msg);
    // scfg_core.errors.ConfigError(message, code=code).
    static Err config_error(std::string error_code, std::string msg);
    // scfg_core.restore.RestoreError (Python's message is the code itself).
    static Err restore_error(std::string error_code, std::string msg);
    // scfg_core.rpc.RpcError(code, message).
    static Err rpc_error(int64_t json_rpc_code, std::string msg);
    // scfg_core.coio.TransportError(message, status).
    static Err transport_error(std::string msg, std::optional<int> http_status);
};

namespace errors {

// The default `on_error`: a crash the core contained (one session's step, one stored receipt, one restore
// candidate) is printed and the work goes on: its line ("scfg: contained error: <str()>", no newline) goes to the
// print sink, or to stderr while none is set.
SCFGCORE_API void print_error(const Err& e);

// Where print_error's lines go instead of stderr (a host's log, a test's capture); an empty one restores stderr.
// Returns the sink it replaces. Single-threaded, as the core is.
using PrintSink = std::function<void(std::string_view line)>;
SCFGCORE_API PrintSink set_print_sink(PrintSink sink);

// Programming errors reported so far (an unknown kind given to Err::make or Err::is_a), oldest first.
SCFGCORE_API std::vector<std::string>& misuse();
SCFGCORE_API void clear_misuse();

}  // namespace errors

namespace detail {

// Result's wrong-alternative access (value() of an error, err() of a success): a programming error. Prints what
// and aborts, as std::get does without exceptions.
[[noreturn]] SCFGCORE_API void bad_result_access(const char* what, const Err* e);

// Any other misuse of the core's own machinery (a Promise completed twice, a coroutine that ended without
// co_return, ...): a programming error. Prints what and aborts.
[[noreturn]] SCFGCORE_API void programming_error(const char* what);

}  // namespace detail
}  // namespace scfg
