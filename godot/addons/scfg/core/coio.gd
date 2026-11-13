## Network I/O for the payment core: the transport contract. Mirrors the transport half of
## scfg_core/coio.py (TransportError) and rpc.UrllibTransport's response rules.
##
## The core never performs a network request itself. Python's cooperative generators (`yield
## Request`, driven by run_sync or the PumpWorker) become GDScript coroutines here: code that may
## need the network awaits an injected transport, and every caller of such code awaits it too.
##
## THE TRANSPORT CONTRACT. A transport is any RefCounted with
##
##     func post_json(url: String, payload: Dictionary, timeout: float) -> Variant
##
## a coroutine (it may also return at once, as a test fake does) that POSTs `payload` as JSON to
## `url`, waiting at most `timeout` seconds, and returns:
##   - the parsed response body (a Dictionary, an Array, a String, a float, a bool or null, as
##     JSON.parse_string makes them: every number is a float) for a 2xx answer with a JSON body;
##   - the parsed body too for a non-2xx answer whose body is a JSON object with a truthy
##     `error` (providers send JSON-RPC errors such as "range too large" with a 4xx status);
##   - an ScfgErr of kind TransportError otherwise: `status` is the HTTP status for any other
##     non-2xx answer (message `HTTP <status>`), null when no answer arrived (connection,
##     timeout, TLS), and the message is "invalid JSON response" for a 2xx body that is not JSON
##     (as Python's json reads it with ints.int_from: an integer literal of more than 4300 digits
##     is not JSON either; parse_rpc_body).
## Any other ScfgErr kind it returns is not an endpoint failure: ScfgRpcClient lets it propagate,
## as Python's `except TransportError` does (the Tier 2 replay transport returns AssertionError
## for an unexpected request).
##
## Extending Transport below is optional (the contract is duck-typed); it documents the method
## and gives fakes and ScfgHttpTransport a common base. ScfgHttpTransport is the real one.
##
## Not ported: run_sync, Sync and synced (await replaces them), and response_from_envelope (Ren'Py's
## web build reaches fetch() through bridge.js; Godot's HTTPRequest uses fetch itself in a web
## export, so ScfgHttpTransport applies response_for directly).
class_name ScfgCoio
extends RefCounted


## The base of transports (see the header). Subclasses override post_json.
class Transport:
	extends RefCounted

	## POST `payload` as JSON to `url`. Returns the parsed response or an ScfgErr (see ScfgCoio).
	## A coroutine in every real transport: callers `await` it.
	## @fallible
	## @coroutine
	func post_json(url: String, _payload: Dictionary, _timeout: float) -> Variant:
		return ScfgErr.make("RuntimeError", "no transport to reach %s" % url)


## Python's `TransportError(message, status=status)`. Always an ScfgErr.
## @fallible
static func transport_error(message: String, status: Variant = null) -> Variant:
	return ScfgErr.make("TransportError", message, null, status)


## The JSON value of a response body as Python's `json.loads(raw.decode("utf-8"))` reads it:
## `[true, value]`, or `[false, null]` when the bytes are not UTF-8 or not JSON. Strict UTF-8 is
## checked first (Python's decoder; Godot's would print an error and substitute U+FFFD), and a
## BOM or a NUL is not JSON (json.loads raises on both).
static func parse_body(raw: PackedByteArray) -> Array:
	if not is_utf8(raw) or raw.has(0):
		return [false, null]
	if raw.size() >= 3 and raw[0] == 0xef and raw[1] == 0xbb and raw[2] == 0xbf:
		return [false, null]
	var j := JSON.new()
	if j.parse(raw.get_string_from_utf8()) != OK:
		return [false, null]
	return [true, j.data]


## parse_body for an RPC answer, as the Python core reads one (ints.json_loads): an integer
## literal of more than 4300 digits makes the body unreadable, as CPython's json refuses it, where
## Godot's JSON would read it as inf. Linear: a native regex anchored by a lookbehind (unanchored,
## it would retry at every digit) finds whether such a run exists before the bytes are scanned for
## one outside strings. Port difference: a 309-4300 digit integer is still Godot's float (inf)
## where Python reads an int; no honest node sends one (see conformance/README.md).
static func parse_rpc_body(raw: PackedByteArray) -> Array:
	# checked before JSON parses it (Godot's parser warns "Exponent too high" on such a number);
	# the regex reads the bytes as Latin-1, which keeps every ASCII digit and never fails
	if (raw.size() > ScfgPy.INT_MAX_STR_DIGITS
			and _long_digit_run().search(raw.get_string_from_ascii()) != null and _has_overlong_int(raw)):
		return [false, null]
	return parse_body(raw)


static var _long_run: RegEx = null


static func _long_digit_run() -> RegEx:
	if _long_run == null:
		_long_run = RegEx.create_from_string("(?<![0-9])[0-9]{%d}" % (ScfgPy.INT_MAX_STR_DIGITS + 1))
	return _long_run


## Whether the JSON bytes hold an integer literal of more than 4300 digits outside strings.
static func _has_overlong_int(b: PackedByteArray) -> bool:
	var n := b.size()
	var i := 0
	while i < n:
		var c := b[i]
		if c == 0x22:  # a string: to its closing quote, past escapes
			i += 1
			while i < n and b[i] != 0x22:
				i += 2 if b[i] == 0x5c else 1
			i += 1
		elif c >= 0x30 and c <= 0x39:
			var start := i
			while i < n and b[i] >= 0x30 and b[i] <= 0x39:
				i += 1
			if i < n and (b[i] == 0x2e or b[i] == 0x65 or b[i] == 0x45):  # . e E: a float
				i += 1
				while i < n and ((b[i] >= 0x30 and b[i] <= 0x39) or b[i] == 0x2b or b[i] == 0x2d
						or b[i] == 0x65 or b[i] == 0x45):
					i += 1
			elif i - start > ScfgPy.INT_MAX_STR_DIGITS:
				return true
		else:
			i += 1
	return false


## Strict UTF-8 (CPython's decoder): no overlong forms, no surrogates, nothing above U+10FFFF.
static func is_utf8(b: PackedByteArray) -> bool:
	var i := 0
	var n := b.size()
	while i < n:
		var c := b[i]
		if c < 0x80:
			i += 1
			continue
		var need := 0
		var lo := 0x80
		var hi := 0xbf
		if c >= 0xc2 and c <= 0xdf:
			need = 1
		elif c >= 0xe0 and c <= 0xef:
			need = 2
			if c == 0xe0:
				lo = 0xa0
			elif c == 0xed:
				hi = 0x9f
		elif c >= 0xf0 and c <= 0xf4:
			need = 3
			if c == 0xf0:
				lo = 0x90
			elif c == 0xf4:
				hi = 0x8f
		else:
			return false
		if i + need >= n:
			return false
		for k in range(1, need + 1):
			var cc := b[i + k]
			if k == 1:
				if cc < lo or cc > hi:
					return false
			elif cc < 0x80 or cc > 0xbf:
				return false
		i += need + 1
	return true


## The response for an HTTP answer, by rpc.UrllibTransport's rules (see the header). `parsed`
## is parse_body's `[readable, value]`.
## @fallible
static func response_for(status: int, parsed: Array) -> Variant:
	var readable: bool = parsed[0]
	var body: Variant = parsed[1]
	if status < 200 or status >= 300:
		if readable and body is Dictionary and ScfgPy.py_truthy((body as Dictionary).get("error")):
			return body
		return transport_error("HTTP %d" % status, status)
	if not readable:
		return transport_error("invalid JSON response")
	return body

