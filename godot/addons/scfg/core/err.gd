## A Python exception as a value: GDScript has no exceptions.
##
## Every function that Python can raise from returns either its value or an ScfgErr, and is
## marked with a `## @fallible` line in its doc comment. Callers check every such call:
##
##     var v = ScfgPy.py_int(text)
##     if v is ScfgErr:
##         return v          # propagate (the caller is then @fallible too)
##
## or handle the error where Python has an `except`. godot/tests/test_lints.gd enforces this.
##
## `kind` is the Python exception class name, and `is_a()` follows Python's class tree, so the
## `except` clauses translate directly: `except ValueError` is `e.is_a("ValueError")`, which
## also holds for a ConfigError, as `class ConfigError(ScfgError, ValueError)` does in Python.
class_name ScfgErr
extends RefCounted

## Each kind's Python base classes (all the way up). The kinds used by the core: Python
## built-ins, the decimal module's signals, the QR generator's DataTooLongError, scfg_core's own (errors.ConfigError, restore.RestoreError, rpc.RpcError,
## rpc.RpcUnavailable, coio.TransportError), asyncio's CancelledError (a BaseException, so an
## `except Exception` around a session step does not swallow a cancellation) and the Godot-only
## HostGone (ScfgHttpTransport).
const BASES := {
	"BaseException": [],
	"Exception": ["BaseException"],
	"ValueError": ["Exception", "BaseException"],
	"TypeError": ["Exception", "BaseException"],
	"LookupError": ["Exception", "BaseException"],
	"KeyError": ["LookupError", "Exception", "BaseException"],
	"IndexError": ["LookupError", "Exception", "BaseException"],
	"AttributeError": ["Exception", "BaseException"],
	"ArithmeticError": ["Exception", "BaseException"],
	"OverflowError": ["ArithmeticError", "Exception", "BaseException"],
	"ZeroDivisionError": ["ArithmeticError", "Exception", "BaseException"],
	# the decimal module's signals (ScfgDec): ArithmeticErrors, not ValueErrors
	"DecimalException": ["ArithmeticError", "Exception", "BaseException"],
	"InvalidOperation": ["DecimalException", "ArithmeticError", "Exception", "BaseException"],
	"Inexact": ["DecimalException", "ArithmeticError", "Exception", "BaseException"],
	"Rounded": ["DecimalException", "ArithmeticError", "Exception", "BaseException"],
	"Overflow": ["Inexact", "Rounded", "DecimalException", "ArithmeticError", "Exception", "BaseException"],
	# the vendored QR generator's (qrcodegen.gd)
	"DataTooLongError": ["ValueError", "Exception", "BaseException"],
	"AssertionError": ["Exception", "BaseException"],
	# coio.run_sync's "no transport to reach <url>"
	"RuntimeError": ["Exception", "BaseException"],
	"ScfgError": ["Exception", "BaseException"],
	"ConfigError": ["ScfgError", "ValueError", "Exception", "BaseException"],
	"RestoreError": ["Exception", "BaseException"],
	"RpcError": ["Exception", "BaseException"],
	"RpcUnavailable": ["Exception", "BaseException"],
	"TransportError": ["Exception", "BaseException"],
	# Godot only (no Python twin): ScfgHttpTransport's host Node left the tree or was freed.
	# Not a TransportError, so ScfgRpcClient lets it propagate and cools no endpoint down.
	"HostGone": ["Exception", "BaseException"],
	"CancelledError": ["BaseException"],
}

## The Python exception class, a key of BASES.
var kind: String = ""
## ConfigError/RestoreError: the String code (`chains.empty`, `tx_not_found`); RpcError: the
## JSON-RPC int code; otherwise null.
var code: Variant = null
## `str(exc)` in Python. For a KeyError that is the key's repr (`'chain'`).
var message: String = ""
## TransportError: the HTTP status (int), or null when no response arrived.
var status: Variant = null
## Extra attributes Python sets on the exception object. RpcError: `url`, the endpoint that
## answered with the error (Python's `err.url = url` in RpcClient._attempt).
var data: Dictionary = {}


## Programming errors reported so far (an unknown kind name), newest last. Tests set
## `silence_misuse` to read them back here instead of printing an ERROR line.
static var misuse: Array[String] = []
static var silence_misuse: bool = false
## Tests only: with `silence_printed` set, the contained errors report() would print (its
## on_error not a valid Callable) are collected here instead, newest last; otherwise they are
## printed and never kept.
static var printed: Array[String] = []
static var silence_printed: bool = false


static func _report_misuse(message: String) -> void:
	misuse.append(message)
	if not silence_misuse:
		push_error(message)


## A new error. An unknown `kind` is a programming error: it is reported with push_error and
## the error is still returned (as that kind, which is then only a BaseException).
static func make(kind: String, message: String, code: Variant = null, status: Variant = null) -> ScfgErr:
	if not BASES.has(kind):
		_report_misuse("ScfgErr.make: unknown kind %s (add it to ScfgErr.BASES)" % kind)
	var e := ScfgErr.new()
	e.kind = kind
	e.message = message
	e.code = code
	e.status = status
	return e


## Python's `isinstance(e, <kind>)`. A `k` that is not a known kind (a typo such as
## "ValueEror") would silently be false, so it is reported with push_error like `make()` does.
func is_a(k: String) -> bool:
	if not BASES.has(k):
		_report_misuse("ScfgErr.is_a: unknown kind %s (add it to ScfgErr.BASES)" % k)
	if k == kind:
		return true
	if BASES.has(kind):
		return k in BASES[kind]
	return k == "BaseException"


## True when `v` is an ScfgErr (the same as `v is ScfgErr`).
static func is_err(v: Variant) -> bool:
	return v is ScfgErr


## Python's `(on_error or print_error)(err)`: a crash the core contained (one stored receipt, one
## restore candidate) goes to `on_error` when it is a valid Callable, else it is printed.
static func report(on_error: Variant, err: ScfgErr) -> void:
	if on_error is Callable and (on_error as Callable).is_valid():
		(on_error as Callable).call(err)
		return
	var line := "scfg: contained error: %s" % str(err)
	if silence_printed:
		printed.append(line)  # tests only: a game never collects them
	else:
		printerr(line)


## `Kind: message`, `Kind(code): message` or `Kind(status=503): message`.
func _to_string() -> String:
	if code != null:
		return "%s(%s): %s" % [kind, code, message]
	if status != null:
		return "%s(status=%s): %s" % [kind, status, message]
	return "%s: %s" % [kind, message]
