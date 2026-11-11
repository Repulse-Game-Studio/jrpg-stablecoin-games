## ScfgRpcClient (rpc.gd): the twin of tests/unit/test_rpc_client.py, plus the strictness on
## malformed bodies that M1 found (rpgmaker/src/core/rpc.ts) and the GDScript-only rules (ints
## never floats in requests, errors as values, coroutines).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const A := "https://a.rpc"
const B := "https://b.rpc"


## [client, transport, clock] for {url: handler}; `opts` adds client options (`clock` replaces
## the FakeClock).
func _client(handlers: Dictionary, opts: Dictionary = {}) -> Array:
	var clock: Variant = opts.get("clock", F.FakeClock.new())
	var t := F.FakeTransport.new(handlers)
	var o := {"transport": t, "clock": clock.fn if clock is F.FakeClock else clock}
	for k: String in opts:
		if k != "clock":
			o[k] = opts[k]
	return [ScfgRpcClient.new(handlers.keys(), o), t, clock]


func _urls(t: F.FakeTransport) -> Array:
	return t.calls.map(func(c: Array) -> String: return c[0])


func _rpc_err(code: int, message: String) -> ScfgErr:
	return ScfgRpcClient.rpc_error(code, message)


func test_requires_urls() -> bool:
	T.is_err(ScfgRpcClient.create([], {}), "ValueError", "create([])")
	var c := ScfgRpcClient.new([], {})
	T.is_err(c.init_error, "ValueError", "new([]) records the error")
	var ok: Variant = ScfgRpcClient.create([A], {"transport": F.FakeTransport.new({})})
	T.ok(ok is ScfgRpcClient, "create([A]) is a client")
	return T.done()


func test_unknown_option_is_a_type_error() -> bool:
	T.is_err(ScfgRpcClient.create([A], {"log_rnage": 5}), "TypeError", "a misspelt keyword")
	T.is_err(ScfgRpcClient.create("https://a.rpc", {}), "TypeError", "urls must be a list of str")
	T.is_err(ScfgRpcClient.create([5], {}), "TypeError", "a url must be a str")
	return T.done()


func test_broken_client_returns_its_init_error() -> bool:
	var c := ScfgRpcClient.new([], {})
	T.is_err(await c.call_rpc("eth_blockNumber", []), "ValueError", "call_rpc")
	T.is_err(await c.get_logs("0x", [], 0, 10), "ValueError", "get_logs")
	T.eq(c.live_urls(), [], "live_urls")
	return T.done()


func test_call_sends_jsonrpc_and_returns_result() -> bool:
	var r := _client({A: F.always(F.ok("0x10"))})
	var c: ScfgRpcClient = r[0]
	var t: F.FakeTransport = r[1]
	T.eq(await c.call_rpc("eth_blockNumber", []), "0x10", "result")
	T.eq(t.calls, [[A, "eth_blockNumber", []]], "calls")
	T.eq(t.payloads[0], {"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber", "params": []}, "payload")
	T.eq(t.payloads[0].keys(), ["jsonrpc", "id", "method", "params"], "payload key order (json.dumps order)")
	T.eq(t.timeouts, [10.0], "default timeout")
	return T.done()


func test_ids_count_from_one_per_attempt() -> bool:
	var r := _client({A: F.down(), B: F.always(F.ok("0x1"))})
	var c: ScfgRpcClient = r[0]
	var t: F.FakeTransport = r[1]
	await c.block_number()  # A fails (id 1), B answers (id 2)
	await c.block_number()  # A cooling down: B (id 3)
	T.eq(t.payloads.map(func(p: Dictionary) -> Variant: return p["id"]), [1, 2, 3], "ids")
	T.eq(typeof(t.payloads[0]["id"]), TYPE_INT, "the id is an int")
	return T.done()


func test_timeout_option_reaches_the_transport() -> bool:
	var r := _client({A: F.always(F.ok(null))}, {"timeout": 2.5})
	await (r[0] as ScfgRpcClient).call_rpc("x", [])
	T.eq((r[1] as F.FakeTransport).timeouts, [2.5], "timeout")
	return T.done()


