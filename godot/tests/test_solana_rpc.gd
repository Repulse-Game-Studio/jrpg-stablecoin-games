## ScfgSolanaRpc (solana/rpc.gd): the twin of tests/unit/test_solana_rpc.py, plus Python's
## outcomes on malformed answers (py_get lookups: a missing key is a KeyError) and the
## version-negotiation details.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const A := "https://a.example"
const B := "https://b.example"

const _V2 := ("Transaction version (2) is not supported by the requesting client. Please try the "
	+ "request again with the following configuration parameter: \"maxSupportedTransactionVersion\": 2")


## [rpc, transport] for {url: handler}, with a clock that always reads 0.
func _client(handlers: Dictionary) -> Array:
	var t := F.FakeTransport.new(handlers)
	var c := ScfgRpcClient.new(handlers.keys(), {"transport": t, "clock": func() -> float: return 0.0})
	return [ScfgSolanaRpc.new(c), t]


func _version_of(call: Array) -> Variant:
	return (call[2] as Array)[1]["maxSupportedTransactionVersion"]


func test_get_transaction_asks_for_newest_version_and_json() -> bool:
	var r := _client({A: F.always(F.ok({"slot": 5.0}))})
	var rpc: ScfgSolanaRpc = r[0]
	T.eq(await rpc.get_transaction("SIG"), {"slot": 5.0}, "the transaction")
	var call: Array = (r[1] as F.FakeTransport).calls[0]
	T.eq(call.slice(1), ["getTransaction", ["SIG", {"encoding": "json", "commitment": "confirmed",
		"maxSupportedTransactionVersion": ScfgSolanaRpc.TX_VERSION}]], "params")
	T.eq(ScfgSolanaRpc.TX_VERSION, 1, "TX_VERSION")
	return T.done()


func test_get_transaction_lowers_version_when_the_node_rejects_the_parameter() -> bool:
	var handler := func(_m: String, p: Array) -> Variant:
		if p[1]["maxSupportedTransactionVersion"] == 1:
			return F.err(-32602, "Invalid params: unknown maxSupportedTransactionVersion 1")
		return F.ok({"slot": 5.0})
	var rpc: ScfgSolanaRpc = _client({A: handler})[0]
	T.eq(await rpc.get_transaction("SIG"), {"slot": 5.0}, "the transaction")
	T.eq(rpc.tx_version, 0, "lowered")
	return T.done()


## Lowering stops at 0: the -32602 is then raised (CPython: 2 calls, tx_version 0).
func test_get_transaction_lowering_stops_at_zero() -> bool:
	var r := _client({A: F.always(F.err(-32602, "unknown version"))})
	var rpc: ScfgSolanaRpc = r[0]
	T.is_err(await rpc.get_transaction("SIG"), "RpcError", "raised")
	T.eq(rpc.tx_version, 0, "at 0")
	T.eq((r[1] as F.FakeTransport).calls.size(), 2, "two calls")
	return T.done()


func test_try_at_each_live_url_reports_per_endpoint_and_dead_ones_cool_down() -> bool:
	var r := _client({A: F.down(), B: F.always(F.err(-1, "boom"))})
	var c: ScfgRpcClient = (r[0] as ScfgSolanaRpc).client
	var out := []
	for url: String in c.live_urls():
		var res: Variant = await c.try_at(url, "getSlot", [])
		if res != null:
			out.append([url, res])
	T.ok(out.size() == 1 and out[0][0] == B, "only B answered (%s)" % [out])
	if out.size() == 1:
		var kind: Variant = out[0][1][0]
		var value: Variant = out[0][1][1]
		T.ok(kind == "error" and value is ScfgErr and (value as ScfgErr).is_a("RpcError"), "an RpcError")
	T.eq(c.live_urls(), [B], "A cools down")
	return T.done()


func _history(first_a: Variant, first_b: Variant, tx_b: Variant = null) -> Dictionary:
	var make := func(first: Variant, tx: Variant) -> Callable:
		return func(m: String, _p: Array) -> Variant:
			if m == "getFirstAvailableBlock":
				return F.ok(first)
			return F.ok(tx)
	return {A: make.call(first_a, null), B: make.call(first_b, tx_b)}


