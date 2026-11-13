## Minimal Ethereum JSON-RPC client with endpoint failover. Mirrors scfg_core/rpc.py, statement
## by statement (the module functions and constants are this class's statics).
##
## Public RPC endpoints are free but unreliable: they rate-limit, go down, and cap eth_getLogs
## block ranges. The client therefore:
##   * tries endpoints in order on every call, skipping ones in cooldown,
##   * puts a failing endpoint in cooldown (30 s, doubling to 10 min),
##   * remembers, per endpoint, the largest eth_getLogs range it accepts (public providers allow
##     anything from 50 to 2000+ blocks), shrinking it when a provider says a range is too large.
## The network is an injected transport (see ScfgCoio); every method that talks to it is a
## coroutine: `var v = await client.block_number()`.
##
## Errors are values (ScfgErr): an RpcError has an int `code`, the node's `message` and
## `data.url` (the endpoint that answered); RpcUnavailable means every endpoint failed or is cooling
## down. A transport ScfgErr that is not a TransportError propagates unchanged.
##
## Differences forced by GDScript:
## - Python's `call` is `call_rpc` (Object.call cannot be overridden);
## - `RpcClient(urls, ...)` raising ValueError is `create(urls, opts)` returning it; `new(urls,
##   opts)` keeps it in `init_error`, and every coroutine then returns that error;
## - keyword arguments are `opts`: `transport`, `log_range` (1000), `clock` (a Callable returning
##   seconds, monotonic by default) and `timeout` (10.0); an unknown key is a TypeError;
## - urls must be a list of str (Array or PackedStringArray of String), `log_range` an int,
##   `timeout` a number and `clock` a Callable, or it is a TypeError (Python would take any
##   iterable and fail later).
class_name ScfgRpcClient
extends RefCounted

const COOLDOWN_START := 30.0
const COOLDOWN_MAX := 600.0
## The smallest eth_getLogs block range (also the smallest max_log_range a chain may declare:
## ScfgConfig.MIN_LOG_RANGE is this constant).
const MIN_LOG_RANGE := 10
const _RATE_HINTS := ["rate limit", "too many requests", "rate-limit"]
## Phrases providers use for "your eth_getLogs block range is too wide". Some say "archive"
## (publicnode answers wide ranges with "Archive requests require a personal token").
const _RANGE_HINTS := ["range", "too many", "limit", "exceed", "max results", "response size", "too large",
	"archive"]
const _OPTS := ["transport", "log_range", "clock", "timeout"]

## The endpoints, in preference order (Python's tuple).
var urls: Array[String] = []
## Python's `clock`: a Callable returning seconds. The Tier 2 runner replaces it.
var clock: Callable
var timeout: float = 10.0
## The transport (see ScfgCoio); null makes every request a RuntimeError, as run_sync does.
var transport: Variant = null
## The construction error (ValueError or TypeError), or null.
var init_error: Variant = null
## {url: largest accepted eth_getLogs range}
var _ranges: Dictionary = {}
## {url: {"delay": float, "until": float}} (Python's _EndpointState)
var _state: Dictionary = {}
var _next_id := 1

## eth_getLogs block range accepted by the currently preferred endpoint.
var log_range: int:
	get:
		if urls.is_empty():
			return 0
		return _ranges[_preferred()]


func _init(urls_: Variant, opts: Dictionary = {}) -> void:
	clock = _monotonic
	var bad: Variant = _check_args(urls_, opts)
	if bad is ScfgErr:
		init_error = bad
		return
	for u: Variant in urls_:
		urls.append(str(u))
	var range_: int = opts.get("log_range", 1000)
	var timeout_: float = opts.get("timeout", 10.0)
	for u in urls:
		_ranges[u] = range_
		_state[u] = {"delay": 0.0, "until": 0.0}
	if opts.has("clock"):
		clock = opts["clock"]
	timeout = timeout_
	transport = opts.get("transport")