func test_block_number_parses_hex() -> bool:
	var r := _client({A: F.always(F.ok("0x1b4"))})
	T.eq(await (r[0] as ScfgRpcClient).block_number(), 436, "0x1b4")
	return T.done()


## Python's int(result, 16), case by case (every expectation produced by CPython).
func test_block_number_hex_is_python_int_base_16() -> bool:
	var rows := [
		["0x1b4", 436], ["1b4", 436], [" 0x1b4 ", 436], ["0X1B4", 436], ["0x_1b4", 436], ["-0x5", -5],
		["0x1_b4", 436], ["٣", 3],
		[436.0, "TypeError"], [null, "TypeError"], ["", "ValueError"], ["0x", "ValueError"],
		["0x1__b4", "ValueError"], ["1b4 x", "ValueError"],
	]
	for row: Array in rows:
		var r := _client({A: F.always(F.ok(row[0]))})
		var got: Variant = await (r[0] as ScfgRpcClient).block_number()
		if row[1] is String:
			T.is_err(got, row[1], "block_number of %s" % [row[0]])
		else:
			T.eq(got, row[1], "block_number of %s" % [row[0]])
	var big := _client({A: F.always(F.ok("0x" + "f".repeat(20)))})
	var v: Variant = await (big[0] as ScfgRpcClient).block_number()
	T.ok(v is ScfgBigInt and (v as ScfgBigInt).to_hex() == "f".repeat(20), "a huge hex is an ScfgBigInt (%s)" % [v])
	return T.done()


func test_falls_back_to_next_endpoint_on_transport_error() -> bool:
	var r := _client({A: F.down(), B: F.always(F.ok("0x1"))})
	T.eq(await (r[0] as ScfgRpcClient).block_number(), 1, "B's answer")
	T.eq(_urls(r[1]), [A, B], "A then B")
	return T.done()


func test_failed_endpoint_cools_down_then_is_retried() -> bool:
	var state := {"a_up": false}
	var a := func(_m: String, _p: Array) -> Variant:
		if not state["a_up"]:
			return ScfgErr.make("TransportError", "HTTP 503", null, 503)
		return F.ok("0xa")
	var r := _client({A: a, B: F.always(F.ok("0xb"))})
	var c: ScfgRpcClient = r[0]
	var t: F.FakeTransport = r[1]
	var clock: F.FakeClock = r[2]
	T.eq(await c.block_number(), 0xB, "A fails -> B")
	t.calls.clear()
	T.eq(await c.block_number(), 0xB, "A cooling down")
	T.eq(_urls(t), [B], "A not even tried")
	state["a_up"] = true
	clock.advance(ScfgRpcClient.COOLDOWN_START + 0.1)
	T.eq(await c.block_number(), 0xA, "A preferred again after cooldown")
	return T.done()


func test_cooldown_doubles_and_caps() -> bool:
	var r := _client({A: F.down()})
	var c: ScfgRpcClient = r[0]
	var clock: F.FakeClock = r[2]
	var delays := []
	for i in 8:
		T.is_err(await c.call_rpc("eth_blockNumber", []), "RpcUnavailable", "round %d" % i)
		var st: Dictionary = c._state[A]
		delays.append(st["delay"])
		clock.advance(st["delay"] + 0.1)
	T.eq(delays.slice(0, 3), [ScfgRpcClient.COOLDOWN_START, ScfgRpcClient.COOLDOWN_START * 2,
		ScfgRpcClient.COOLDOWN_START * 4], "doubling")
	T.eq(delays.max(), ScfgRpcClient.COOLDOWN_MAX, "capped")
	T.eq(ScfgRpcClient.COOLDOWN_START, 30.0, "30 s")
	T.eq(ScfgRpcClient.COOLDOWN_MAX, 600.0, "10 min")
	return T.done()