func test_get_transaction_any_returns_first_found() -> bool:
	var rpc: ScfgSolanaRpc = _client(_history(1000000000.0, 0.0, {"slot": 50.0}))[0]
	T.eq(await rpc.get_transaction_any("SIG", {"min_slot": 50}), [{"slot": 50.0}, false], "found on B")
	return T.done()


func test_null_from_full_history_endpoint_is_authoritative() -> bool:
	var rpc: ScfgSolanaRpc = _client(_history(1000000000.0, 0.0))[0]
	T.eq(await rpc.get_transaction_any("SIG", {"min_slot": 50}), [null, true], "authoritative")
	return T.done()


func test_null_only_from_pruned_endpoints_is_not_authoritative() -> bool:
	var rpc: ScfgSolanaRpc = _client(_history(1000000000.0, 1000000000.0))[0]
	T.eq(await rpc.get_transaction_any("SIG", {"min_slot": 50}), [null, false], "not authoritative")
	return T.done()


func test_get_transaction_any_without_answers_raises() -> bool:
	var rpc: ScfgSolanaRpc = _client({A: F.down(), B: F.down()})[0]
	var got: Variant = await rpc.get_transaction_any("SIG", {"min_slot": 1})
	T.is_err(got, "RpcUnavailable", "no answers")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "no endpoint answered getTransaction for SIG", "message")
	return T.done()


func test_get_transaction_any_options() -> bool:
	var r := _client({A: F.always(F.ok(null))})
	var rpc: ScfgSolanaRpc = r[0]
	T.eq(await rpc.get_transaction_any("S"), [null, false], "no min_slot: never authoritative")
	var t: F.FakeTransport = r[1]
	T.eq(t.calls.map(func(c: Array) -> String: return c[1]), ["getTransaction"], "no history lookup without min_slot")
	T.eq((t.calls[0][2] as Array)[1]["commitment"], "finalized", "finalized by default")
	T.is_err(await rpc.get_transaction_any("S", {"min_slto": 1}), "TypeError", "a misspelt keyword")
	return T.done()


## _first_available caches only a readable answer: a non-int is _UNKNOWN_HISTORY (pruned) and is
## asked again next time (CPython: the call order below).
func test_first_available_is_cached_only_when_readable() -> bool:
	var calls := []
	var answers := {"first": "bad"}
	var h := func(m: String, _p: Array) -> Variant:
		calls.append(m)
		if m == "getFirstAvailableBlock":
			return F.ok(answers["first"])
		return F.ok(null)
	var rpc: ScfgSolanaRpc = _client({A: h})[0]
	T.eq(await rpc.get_transaction_any("S", {"min_slot": 5}), [null, false], "unreadable: not authoritative")
	T.eq(await rpc.get_transaction_any("S", {"min_slot": 5}), [null, false], "again")
	T.eq(calls, ["getTransaction", "getFirstAvailableBlock", "getTransaction", "getFirstAvailableBlock"], "asked twice")
	answers["first"] = 3.0
	calls.clear()
	T.eq(await rpc.get_transaction_any("S", {"min_slot": 5}), [null, true], "readable")
	T.eq(await rpc.get_transaction_any("S", {"min_slot": 5}), [null, true], "cached")
	T.eq(calls, ["getTransaction", "getFirstAvailableBlock", "getTransaction"], "asked once")
	T.eq(rpc._first_slot, {A: 3}, "cached as an int")
	return T.done()