## null when `RpcClient(urls, **opts)` would construct, else its TypeError or ValueError.
## @fallible
static func _check_args(urls_: Variant, opts: Dictionary) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in _OPTS):
			return ScfgErr.make("TypeError", "RpcClient() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	if not (urls_ is Array or urls_ is PackedStringArray):
		return ScfgErr.make("TypeError", "urls must be a list of str, not %s" % ScfgPy.py_type(urls_))
	if (urls_ as Array).is_empty() if urls_ is Array else (urls_ as PackedStringArray).is_empty():
		return ScfgErr.make("ValueError", "at least one RPC url required")
	for u: Variant in urls_:
		if not (u is String or u is StringName):
			return ScfgErr.make("TypeError", "an RPC url must be a str, not %s" % ScfgPy.py_type(u))
	var range_: Variant = opts.get("log_range", 1000)
	if typeof(range_) != TYPE_INT:
		return ScfgErr.make("TypeError", "log_range must be an int, not %s" % ScfgPy.py_type(range_))
	var timeout_: Variant = opts.get("timeout", 10.0)
	if typeof(timeout_) != TYPE_INT and typeof(timeout_) != TYPE_FLOAT:
		return ScfgErr.make("TypeError", "timeout must be a number, not %s" % ScfgPy.py_type(timeout_))
	if opts.has("clock") and not opts["clock"] is Callable:
		return ScfgErr.make("TypeError", "clock must be a Callable")
	return null


## Python's `RpcClient(urls, **opts)`: the client, or its ValueError/TypeError.
## @fallible
static func create(urls_: Variant, opts: Dictionary = {}) -> Variant:
	var c := ScfgRpcClient.new(urls_, opts)
	if c.init_error != null:
		return c.init_error
	return c


static func _monotonic() -> float:
	return Time.get_ticks_usec() / 1000000.0


## Python's `RpcError(code, message)`. Always an ScfgErr (`data.url` unset).
## @fallible
static func rpc_error(code: Variant, message: String) -> Variant:
	return ScfgErr.make("RpcError", message, code)


static func is_range_error(err: ScfgErr) -> bool:
	var m := err.message.to_lower()
	return _RANGE_HINTS.any(func(h: String) -> bool: return m.contains(h))


## Next eth_getLogs range to try after `err` rejected a `tried`-block request.
##
## Uses the limit when the provider states one ("limited to 0 - 50 blocks range"), otherwise
## halves. Python's `re.findall(r"\d+")`: runs of any Unicode decimal digits.
static func suggested_range(err: ScfgErr, tried: int) -> int:
	var best := -1
	for n in _numbers(err.message):
		if MIN_LOG_RANGE <= n and n < tried and n > best:
			best = n
	if best >= 0:
		return best
	return maxi(MIN_LOG_RANGE, floori(tried / 2.0) if tried < 0 else tried / 2)


## The numbers `\d+` finds in `s`. A run beyond 18 significant digits is skipped
## (ScfgPy.DIGIT_RUN_LIMIT): it is above every range a caller can try (a range is an int).
## Linear (ScfgPy.digit_run).
static func _numbers(s: String) -> Array[int]:
	var out: Array[int] = []
	var n := s.length()
	var i := 0
	while i < n:
		if ScfgPy.decimal_digit(s.unicode_at(i)) < 0:
			i += 1
			continue
		var run := ScfgPy.digit_run(s, i)
		i = run[0]
		if run[1] != null:
			out.append(run[1])
	return out


## Python's `str(v)` for a value read from JSON: an integral number is the int Python's json
## made of it (`5`, not `5.0`), a str is itself, anything else its repr.
static func _json_str(v: Variant) -> String:
	return ScfgPy.py_str(_json_view(v))


static func _json_view(v: Variant) -> Variant:
	if v is float:
		var f: float = v
		if not is_nan(f) and not is_inf(f) and f == floorf(f):
			var n: Variant = ScfgPy.py_int(f)
			if not n is ScfgErr:
				return n
		return v
	if v is Array:
		var items := []
		for x: Variant in v:
			items.append(_json_view(x))
		return items
	if v is Dictionary:
		var out := {}
		for k: Variant in v:
			out[k] = _json_view(v[k])
		return out
	return v


## Providers are inconsistent: the error may be an object, a bare string, or carry a non-numeric
## code. Never let a malformed error crash the game. Python's int(code) falls back to 0 on a
## TypeError or ValueError; its OverflowError (an infinite code) propagates. Port limit: a code
## beyond 64 bits (Python keeps the big int) is 0, so `code` is always an int.
## @fallible
static func _parse_error(e: Variant) -> Variant:
	var code: Variant
	var message: Variant
	if e is Dictionary:
		code = (e as Dictionary).get("code", 0)
		message = (e as Dictionary).get("message", "")
	else:
		code = 0
		message = e
	var n: Variant = ScfgPy.py_int(code)
	if n is ScfgErr:
		if code is float and is_inf(code as float):
			return n
		n = 0  # TypeError, ValueError (and the port limit's OverflowError below -2^63)
	if not n is int:
		n = 0  # an ScfgBigInt: the port limit above
	return rpc_error(n, _json_str(message))


static func _is_rate_error(err: ScfgErr) -> bool:
	var m := err.message.to_lower()
	return _RATE_HINTS.any(func(h: String) -> bool: return m.contains(h))


## @fallible: a KeyError for a url that is not one of the client's
func _fail(url: String, now: float) -> Variant:
	if not _state.has(url):
		return ScfgErr.make("KeyError", ScfgPy.py_repr(url))
	var st: Dictionary = _state[url]
	st["delay"] = COOLDOWN_START if st["delay"] == 0 else minf(COOLDOWN_MAX, st["delay"] * 2)
	st["until"] = now + st["delay"]
	return null


## @fallible: a KeyError for a url that is not one of the client's
func _ok(url: String) -> Variant:
	if not _state.has(url):
		return ScfgErr.make("KeyError", ScfgPy.py_repr(url))
	var st: Dictionary = _state[url]
	st["delay"] = 0.0
	st["until"] = 0.0
	return null


## The endpoint the next call will try first.
func _preferred() -> String:
	var now: float = clock.call()
	for url in urls:
		if _state[url]["until"] <= now:
			return url
	return urls[0]


## Endpoints not cooling down right now, in preference order.
func live_urls() -> Array:
	var now: float = clock.call()
	var out := []
	for u in urls:
		if _state[u]["until"] <= now:
			out.append(u)
	return out


## The result of `method` from the first endpoint that answers.
## @fallible
func call_rpc(method: String, params: Array) -> Variant:
	var res: Variant = await _call(method, params)
	if res is ScfgErr:
		return res
	return res[0]


## ["ok", result] | ["error", RpcError] | null when the endpoint failed (cooled down).
## @fallible: the transport's non-TransportError errors, a KeyError for a body without `result`
func _attempt(url: String, method: String, params: Array, now: float) -> Variant:
	if init_error != null:
		return init_error
	var payload := {"jsonrpc": "2.0", "id": _next_id, "method": method, "params": params}
	_next_id += 1
	if transport == null:
		return ScfgErr.make("RuntimeError", "no transport to reach %s" % url)
	var resp: Variant = await transport.post_json(url, payload, timeout)
	if resp is ScfgErr:
		if not (resp as ScfgErr).is_a("TransportError"):
			return resp
		var f: Variant = _fail(url, now)
		if f is ScfgErr:
			return f
		return null
	if not resp is Dictionary or (not (resp as Dictionary).has("result") and not (resp as Dictionary).has("error")):
		var f: Variant = _fail(url, now)
		if f is ScfgErr:
			return f
		return null
	var body: Dictionary = resp
	if body.get("error") != null:
		var err: Variant = _parse_error(body["error"])
		if err is ScfgErr and (err as ScfgErr).is_a("RpcError"):
			if _is_rate_error(err):
				var f: Variant = _fail(url, now)
				if f is ScfgErr:
					return f
				return null
			var o: Variant = _ok(url)
			if o is ScfgErr:
				return o
			(err as ScfgErr).data["url"] = url
			return ["error", err]
		return err
	var ok: Variant = _ok(url)
	if ok is ScfgErr:
		return ok
	# Python's resp["result"]: {"error": null} has no result, a KeyError raised after _ok()
	var result: Variant = ScfgPy.py_get(body, "result")
	if result is ScfgErr:
		return result
	return ["ok", result]


## Like call_rpc(), but also returns which endpoint answered: [result, url].
## @fallible
func _call(method: String, params: Array) -> Variant:
	if init_error != null:
		return init_error
	var now: float = clock.call()
	for url in urls:
		if _state[url]["until"] > now:
			continue
		var res: Variant = await _attempt(url, method, params, now)
		if res is ScfgErr:
			return res
		if res == null:
			continue
		if res[0] == "error":
			return res[1]
		return [res[1], url]
	return ScfgErr.make("RpcUnavailable", "all endpoints failed for %s" % method)


## Ask one endpoint only. RpcUnavailable if it fails, RpcError for a node error.
## @fallible
func call_at(url: String, method: String, params: Array) -> Variant:
	if init_error != null:
		return init_error
	var res: Variant = await _attempt(url, method, params, clock.call())
	if res is ScfgErr:
		return res
	if res == null:
		return ScfgErr.make("RpcUnavailable", "%s failed for %s" % [url, method])
	return res[1]  # the result, or the RpcError


## Ask one endpoint: ["ok", result] | ["error", RpcError] | null if it failed.
## @fallible
func try_at(url: String, method: String, params: Array) -> Variant:
	if init_error != null:
		return init_error
	return await _attempt(url, method, params, clock.call())


## Python's `int(result, 16)`: an int (an ScfgBigInt beyond 64 bits), a TypeError for a non-str.
## @fallible
func block_number() -> Variant:
	var res: Variant = await call_rpc("eth_blockNumber", [])
	if res is ScfgErr:
		return res
	return ScfgPy.py_hex_int(res)


## @fallible
func get_transaction_receipt(tx_hash: Variant) -> Variant:
	return await call_rpc("eth_getTransactionReceipt", [tx_hash])


## The block's hash, lowercased, or null for an unknown (falsy) block. Python's
## `block["hash"].lower()`: a missing key is a KeyError, a non-str hash an AttributeError.
## @fallible
func get_block_hash(number: int) -> Variant:
	var block: Variant = await call_rpc("eth_getBlockByNumber", [_hex(number), false])
	if block is ScfgErr:
		return block
	if not ScfgPy.py_truthy(block):
		return null
	var h: Variant = ScfgPy.py_get(block, "hash")
	if h is ScfgErr:
		return h
	if not (h is String or h is StringName):
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'lower'" % ScfgPy.py_type(h))
	return str(h).to_lower()


## Every log of `address` matching `topics` from `from_block` to `to_block` (inclusive), in
## chunks of the preferred endpoint's range; a range error shrinks that endpoint's range and the
## chunk is retried, down to MIN_LOG_RANGE.
## @fallible
func get_logs(address: Variant, topics: Variant, from_block: int, to_block: int) -> Variant:
	if init_error != null:
		return init_error
	var out := []
	var start := from_block
	while start <= to_block:
		var end := mini(to_block, start + log_range - 1)
		var res: Variant = await _call("eth_getLogs", [{
			"address": address, "topics": topics,
			"fromBlock": _hex(start), "toBlock": _hex(end)}])
		if res is ScfgErr:
			var e: ScfgErr = res
			if not e.is_a("RpcError"):
				return e
			# getattr(e, "url", self._preferred()): Python evaluates the default first
			var preferred := _preferred()
			var url: String = e.data.get("url", preferred)
			var tried := mini(_ranges[url], end - start + 1)
			if is_range_error(e) and tried > MIN_LOG_RANGE:
				_ranges[url] = suggested_range(e, tried)
				continue
			return e
		var logs: Variant = res[0]
		# out.extend(logs or [])
		if ScfgPy.py_truthy(logs):
			var items: Variant = ScfgPy.py_iter(logs)
			if items is ScfgErr:
				return items
			out.append_array(items)
		start = end + 1
	return out


## Python's `hex(n)`.
static func _hex(n: int) -> String:
	return "-0x%x" % -n if n < 0 else "0x%x" % n