func test_success_resets_cooldown() -> bool:
	var state := {"fail": true}
	var a := func(_m: String, _p: Array) -> Variant:
		if state["fail"]:
			return ScfgErr.make("TransportError", "x")
		return F.ok("0x1")
	var r := _client({A: a})
	var c: ScfgRpcClient = r[0]
	T.is_err(await c.call_rpc("eth_blockNumber", []), "RpcUnavailable", "first")
	state["fail"] = false
	(r[2] as F.FakeClock).advance(ScfgRpcClient.COOLDOWN_START + 1)
	T.eq(await c.call_rpc("eth_blockNumber", []), "0x1", "second")
	T.eq(c._state[A]["delay"], 0.0, "delay reset")
	return T.done()


func test_all_endpoints_down_raises_unavailable() -> bool:
	var r := _client({A: F.down(), B: F.down()})
	var got: Variant = await (r[0] as ScfgRpcClient).block_number()
	T.is_err(got, "RpcUnavailable", "both down")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "all endpoints failed for eth_blockNumber", "message")
	return T.done()


func test_malformed_response_counts_as_endpoint_failure() -> bool:
	for bad: Variant in ["not a dict", {"jsonrpc": "2.0", "id": 1}, null, [1], 5.0]:
		var r := _client({A: F.always(bad), B: F.always(F.ok("0x2"))})
		T.eq(await (r[0] as ScfgRpcClient).block_number(), 2, "%s" % [bad])
		T.eq((r[0] as ScfgRpcClient)._state[A]["delay"], ScfgRpcClient.COOLDOWN_START, "%s cools A down" % [bad])
	return T.done()


func test_jsonrpc_error_is_raised_without_rotation() -> bool:
	var r := _client({A: F.always(F.err(-32000, "execution reverted")), B: F.always(F.ok("0x1"))})
	var got: Variant = await (r[0] as ScfgRpcClient).call_rpc("eth_call", [])
	T.is_err(got, "RpcError", "an RpcError")
	if got is ScfgErr:
		T.eq((got as ScfgErr).code, -32000, "code")
		T.eq((got as ScfgErr).message, "execution reverted", "message")
		T.eq((got as ScfgErr).data, {"url": A}, "the url of the endpoint that answered")
	T.eq(_urls(r[1]), [A], "no rotation")
	T.eq((r[0] as ScfgRpcClient)._state[A]["delay"], 0.0, "an answering endpoint is healthy")
	return T.done()


func test_rate_limit_jsonrpc_error_rotates() -> bool:
	var r := _client({A: F.always(F.err(-32005, "Too Many Requests: rate limit")), B: F.always(F.ok("0x3"))})
	T.eq(await (r[0] as ScfgRpcClient).block_number(), 3, "B")
	T.eq((r[0] as ScfgRpcClient)._state[A]["delay"], ScfgRpcClient.COOLDOWN_START, "A cooled down")
	return T.done()


func test_is_range_error() -> bool:
	T.ok(ScfgRpcClient.is_range_error(_rpc_err(-32600, "eth_getLogs block range is too large")), "too large")
	T.ok(ScfgRpcClient.is_range_error(_rpc_err(-32005, "query returned more than 10000 results; limit exceeded")),
		"limit exceeded")
	T.ok(not ScfgRpcClient.is_range_error(_rpc_err(-32000, "execution reverted")), "reverted")
	return T.done()


func test_receipt_and_block_hash() -> bool:
	var h := func(m: String, p: Array) -> Variant:
		if m == "eth_getTransactionReceipt":
			return F.ok(null if p[0] == "0xmissing" else {"status": "0x1"})
		if m == "eth_getBlockByNumber":
			if p != ["0x64", false]:
				return ScfgErr.make("AssertionError", "params %s" % [p])
			return F.ok({"hash": "0xABC"})
		return null
	var c: ScfgRpcClient = _client({A: h})[0]
	T.eq(await c.get_transaction_receipt("0xmissing"), null, "missing receipt")
	T.eq(await c.get_transaction_receipt("0xok"), {"status": "0x1"}, "receipt")
	T.eq(await c.get_block_hash(100), "0xabc", "lowercased hash")
	return T.done()


