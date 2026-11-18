// Copyright 2026 Repulse
// Twin of scfg_core/errors.py, plus Python's built-in exception classes (the class tree of err.gd's BASES).
#include "Scfg/Err.h"

#include "Scfg/Py.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace scfg {
namespace errors::detail {

struct Kind {
    std::string_view name;
    // Every base class, all the way up, space-separated (BaseException last); empty for BaseException itself.
    std::string_view bases;
};

// godot/addons/scfg/core/err.gd's BASES, whole: Python built-ins, the decimal module's
// signals, the QR generator's DataTooLongError, scfg_core's own, and asyncio's CancelledError (a BaseException,
// so an `except Exception` around a session step does not swallow a cancellation).
constexpr std::array<Kind, 28> kKinds = {{
    {"BaseException", ""},
    {"Exception", "BaseException"},
    {"ValueError", "Exception BaseException"},
    {"TypeError", "Exception BaseException"},
    {"LookupError", "Exception BaseException"},
    {"KeyError", "LookupError Exception BaseException"},
    {"IndexError", "LookupError Exception BaseException"},
    {"AttributeError", "Exception BaseException"},
    {"ArithmeticError", "Exception BaseException"},
    {"OverflowError", "ArithmeticError Exception BaseException"},
    {"ZeroDivisionError", "ArithmeticError Exception BaseException"},
    // the decimal module's signals (Dec): ArithmeticErrors, not ValueErrors
    {"DecimalException", "ArithmeticError Exception BaseException"},
    {"InvalidOperation", "DecimalException ArithmeticError Exception BaseException"},
    {"Inexact", "DecimalException ArithmeticError Exception BaseException"},
    {"Rounded", "DecimalException ArithmeticError Exception BaseException"},
    {"Overflow", "Inexact Rounded DecimalException ArithmeticError Exception BaseException"},
    // the vendored QR generator's (_qrcodegen.py)
    {"DataTooLongError", "ValueError Exception BaseException"},
    {"AssertionError", "Exception BaseException"},
    // coio.run_sync's "no transport to reach <url>"
    {"RuntimeError", "Exception BaseException"},
    {"ScfgError", "Exception BaseException"},
    {"ConfigError", "ScfgError ValueError Exception BaseException"},
    {"RestoreError", "Exception BaseException"},
    {"RpcError", "Exception BaseException"},
    {"RpcUnavailable", "Exception BaseException"},
    {"TransportError", "Exception BaseException"},
    // err.gd's Godot-only HostGone (its HTTP host node left the tree): kept, as the table is copied whole, for
    // the Unreal transport's equivalent. Not a TransportError, so the RPC client cools no endpoint down for it.
    {"HostGone", "Exception BaseException"},
    // worker.py's Abandoned: a superseded command's refused RPC request (Engine::refuse_requests). Not a
    // TransportError either, so no endpoint cools down.
    {"Abandoned", "Exception BaseException"},
    {"CancelledError", "BaseException"},
}};

const Kind* find(std::string_view name) {
    for (const Kind& k : kKinds) {
        if (k.name == name) return &k;
    }
    return nullptr;
}

std::vector<std::string>& misuse_list() {
    static std::vector<std::string> list;
    return list;
}

void report_misuse(std::string message) { misuse_list().push_back(std::move(message)); }

PrintSink& print_sink() {
    static PrintSink sink;
    return sink;
}

}  // namespace errors::detail

bool Err::is_a(std::string_view k) const {
    const errors::detail::Kind* asked = errors::detail::find(k);
    if (asked == nullptr) {
        errors::detail::report_misuse("Err::is_a: unknown kind " + std::string(k) + " (add it to Err.cpp's kKinds)");
        return false;
    }
    const errors::detail::Kind* mine = errors::detail::find(kind);
    if (mine == nullptr) {
        errors::detail::report_misuse("Err::is_a: an Err of unknown kind " + kind + " (add it to Err.cpp's kKinds)");
        return false;
    }
    if (mine == asked) return true;
    std::string_view rest = mine->bases;
    while (!rest.empty()) {
        size_t space = rest.find(' ');
        if (rest.substr(0, space) == k) return true;
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
    }
    return false;
}

std::string Err::str() const {
    if (!code.empty()) return kind + "(" + code + "): " + message;
    if (rpc_code) return kind + "(" + py::dec(*rpc_code) + "): " + message;
    if (status) return kind + "(status=" + py::dec(*status) + "): " + message;
    return kind + ": " + message;
}

Err Err::make(std::string k, std::string msg) {
    if (errors::detail::find(k) == nullptr) {
        errors::detail::report_misuse("Err::make: unknown kind " + k + " (add it to Err.cpp's kKinds)");
    }
    Err e;
    e.kind = std::move(k);
    e.message = std::move(msg);
    return e;
}

Err Err::value_error(std::string msg) { return make("ValueError", std::move(msg)); }
Err Err::type_error(std::string msg) { return make("TypeError", std::move(msg)); }
Err Err::key_error(std::string msg) { return make("KeyError", std::move(msg)); }
Err Err::index_error(std::string msg) { return make("IndexError", std::move(msg)); }
Err Err::overflow_error(std::string msg) { return make("OverflowError", std::move(msg)); }

Err Err::config_error(std::string error_code, std::string msg) {
    Err e = make("ConfigError", std::move(msg));
    e.code = std::move(error_code);
    return e;
}

Err Err::restore_error(std::string error_code, std::string msg) {
    Err e = make("RestoreError", std::move(msg));
    e.code = std::move(error_code);
    return e;
}

Err Err::rpc_error(int64_t json_rpc_code, std::string msg) {
    Err e = make("RpcError", std::move(msg));
    e.rpc_code = json_rpc_code;
    return e;
}

Err Err::transport_error(std::string msg, std::optional<int> http_status) {
    Err e = make("TransportError", std::move(msg));
    e.status = http_status;
    return e;
}

namespace errors {

// Python prints the traceback (traceback.print_exception); a value has none, so this is err.gd's one line.
void print_error(const Err& e) {
    const std::string line = "scfg: contained error: " + e.str();
    if (const PrintSink& sink = detail::print_sink()) {
        sink(line);
        return;
    }
    std::fputs((line + "\n").c_str(), stderr);
}

PrintSink set_print_sink(PrintSink sink) {
    PrintSink previous = std::move(detail::print_sink());
    detail::print_sink() = std::move(sink);
    return previous;
}

std::vector<std::string>& misuse() { return detail::misuse_list(); }
void clear_misuse() { detail::misuse_list().clear(); }

}  // namespace errors

namespace detail {

void bad_result_access(const char* what, const Err* e) {
    std::string line = std::string("scfg: ") + what;
    if (e != nullptr) line += ": " + e->str();
    line += "\n";
    std::fputs(line.c_str(), stderr);
    std::abort();
}

void programming_error(const char* what) {
    std::string line = std::string("scfg: programming error: ") + what + "\n";
    std::fputs(line.c_str(), stderr);
    std::abort();
}

}  // namespace detail
}  // namespace scfg