func test_first_available_errors() -> bool:
	# an RpcError or RpcUnavailable, a TypeError or ValueError from int(): unknown history
	for first: Variant in [null, "x", [1]]:
		var rpc: ScfgSolanaRpc = _client(_history(first, first))[0]
		T.eq(await rpc.get_transaction_any("S", {"min_slot": 5}), [null, false], "first %s" % [first])
	var rpc_err := func(m: String, _p: Array) -> Variant:
		return F.err(-32000, "no") if m == "getFirstAvailableBlock" else F.ok(null)
	T.eq(await (_client({A: rpc_err})[0] as ScfgSolanaRpc).get_transaction_any("S", {"min_slot": 5}), [null, false], "RpcError")
	# a KeyError (`{"error": null}`) is not caught: it propagates
	var key_err := func(m: String, _p: Array) -> Variant:
		return {"error": null} if m == "getFirstAvailableBlock" else F.ok(null)
	T.is_err(await (_client({A: key_err})[0] as ScfgSolanaRpc).get_transaction_any("S", {"min_slot": 5}), "KeyError",
		"KeyError propagates")
	return T.done()


func test_small_helpers() -> bool:
	var handler := func(m: String, _p: Array) -> Variant:
		match m:
			"getSlot":
				return F.ok(77.0)
			"getSignaturesForAddress":
				return F.ok([{"signature": "S2"}, {"signature": "S1"}])
			"getSignatureStatuses":
				return F.ok({"value": [null, {"confirmationStatus": "finalized"}]})
			"getAccountInfo":
				return F.ok({"value": null})
			"getLatestBlockhash":
				return F.ok({"value": {"blockhash": "BH", "lastValidBlockHeight": 9.0}})
			"getBalance":
				return F.ok({"value": 1234.0})
			"getRecentPrioritizationFees":
				return F.ok([9.0, 1.0, 5.0].map(func(f: float) -> Dictionary: return {"slot": 1.0, "prioritizationFee": f}))
		return null
	var r := _client({A: handler})
	var rpc: ScfgSolanaRpc = r[0]
	var t: F.FakeTransport = r[1]
	T.eq(await rpc.get_slot(), 77, "get_slot")
	var sigs: Variant = await rpc.get_signatures_for_address("X", {"until": "S0"})
	T.eq((sigs as Array).map(func(s: Dictionary) -> String: return s["signature"]) if sigs is Array else sigs,
		["S2", "S1"], "signatures")
	T.eq(t.calls[-1][2], ["X", {"limit": 100, "commitment": "confirmed", "until": "S0"}], "signature params")
	var st: Variant = await rpc.get_signature_statuses(["a", "b"])
	T.eq(st[1]["confirmationStatus"] if st is Array else st, "finalized", "statuses")
	T.eq(t.calls[-1][2][1], {"searchTransactionHistory": true}, "status params")
	T.eq(await rpc.account_exists("X"), false, "account_exists")
	T.eq(await rpc.get_token_balance("X"), 0, "get_token_balance")
	T.eq(await rpc.get_latest_blockhash(), "BH", "blockhash")
	T.eq(await rpc.get_balance("X"), 1234, "get_balance")
	T.eq(await rpc.get_priority_fee(["X"]), 5, "median fee")
	return T.done()


func test_signatures_options_and_falsy_answers() -> bool:
	var r := _client({A: F.always(F.ok(null))})
	var rpc: ScfgSolanaRpc = r[0]
	var t: F.FakeTransport = r[1]
	T.eq(await rpc.get_signatures_for_address("X", {"before": "B1", "until": "", "limit": 5, "commitment": "finalized"}),
		[], "None or [] is []")
	T.eq(t.calls[-1][2], ["X", {"limit": 5, "commitment": "finalized", "before": "B1"}], "falsy until left out")
	T.is_err(await rpc.get_signatures_for_address("X", {"limt": 5}), "TypeError", "a misspelt keyword")
	for v: Variant in [[], {}, "", 0.0]:
		var c: ScfgSolanaRpc = _client({A: F.always(F.ok(v))})[0]
		T.eq(await c.get_signatures_for_address("X"), [], "%s or []" % [v])
	var one: ScfgSolanaRpc = _client({A: F.always(F.ok([1.0]))})[0]
	T.eq(await one.get_signatures_for_address("X"), [1.0], "a list as it is")
	return T.done()