func test_block_hash_none_for_unknown_block() -> bool:
	var r := _client({A: F.always(F.ok(null))})
	T.eq(await (r[0] as ScfgRpcClient).get_block_hash(1000000000), null, "unknown block")
	T.eq((r[1] as F.FakeTransport).calls[0][2], ["0x3b9aca00", false], "hex(10**9)")
	return T.done()


## `block["hash"].lower() if block else None`, case by case (CPython's outcomes).
func test_block_hash_truthiness_and_lookups() -> bool:
	var rows := [
		[null, null], [{}, null], [[], null], ["", null], [0.0, null], [false, null],
		[{"hash": "0xAB"}, "0xab"],
		[{"hash": 5.0}, "AttributeError"], [{"x": 1.0}, "KeyError"], [["a"], "TypeError"],
		["abc", "TypeError"], [5.0, "TypeError"], [true, "TypeError"], [{"hash": null}, "AttributeError"],
	]
	for row: Array in rows:
		var c: ScfgRpcClient = _client({A: F.always(F.ok(row[0]))})[0]
		var got: Variant = await c.get_block_hash(5)
		if row[1] is String and not (row[1] as String).begins_with("0x"):
			T.is_err(got, row[1], "block %s" % [row[0]])
		else:
			T.eq(got, row[1], "block %s" % [row[0]])
	return T.done()


## A body like {"error": null} has no result: Python's resp["result"] is a KeyError raised
## after _ok(url), so it propagates (no rotation, the endpoint stays healthy).
func test_error_null_without_result_is_a_key_error_after_ok() -> bool:
	var r := _client({A: F.always({"error": null}), B: F.always(F.ok("0x1"))})
	var c: ScfgRpcClient = r[0]
	(c._state[A] as Dictionary)["delay"] = 30.0  # _ok() must reset it
	var got: Variant = await c.call_rpc("x", [])
	T.is_err(got, "KeyError", "KeyError")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "'result'", "the key's repr")
	T.eq(_urls(r[1]), [A], "no rotation")
	T.eq(c._state[A]["delay"], 0.0, "_ok ran first")
	var r2 := _client({A: F.always({"error": null, "result": 5.0})})
	T.eq(await (r2[0] as ScfgRpcClient).call_rpc("x", []), 5.0, "error null with a result")
	return T.done()


## A transport failure that is not a TransportError (the Tier 2 replay's AssertionError, a fake's
## KeyError) is not an endpoint failure: Python's `except TransportError` lets it propagate.
func test_non_transport_errors_propagate() -> bool:
	var boom := F.always(ScfgErr.make("AssertionError", "unexpected call"))
	var r := _client({A: boom, B: F.always(F.ok("0x1"))})
	var c: ScfgRpcClient = r[0]
	T.is_err(await c.call_rpc("x", []), "AssertionError", "call_rpc")
	T.eq(_urls(r[1]), [A], "no rotation")
	T.eq(c._state[A]["delay"], 0.0, "not cooled down")
	T.is_err(await c.call_at(A, "x", []), "AssertionError", "call_at")
	T.is_err(await c.try_at(A, "x", []), "AssertionError", "try_at")
	T.is_err(await c.get_logs("0x", [], 0, 5), "AssertionError", "get_logs")
	return T.done()


func test_no_transport_is_a_runtime_error() -> bool:
	var c := ScfgRpcClient.new([A], {})
	var got: Variant = await c.call_rpc("x", [])
	T.is_err(got, "RuntimeError", "no transport")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "no transport to reach https://a.rpc", "message")
	return T.done()


