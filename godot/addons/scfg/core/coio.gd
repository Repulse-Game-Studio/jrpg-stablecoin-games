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
##     timeout, TLS), and the message is "invalid JSON response" for a 2xx body that is not JSON.
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