## py_get lookups: Python's res["value"] etc. (CPython's outcomes).
func test_lookups_mirror_python_subscripts() -> bool:
	var rows := [
		# [result, get_token_balance, account_exists]
		[null, "TypeError", "TypeError"], [{}, "KeyError", "KeyError"], [{"value": null}, 0, false],
		[{"value": {"data": {}}}, "KeyError", true],
		[{"value": {"data": {"parsed": {"info": {"tokenAmount": {"amount": "12"}}}}}}, 12, true],
		[5.0, "TypeError", "TypeError"], [[], "TypeError", "TypeError"],
	]
	for row: Array in rows:
		var rpc: ScfgSolanaRpc = _client({A: F.always(F.ok(row[0]))})[0]
		var bal: Variant = await rpc.get_token_balance("X")
		var ex: Variant = await rpc.account_exists("X")
		if row[1] is String:
			T.is_err(bal, row[1], "balance of %s" % [row[0]])
		else:
			T.eq(bal, row[1], "balance of %s" % [row[0]])
		if row[2] is String:
			T.is_err(ex, row[2], "exists of %s" % [row[0]])
		else:
			T.eq(ex, row[2], "exists of %s" % [row[0]])
	var big: ScfgSolanaRpc = _client({A: F.always(F.ok({"value": {"data": {"parsed": {"info": {"tokenAmount": {
		"amount": "123456789012345678901234567890"}}}}}}))})[0]
	var v: Variant = await big.get_token_balance("X")
	T.ok(v is ScfgBigInt and (v as ScfgBigInt).to_decimal() == "123456789012345678901234567890", "a uint64+ amount (%s)" % [v])
	var statuses: ScfgSolanaRpc = _client({A: F.always({"error": null})})[0]
	T.is_err(await statuses.get_signature_statuses(["a"]), "KeyError", "statuses: no result")
	return T.done()


func test_slot_and_balance_are_python_int() -> bool:
	var rows := [["77", 77], [77.9, 77], [null, "TypeError"], ["x", "ValueError"], [true, 1]]
	for row: Array in rows:
		var rpc: ScfgSolanaRpc = _client({A: F.always(F.ok(row[0]))})[0]
		var got: Variant = await rpc.get_slot()
		if row[1] is String:
			T.is_err(got, row[1], "slot %s" % [row[0]])
		else:
			T.eq(got, row[1], "slot %s" % [row[0]])
	return T.done()


func test_priority_fee_rows_like_python() -> bool:
	var rows := [[null, 0], [[], 0], [{"a": 1.0}, "TypeError"], ["ab", "TypeError"], [5.0, "TypeError"],
		[[{"prioritizationFee": "7"}, {"prioritizationFee": 3.0}], 7], [[{"x": 1.0}], "KeyError"],
		[[{"prioritizationFee": 1.0}, {"prioritizationFee": 2.0}], 2]]
	for row: Array in rows:
		var rpc: ScfgSolanaRpc = _client({A: F.always(F.ok(row[0]))})[0]
		var got: Variant = await rpc.get_priority_fee(["X"])
		if row[1] is String:
			T.is_err(got, row[1], "fees %s" % [row[0]])
		else:
			T.eq(got, row[1], "fees %s" % [row[0]])
	return T.done()


func test_list_arguments_are_python_list() -> bool:
	var r := _client({A: F.always(F.ok({"value": [1.0]}))})
	var rpc: ScfgSolanaRpc = r[0]
	await rpc.get_signature_statuses("ab")
	T.eq((r[1] as F.FakeTransport).calls[-1][2], [["a", "b"], {"searchTransactionHistory": true}], "list('ab')")
	await rpc.get_signature_statuses(PackedStringArray(["s"]))
	T.eq((r[1] as F.FakeTransport).calls[-1][2][0], ["s"], "a PackedStringArray")
	T.is_err(await rpc.get_signature_statuses(5), "TypeError", "list(5)")
	T.eq((r[1] as F.FakeTransport).calls.size(), 2, "no request for list(5)")
	return T.done()


func _versioned(needs: int) -> Callable:
	return func(m: String, p: Array) -> Variant:
		if m == "getFirstAvailableBlock":
			return F.ok(0.0)
		if p[1]["maxSupportedTransactionVersion"] < needs:
			return F.err(-32015, _V2.replace("(2)", "(%d)" % needs).replace(": 2", ": %d" % needs))
		return F.ok({"slot": 5.0})