func test_call_at_and_try_at() -> bool:
	var r := _client({A: F.down(), B: F.always(F.err(-1, "boom"))})
	var c: ScfgRpcClient = r[0]
	var got: Variant = await c.call_at(A, "getSlot", [])
	T.is_err(got, "RpcUnavailable", "call_at on a dead endpoint")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "https://a.rpc failed for getSlot", "message")
	T.eq(await c.try_at(A, "getSlot", []), null, "try_at on a dead endpoint")
	got = await c.call_at(B, "getSlot", [])
	T.is_err(got, "RpcError", "call_at: node error")
	var res: Variant = await c.try_at(B, "getSlot", [])
	T.ok(res is Array and res.size() == 2 and res[0] == "error" and res[1] is ScfgErr
		and (res[1] as ScfgErr).is_a("RpcError") and (res[1] as ScfgErr).code == -1, "try_at: [\"error\", RpcError] (%s)" % [res])
	var ok := _client({A: F.always(F.ok(7.0))})
	T.eq(await (ok[0] as ScfgRpcClient).try_at(A, "getSlot", []), ["ok", 7.0], "try_at ok")
	T.eq(await (ok[0] as ScfgRpcClient).call_at(A, "getSlot", []), 7.0, "call_at ok")
	T.is_err(await (ok[0] as ScfgRpcClient).call_at("https://other", "getSlot", []), "KeyError",
		"call_at of an unknown url (the fake has no handler)")
	return T.done()


# --- eth_getLogs chunking ---------------------------------------------------


func _logs_handler(record: Array, max_span: int = -1) -> Callable:
	return func(_m: String, p: Array) -> Variant:
		var f: Dictionary = p[0]
		var lo: int = ScfgPy.py_hex_int(f["fromBlock"])  # err-ok: test input
		var hi: int = ScfgPy.py_hex_int(f["toBlock"])  # err-ok: test input
		record.append([lo, hi])
		if max_span >= 0 and hi - lo + 1 > max_span:
			return F.err(-32600, "block range too large")
		return F.ok([{"blockNumber": "0x%x" % lo}])


func test_get_logs_chunks_by_log_range() -> bool:
	var seen := []
	var c: ScfgRpcClient = _client({A: _logs_handler(seen)}, {"log_range": 100})[0]
	var logs: Variant = await c.get_logs("0xtoken", ["0xtopic"], 1000, 1250)
	T.eq(seen, [[1000, 1099], [1100, 1199], [1200, 1250]], "chunks")
	T.eq((logs as Array).size() if logs is Array else -1, 3, "logs")
	return T.done()


func test_get_logs_empty_range_makes_no_calls() -> bool:
	var seen := []
	var c: ScfgRpcClient = _client({A: _logs_handler(seen)})[0]
	T.eq(await c.get_logs("0xtoken", [], 10, 9), [], "no logs")
	T.eq(seen, [], "no calls")
	return T.done()


func test_get_logs_halves_range_on_range_error_and_remembers() -> bool:
	var seen := []
	var c: ScfgRpcClient = _client({A: _logs_handler(seen, 300)}, {"log_range": 1000})[0]
	await c.get_logs("0xtoken", [], 0, 499)
	T.eq(c.log_range, 250, "remembered")
	T.eq(seen, [[0, 499], [0, 249], [250, 499]], "the 500-block request is retried as 250")
	return T.done()


func test_get_logs_gives_up_at_minimum_range() -> bool:
	var seen := []
	var c: ScfgRpcClient = _client({A: _logs_handler(seen, 1)}, {"log_range": 40})[0]
	T.is_err(await c.get_logs("0xtoken", [], 0, 100), "RpcError", "gives up")
	T.eq(c.log_range, ScfgRpcClient.MIN_LOG_RANGE, "at the minimum")
	return T.done()


func test_get_logs_sends_filter_shape() -> bool:
	var calls := []
	var h := func(_m: String, p: Array) -> Variant:
		calls.append(p)
		return F.ok([])
	var c: ScfgRpcClient = _client({A: h}, {"log_range": 1000})[0]
	await c.get_logs("0xToken", ["0xt0", null, "0xt2"], 16, 31)
	T.eq(calls, [[{"address": "0xToken", "topics": ["0xt0", null, "0xt2"], "fromBlock": "0x10", "toBlock": "0x1f"}]],
		"filter")
	T.eq((calls[0][0] as Dictionary).keys(), ["address", "topics", "fromBlock", "toBlock"], "key order")
	return T.done()


## `out.extend(logs or [])`: a dict extends with its keys, a str with its characters, a number is
## a TypeError (CPython's outcomes).
func test_get_logs_extends_like_python() -> bool:
	var rows := [[null, []], [[], []], [{"a": 1.0}, ["a"]], ["ab", ["a", "b"]], [5.0, "TypeError"],
		[[1.0, 2.0], [1.0, 2.0]], [0.0, []], [true, "TypeError"]]
	for row: Array in rows:
		var c: ScfgRpcClient = _client({A: F.always(F.ok(row[0]))})[0]
		var got: Variant = await c.get_logs("0x", [], 0, 5)
		if row[1] is String:
			T.is_err(got, row[1], "logs %s" % [row[0]])
		else:
			T.eq(got, row[1], "logs %s" % [row[0]])
	return T.done()


func test_log_range_is_remembered_per_endpoint() -> bool:
	var state := {"a_down": false}
	var seen := []
	var plain := _logs_handler(seen)
	var a := func(m: String, p: Array) -> Variant:
		if state["a_down"]:
			return ScfgErr.make("TransportError", "down")
		return plain.call(m, p)
	var r := _client({A: a, B: _logs_handler(seen, 50)}, {"log_range": 1000})
	var c: ScfgRpcClient = r[0]
	await c.get_logs("0xtoken", [], 0, 999)
	T.eq(c.log_range, 1000, "A's range")
	state["a_down"] = true
	await c.get_logs("0xtoken", [], 0, 199)  # served by B, which only allows 50
	T.ok(c._ranges[B] < 1000 and c.log_range == c._ranges[B], "B's range (%s)" % [c._ranges])
	state["a_down"] = false
	(r[2] as F.FakeClock).advance(ScfgRpcClient.COOLDOWN_START + 1)
	T.eq(c.log_range, 1000, "A is preferred again, with its own range")
	return T.done()


func test_suggested_range_uses_stated_limit_or_halves() -> bool:
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32602, "eth_getLogs is limited to 0 - 50 blocks range"), 2000), 50, "stated")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(35, "ranges over 10000 blocks are not supported on free plan"), 2000),
		1000, "too big a number: halve")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32602, "Archive requests require a personal token"), 500), 250, "halve")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32005, "block range is too large"), 15), ScfgRpcClient.MIN_LOG_RANGE,
		"floor")
	return T.done()


## Python's `\d+` reads any Unicode decimal digit, int() of leading zeros is fine, and `//`
## floors (CPython's outcomes).
func test_suggested_range_reads_like_python() -> bool:
	var rows := [["max ٥٠ blocks", 2000, 50], ["0050", 100, 50], ["x", 7, 10], ["x", -7, 10], ["x", 21, 10],
		["123456789012345678901234 and 40", 100, 40], ["x", 101, 50], ["10 or 11 or 12", 12, 11]]
	for row: Array in rows:
		T.eq(ScfgRpcClient.suggested_range(_rpc_err(0, row[0]), row[1]), row[2], "%s / %d" % [row[0], row[1]])
	T.eq(ScfgRpcClient.MIN_LOG_RANGE, 10, "rpc.py's MIN_LOG_RANGE")
	return T.done()


# tests/unit/test_rpc_client.py::test_suggested_range_ignores_numbers_beyond_18_digits (Ruling 6): a
# run beyond 18 significant digits is no number.
func test_suggested_range_ignores_numbers_beyond_18_digits() -> bool:
	var rows := [["limited to 50 blocks; request " + "9".repeat(4301), 2000, 50],
		["block range too large: " + "9".repeat(4301), 5000, 2500], ["max " + "0".repeat(30) + "50 blocks", 2000, 50],
		["max " + "9".repeat(19) + " or 50", 2000, 50], ["max " + "٠".repeat(25) + "٥٠", 2000, 50],
		["max ５０", 2000, 50], ["max ५० or ৫০", 2000, 50]]
	for row: Array in rows:
		T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32005, row[0]), row[1]), row[2], (row[0] as String).substr(0, 40))
	return T.done()