func test_get_transaction_retries_once_with_the_version_the_node_names() -> bool:
	var r := _client({A: _versioned(2)})
	var rpc: ScfgSolanaRpc = r[0]
	T.eq(await rpc.get_transaction("SIG"), {"slot": 5.0}, "the transaction")
	T.eq((r[1] as F.FakeTransport).calls.map(_version_of), [ScfgSolanaRpc.TX_VERSION, 2], "versions asked")
	T.eq(rpc.tx_version, ScfgSolanaRpc.TX_VERSION, "tx_version unchanged")
	return T.done()


func test_unsupported_version_error_naming_an_older_version_is_raised() -> bool:
	var r := _client({A: F.always(F.err(-32015, _V2.replace("(2)", "(0)")))})
	T.is_err(await (r[0] as ScfgSolanaRpc).get_transaction("SIG"), "RpcError", "raised")
	T.eq((r[1] as F.FakeTransport).calls.size(), 1, "one call")
	return T.done()


func test_get_transaction_any_retries_with_the_named_version() -> bool:
	var rpc: ScfgSolanaRpc = _client({A: _versioned(3)})[0]
	T.eq(await rpc.get_transaction_any("SIG", {"min_slot": 1}), [{"slot": 5.0}, false], "found")
	return T.done()


func test_get_transaction_any_lowers_the_version_and_starts_over() -> bool:
	var h := func(_m: String, p: Array) -> Variant:
		if p[1]["maxSupportedTransactionVersion"] >= 1:
			return F.err(-32602, "unknown Version")
		return F.ok({"slot": 1.0})
	var r := _client({A: h})
	var rpc: ScfgSolanaRpc = r[0]
	T.eq(await rpc.get_transaction_any("S", {"min_slot": 1}), [{"slot": 1.0}, false], "found")
	T.eq(rpc.tx_version, 0, "lowered")
	T.eq((r[1] as F.FakeTransport).calls.map(_version_of), [1, 0], "versions asked")
	return T.done()


## A named-version retry that fails moves on to the next endpoint (get_transaction raises it).
func test_named_version_retry_failure() -> bool:
	var a := func(_m: String, p: Array) -> Variant:
		if p[1]["maxSupportedTransactionVersion"] == 1:
			return F.err(-32015, "version (5) not supported")
		return F.err(-1, "nope")
	var one: ScfgSolanaRpc = _client({A: a})[0]
	var got: Variant = await one.get_transaction("S")
	T.ok(got is ScfgErr and (got as ScfgErr).is_a("RpcError") and (got as ScfgErr).code == -1, "the retry's error (%s)" % [got])
	var two: ScfgSolanaRpc = _client({A: a, B: F.always(F.ok({"slot": 9.0}))})[0]
	T.eq(await two.get_transaction_any("S"), [{"slot": 9.0}, false], "the next endpoint")
	return T.done()


## `re.search(r"version \((\d+)\)", message, re.IGNORECASE)`: CPython's IGNORECASE also matches
## ſ for s and İ, ı for i; \d is any Unicode decimal digit; the first match counts.
func test_named_version_matches_like_python_re() -> bool:
	var rows := [["Transaction VERſION (2) x", 2], ["version (٣)", 3], ["Version (2", null], ["version (2) version (5)", 2],
		["version(2)", null], ["versıon (4)", 4], ["versİon (4)", 4], ["version (x) version (6)", 6], ["version (1)", null],
		["version (00007)", 7]]
	for row: Array in rows:
		var rpc: ScfgSolanaRpc = _client({})[0]
		T.eq(rpc._named_version(ScfgRpcClient.rpc_error(-32015, row[0])), row[1], row[0])
	var rpc2: ScfgSolanaRpc = _client({})[0]
	T.eq(rpc2._named_version(ScfgRpcClient.rpc_error(-32000, "version (5)")), null, "only for -32015")
	return T.done()