# tests/unit/test_rpc_client.py::test_suggested_range_reads_18_significant_digits_not_19
func test_suggested_range_reads_18_significant_digits_not_19() -> bool:
	var tried := 9 * 1000000000000000000
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(0, "max " + "9".repeat(18)), tried), 999999999999999999, "18 digits")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(0, "max 00000" + "9".repeat(18)), tried), 999999999999999999,
		"18 significant digits")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(0, "max 1" + "0".repeat(18)), tried), tried / 2, "19 digits: halve")
	return T.done()


## A long digit run is read in linear time (no String appends: they are not amortized in GDScript).
func test_a_long_digit_run_is_read_in_linear_time() -> bool:
	var start := Time.get_ticks_msec()
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32005, "too large: " + "9".repeat(200000)), 5000), 2500, "digits")
	T.eq(ScfgRpcClient.suggested_range(_rpc_err(-32005, "max " + "0".repeat(200000) + "50"), 5000), 50, "zeros")
	T.ok(Time.get_ticks_msec() - start < 2000, "200,000-digit runs are read at once")
	return T.done()


func test_archive_message_counts_as_range_error() -> bool:
	T.ok(ScfgRpcClient.is_range_error(_rpc_err(-32602, "Archive requests require a personal token. Get one at x")),
		"archive")
	return T.done()


func test_stated_limit_is_adopted_in_one_step() -> bool:
	var seen := []
	var h := func(_m: String, p: Array) -> Variant:
		var f: Dictionary = p[0]
		var lo: int = ScfgPy.py_hex_int(f["fromBlock"])  # err-ok: test input
		var hi: int = ScfgPy.py_hex_int(f["toBlock"])  # err-ok: test input
		seen.append([lo, hi])
		if hi - lo + 1 > 50:
			return F.err(-32602, "eth_getLogs is limited to 0 - 50 blocks range")
		return F.ok([])
	var c: ScfgRpcClient = _client({A: h}, {"log_range": 2000})[0]
	await c.get_logs("0xtoken", [], 0, 99)
	T.eq(seen, [[0, 99], [0, 49], [50, 99]], "one step")
	T.eq(c.log_range, 50, "adopted")
	return T.done()


func test_malformed_error_objects_become_rpc_errors() -> bool:
	# [error, code, message]: the Python test's three rows, then CPython's outcomes for more forms
	# (numbers arrive from JSON as floats: a code of -32000.0 is the JSON -32000).
	var rows := [
		["query timeout", 0, "query timeout"], [{"code": "bad", "message": "odd"}, 0, "odd"],
		[{"message": "no code"}, 0, "no code"],
		[{"code": "12", "message": 5.0}, 12, "5"], [{"code": 3.7}, 3, ""], [{"code": null, "message": null}, 0, "None"],
		[{"code": true}, 1, ""], [5.0, 0, "5"], [["a"], 0, "['a']"], [{"code": " 1_0 "}, 10, ""],
		[{"code": "0x10"}, 0, ""], [{}, 0, ""], [{"message": {"a": "range"}}, 0, "{'a': 'range'}"],
		[false, 0, "False"], [{"code": "٣"}, 3, ""], [{"code": -32000.0, "message": "x"}, -32000, "x"],
		[{"code": [1]}, 0, ""], [{"message": 2.5}, 0, "2.5"],
		# port limit (Python keeps the big int): a code beyond 64 bits is 0, so `code` stays an int
		[{"code": "99999999999999999999"}, 0, ""], [{"code": "-99999999999999999999"}, 0, ""],
	]
	for row: Array in rows:
		var c: ScfgRpcClient = _client({A: F.always({"jsonrpc": "2.0", "id": 1, "error": row[0]})})[0]
		var got: Variant = await c.call_rpc("eth_blockNumber", [])
		if T.is_err(got, "RpcError", "error %s" % [row[0]]):
			T.eq([(got as ScfgErr).code, (got as ScfgErr).message], [row[1], row[2]], "error %s" % [row[0]])
	return T.done()