# tests/unit/test_solana_rpc.py::test_named_version_ignores_numbers_beyond_18_digits
func test_named_version_ignores_numbers_beyond_18_digits() -> bool:
	var rows := [["Transaction version (" + "9".repeat(19) + ") is not supported", null],
		["Transaction version (" + "9".repeat(4301) + ") is not supported", null],
		["Transaction version (" + "9".repeat(18) + ") is not supported", 999999999999999999],
		["Transaction version (" + "0".repeat(30) + "3) is not supported", 3],
		["version (" + "9".repeat(19) + ") version (5)", null], ["version (12 version (5)", 5]]
	for row: Array in rows:
		var rpc: ScfgSolanaRpc = _client({})[0]
		T.eq(rpc._named_version(ScfgRpcClient.rpc_error(-32015, row[0])), row[1], (row[0] as String).substr(0, 40))
	var start := Time.get_ticks_msec()
	var rpc2: ScfgSolanaRpc = _client({})[0]
	T.eq(rpc2._named_version(ScfgRpcClient.rpc_error(-32015, "version (" + "9".repeat(200000) + ")")), null, "200,000 digits")
	T.eq(rpc2._named_version(ScfgRpcClient.rpc_error(-32015, "version (" + "0".repeat(200000) + "4)")), 4, "200,000 zeros")
	T.ok(Time.get_ticks_msec() - start < 2000, "read in linear time")
	return T.done()


# tests/unit/test_solana_rpc.py::test_an_overlong_named_version_raises_the_node_error
func test_an_overlong_named_version_raises_the_node_error() -> bool:
	var r := _client({A: F.always(F.err(-32015, _V2.replace("(2)", "(" + "9".repeat(4301) + ")")))})
	var rpc: ScfgSolanaRpc = r[0]
	var got: Variant = await rpc.get_transaction("SIG")
	T.is_err(got, "RpcError", "the node's error")
	T.eq((r[1] as F.FakeTransport).calls.size(), 1, "one request")
	return T.done()


## The Solana requests: every int parameter is an int, never a float (see test_rpc_client).
func test_requests_carry_ints_not_floats() -> bool:
	var h := func(m: String, p: Array) -> Variant:
		match m:
			"getTransaction":
				var v: Variant = p[1]["maxSupportedTransactionVersion"]
				if v == 1:
					return F.err(-32015, "version (3) not supported")
				return F.ok(null)
			"getFirstAvailableBlock":
				return F.ok(0.0)
		return F.ok({"value": {"blockhash": "B"}})
	var r := _client({A: h})
	var rpc: ScfgSolanaRpc = r[0]
	await rpc.get_transaction("S")
	await rpc.get_transaction_any("S", {"min_slot": 4})
	await rpc.get_signatures_for_address("X", {"before": "B", "until": "U"})
	await rpc.get_latest_blockhash()
	await rpc.get_slot()
	await rpc.get_balance("X")
	rpc.tx_version = 1
	var lowering := func(_m: String, p: Array) -> Variant:
		if p[1]["maxSupportedTransactionVersion"] == 1:
			return F.err(-32602, "bad version")
		return F.ok({"slot": 1.0})
	var low := _client({A: lowering})
	await (low[0] as ScfgSolanaRpc).get_transaction("S")
	var payloads: Array = (r[1] as F.FakeTransport).payloads + (low[1] as F.FakeTransport).payloads
	T.ok(payloads.size() >= 10, "requests were made (%d)" % payloads.size())
	var versions := []
	for p: Dictionary in payloads:
		T.eq(preload("res://tests/test_rpc_client.gd").find_float(p), "", "no float in %s" % [p])
		T.eq(preload("res://tests/test_rpc_client.gd").fractional_number(JSON.stringify(p)), "",
			"no fractional number in %s" % JSON.stringify(p))
		if p["method"] == "getTransaction":
			versions.append(p["params"][1]["maxSupportedTransactionVersion"])
	T.eq(versions, [1, 3, 1, 3, 1, 0], "the versions asked (named 3, lowered to 0)")
	return T.done()