## Review Focus 2: an int parameter must be an int in the payload, never a float. JSON.stringify
## writes 5.0 as "5" on 4.3 and as "5.0" on 4.7, so the check walks the Variant types (holds on
## both versions) as well as the text (catches it on 4.7).
func test_requests_carry_ints_not_floats() -> bool:
	var h := func(m: String, p: Array) -> Variant:
		if m == "eth_getLogs":
			var f: Dictionary = p[0]
			var span: int = ScfgPy.py_hex_int(f["toBlock"]) - ScfgPy.py_hex_int(f["fromBlock"]) + 1  # err-ok: test input
			return F.err(-32602, "limited to 0 - 50 blocks range") if span > 50 else F.ok([])
		if m == "eth_blockNumber":
			return F.ok("0x10")
		return F.ok({"hash": "0xab"})
	var r := _client({A: h}, {"log_range": 2000})
	var c: ScfgRpcClient = r[0]
	var t: F.FakeTransport = r[1]
	await c.block_number()
	await c.get_block_hash(255)
	await c.get_logs("0xToken", ["0xt0"], 0, 120)
	await c.call_at(A, "eth_blockNumber", [])
	await c.try_at(A, "eth_blockNumber", [])
	T.ok(t.payloads.size() >= 7, "requests were made (%d)" % t.payloads.size())
	for p: Dictionary in t.payloads:
		T.eq(find_float(p), "", "no float in %s" % [p])
		T.eq(fractional_number(JSON.stringify(p)), "", "no fractional number in %s" % JSON.stringify(p))
	T.eq(fractional_number("{\"jsonrpc\":\"2.0\",\"id\":5.0}"), ":5.0", "the text check sees 5.0 (as 4.7 writes it)")
	return T.done()


## The first JSON number with a fraction or exponent in `json` (outside strings), or "".
static func fractional_number(json: String) -> String:
	var re := RegEx.new()
	re.compile("\"(?:[^\"\\\\]|\\\\.)*\"|[:\\[,]\\s*-?\\d+(?:\\.\\d+|[eE][-+]?\\d+)")
	for m: RegExMatch in re.search_all(json):
		if not m.get_string().begins_with("\""):
			return m.get_string()
	return ""


## The path of the first float inside `v` ("" when there is none).
static func find_float(v: Variant, path: String = "$") -> String:
	if typeof(v) == TYPE_FLOAT:
		return path
	if v is Array:
		for i in (v as Array).size():
			var p := find_float(v[i], "%s[%d]" % [path, i])
			if p != "":
				return p
	elif v is Dictionary:
		for k: Variant in v:
			var p := find_float(v[k], "%s.%s" % [path, k])
			if p != "":
				return p
	return ""


## A transport that suspends for a few frames (as the real one does) is awaited through every
## layer: the results and the failover are the same.
func test_awaits_a_transport_that_yields_frames() -> bool:
	var r := _client({A: F.down(), B: F.always(F.ok("0x2a"))})
	(r[1] as F.FakeTransport).frames = 2
	var start := Engine.get_process_frames()
	T.eq(await (r[0] as ScfgRpcClient).block_number(), 42, "B's answer after suspending")
	T.ok(Engine.get_process_frames() - start >= 4, "two requests, two frames each (%d)" % (Engine.get_process_frames() - start))
	return T.done()


## config.py imports rpc.MIN_LOG_RANGE: ScfgConfig must read the one constant, not a copy.
func test_config_uses_the_rpc_min_log_range() -> bool:
	T.eq(ScfgConfig.MIN_LOG_RANGE, ScfgRpcClient.MIN_LOG_RANGE, "the same value")
	var src := FileAccess.get_file_as_string("res://addons/scfg/core/config.gd")
	var re := RegEx.new()
	re.compile("(?m)^\\s*(?:static\\s+var|var|const)\\s+MIN_LOG_RANGE\\s*:?=\\s*(.*)$")
	var m := re.search(src)
	T.ok(m != null and m.get_string(1).strip_edges().begins_with("ScfgRpcClient.MIN_LOG_RANGE"),
		"config.gd's MIN_LOG_RANGE is ScfgRpcClient.MIN_LOG_RANGE (%s)" % [m.get_string(0) if m else "missing"])
	return T.done()
