## Solana JSON-RPC on top of the shared failover client (ScfgRpcClient). Mirrors
## scfg_core/solana/rpc.py, statement by statement.
##
## History-sensitive lookups (restore, launch re-check) ask endpoints one by one, because public
## endpoints keep very different amounts of history: publicnode about two days, api.mainnet-beta
## everything. A missing transaction only counts as "doesn't exist" when an endpoint whose
## history reaches back to the slot in question says so.
##
## Every method is a coroutine returning its value or an ScfgErr. Python's subscripts on answers
## (`res["value"]`) are ScfgPy.py_get (a missing key is a KeyError, a non-dict a TypeError) and
## its int() is ScfgPy.py_int. Python's `call` is `call_rpc`; keyword arguments are `opts`
## Dictionaries (an unknown key is a TypeError).
class_name ScfgSolanaRpc
extends RefCounted

## The highest transaction version this build can read (only meta is used).
const TX_VERSION := 1
const _UNKNOWN_HISTORY := 1 << 62
const _UNSUPPORTED_VERSION := -32015

## An ScfgRpcClient (or anything with its methods).
var client: Variant
var tx_version: int = TX_VERSION
## {url: first available slot}, only for answers that int() could read.
var _first_slot: Dictionary = {}


func _init(client_: Variant) -> void:
	client = client_


## @fallible
func call_rpc(method: String, params: Array) -> Variant:
	return await client.call_rpc(method, params)


## @fallible
func get_slot(commitment: String = "confirmed") -> Variant:
	var res: Variant = await call_rpc("getSlot", [{"commitment": commitment}])
	if res is ScfgErr:
		return res
	return ScfgPy.py_int(res)


## opts: until, before (left out when falsy), limit (100), commitment ("confirmed").
## @fallible
func get_signatures_for_address(address: Variant, opts: Dictionary = {}) -> Variant:
	var bad: Variant = _check_opts(opts, ["until", "before", "limit", "commitment"], "get_signatures_for_address")
	if bad is ScfgErr:
		return bad
	var o := {"limit": opts.get("limit", 100), "commitment": opts.get("commitment", "confirmed")}
	if ScfgPy.py_truthy(opts.get("until")):
		o["until"] = opts["until"]
	if ScfgPy.py_truthy(opts.get("before")):
		o["before"] = opts["before"]
	var res: Variant = await call_rpc("getSignaturesForAddress", [address, o])
	if res is ScfgErr:
		return res
	return res if ScfgPy.py_truthy(res) else []


func _tx_params(sig: Variant, commitment: Variant, version: Variant = null) -> Array:
	return [sig, {"encoding": "json", "commitment": commitment,
		"maxSupportedTransactionVersion": tx_version if version == null else version}]


## The newer version a -32015 "Transaction version (N) is not supported" names, or null. Only
## meta, slot and accountKeys are read, so any version can be read.
func _named_version(error: ScfgErr) -> Variant:
	if not (typeof(error.code) == TYPE_INT and error.code == _UNSUPPORTED_VERSION):
		return null
	var found: Variant = _search_version(error.message)
	if found == null or found <= tx_version:
		return null
	return found


## Python's `re.search(r"version \((\d+)\)", message, re.IGNORECASE)` and int() of the group:
## the first match's number, or null. IGNORECASE as CPython applies it to these letters (ſ also
## matches s; İ and ı also match i), and \d is any Unicode decimal digit. A number beyond 18
## digits is taken as no match (port limit: a request cannot carry it as an int).
static func _search_version(message: String) -> Variant:
	var n := message.length()
	for start in n:
		var i := start
		var ok := true
		for ch in "version":
			if i >= n or not _letter_matches(ch, message.unicode_at(i)):
				ok = false
				break
			i += 1
		if not ok or i + 1 >= n or message[i] != " " or message[i + 1] != "(":
			continue
		i += 2
		var digits := ""
		while i < n and ScfgPy.decimal_digit(message.unicode_at(i)) >= 0:
			digits += str(ScfgPy.decimal_digit(message.unicode_at(i)))
			i += 1
		if digits == "" or i >= n or message[i] != ")":
			continue
		var sig := digits.lstrip("0")
		if sig.length() > 18:
			return null
		return int(sig) if sig != "" else 0
	return null


static func _letter_matches(letter: String, c: int) -> bool:
	if c == letter.unicode_at(0) or c == letter.to_upper().unicode_at(0):
		return true
	if letter == "s":
		return c == 0x17f
	if letter == "i":
		return c == 0x130 or c == 0x131
	return false


## Older nodes reject a maxSupportedTransactionVersion they don't know (-32602).
func _lower_version(error: ScfgErr) -> bool:
	if tx_version > 0 and typeof(error.code) == TYPE_INT and error.code == -32602 \
			and error.message.to_lower().contains("version"):
		tx_version -= 1
		return true
	return false


## @fallible
func get_transaction(sig: Variant, commitment: String = "confirmed") -> Variant:
	var res: Variant = await call_rpc("getTransaction", _tx_params(sig, commitment))
	if res is ScfgErr:
		var e: ScfgErr = res
		if not e.is_a("RpcError"):
			return e
		if _lower_version(e):
			return await get_transaction(sig, commitment)
		var version: Variant = _named_version(e)
		if version == null:
			return e
		return await call_rpc("getTransaction", _tx_params(sig, commitment, version))
	return res


## The url's first available slot (cached), or _UNKNOWN_HISTORY when it cannot be read: unknown
## is treated as pruned and asked again next time. Python's
## `except (RpcUnavailable, RpcError, TypeError, ValueError)`.
## @fallible
func _first_available(url: String) -> Variant:
	if not _first_slot.has(url):
		var res: Variant = await client.call_at(url, "getFirstAvailableBlock", [])
		if not res is ScfgErr:
			res = ScfgPy.py_int(res)
		if res is ScfgErr:
			var e: ScfgErr = res
			if e.is_a("RpcUnavailable") or e.is_a("RpcError") or e.is_a("TypeError") or e.is_a("ValueError"):
				return _UNKNOWN_HISTORY
			return e
		_first_slot[url] = res
	return _first_slot[url]


## [transaction or null, authoritative]: the first endpoint that has it, else whether an endpoint
## whose history reaches `min_slot` said it does not exist. opts: min_slot (null), commitment
## ("finalized"). RpcUnavailable when no endpoint answered.
## @fallible
func get_transaction_any(sig: Variant, opts: Dictionary = {}) -> Variant:
	var bad: Variant = _check_opts(opts, ["min_slot", "commitment"], "get_transaction_any")
	if bad is ScfgErr:
		return bad
	var min_slot: Variant = opts.get("min_slot")
	var commitment: Variant = opts.get("commitment", "finalized")
	var answered := false
	var authoritative := false
	for url: String in client.live_urls():
		var attempt: Variant = await client.try_at(url, "getTransaction", _tx_params(sig, commitment))
		if attempt is ScfgErr:
			return attempt
		if attempt == null:
			continue
		var res: Variant = attempt[1]
		if res is ScfgErr and (res as ScfgErr).is_a("RpcError"):
			if _lower_version(res):
				return await get_transaction_any(sig, opts)
			var version: Variant = _named_version(res)
			if version == null:
				continue
			res = await client.call_at(url, "getTransaction", _tx_params(sig, commitment, version))
			if res is ScfgErr:
				if (res as ScfgErr).is_a("RpcUnavailable") or (res as ScfgErr).is_a("RpcError"):
					continue
				return res
		answered = true
		if res != null:
			return [res, false]
		if min_slot != null:
			var first: Variant = await _first_available(url)
			if first is ScfgErr:
				return first
			if _le(first, min_slot):
				authoritative = true
	if not answered:
		return ScfgErr.make("RpcUnavailable", "no endpoint answered getTransaction for %s" % [sig])
	return [null, authoritative]


## @fallible
func get_signature_statuses(sigs: Variant) -> Variant:
	var listed: Variant = _list(sigs)
	if listed is ScfgErr:
		return listed
	var res: Variant = await call_rpc("getSignatureStatuses", [listed, {"searchTransactionHistory": true}])
	if res is ScfgErr:
		return res
	return ScfgPy.py_get(res, "value")


## @fallible
func account_exists(address: Variant) -> Variant:
	var res: Variant = await call_rpc("getAccountInfo", [address, {"encoding": "base64", "commitment": "confirmed"}])
	if res is ScfgErr:
		return res
	var value: Variant = ScfgPy.py_get(res, "value")
	if value is ScfgErr:
		return value
	return value != null


## @fallible
func get_latest_blockhash() -> Variant:
	var res: Variant = await call_rpc("getLatestBlockhash", [{"commitment": "confirmed"}])
	if res is ScfgErr:
		return res
	return _path(res, ["value", "blockhash"])


## The token account's raw amount (an int, an ScfgBigInt beyond 64 bits), 0 when the account
## does not exist.
## @fallible
func get_token_balance(token_account: Variant) -> Variant:
	var res: Variant = await call_rpc("getAccountInfo", [token_account, {
		"encoding": "jsonParsed", "commitment": "confirmed"}])
	if res is ScfgErr:
		return res
	var value: Variant = ScfgPy.py_get(res, "value")
	if value is ScfgErr:
		return value
	if value == null:
		return 0
	var amount: Variant = _path(value, ["data", "parsed", "info", "tokenAmount", "amount"])
	if amount is ScfgErr:
		return amount
	return ScfgPy.py_int(amount)


## @fallible
func get_balance(address: Variant) -> Variant:
	var res: Variant = await call_rpc("getBalance", [address, {"commitment": "confirmed"}])
	if res is ScfgErr:
		return res
	var value: Variant = ScfgPy.py_get(res, "value")
	if value is ScfgErr:
		return value
	return ScfgPy.py_int(value)


## The median recent prioritization fee for `accounts` (0 when there is none).
## @fallible
func get_priority_fee(accounts: Variant) -> Variant:
	var listed: Variant = _list(accounts)
	if listed is ScfgErr:
		return listed
	var rows: Variant = await call_rpc("getRecentPrioritizationFees", [listed])
	if rows is ScfgErr:
		return rows
	# sorted(int(f["prioritizationFee"]) for f in rows or [])
	var items: Variant = ScfgRpcClient._iterate(rows if ScfgPy.py_truthy(rows) else [])
	if items is ScfgErr:
		return items
	var fees := []
	for f: Variant in items:
		var fee: Variant = ScfgPy.py_get(f, "prioritizationFee")
		if fee is ScfgErr:
			return fee
		var n: Variant = ScfgPy.py_int(fee)
		if n is ScfgErr:
			return n
		fees.append(n)
	fees.sort_custom(func(a: Variant, b: Variant) -> bool: return _lt(a, b))
	return fees[fees.size() / 2] if not fees.is_empty() else 0


## `d[k0][k1]...` with Python's subscript errors.
## @fallible
static func _path(d: Variant, keys: Array) -> Variant:
	var v: Variant = d
	for k: Variant in keys:
		v = ScfgPy.py_get(v, k)
		if v is ScfgErr:
			return v
	return v


## Python's `list(x)`: a copy of a list, a dict's keys, a str's characters; a TypeError otherwise.
## @fallible
static func _list(v: Variant) -> Variant:
	var items: Variant = ScfgRpcClient._iterate(v)
	if items is ScfgErr:
		return items
	return (items as Array).duplicate()


## `a < b` for Python ints (int or ScfgBigInt, which is never below 2^63).
static func _lt(a: Variant, b: Variant) -> bool:
	if a is ScfgBigInt or b is ScfgBigInt:
		if a is ScfgBigInt and b is ScfgBigInt:
			return (a as ScfgBigInt).cmp(b) < 0
		return b is ScfgBigInt
	return a < b


## `a <= b` for a first slot (int or ScfgBigInt) and a caller's min_slot.
static func _le(a: Variant, b: Variant) -> bool:
	return not _lt(b, a)


## A keyword-argument check: an unknown key is a TypeError, as for a Python call.
## @fallible
static func _check_opts(opts: Dictionary, allowed: Array, fn: String) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in allowed):
			return ScfgErr.make("TypeError", "%s() got an unexpected keyword argument %s" % [fn, ScfgPy.py_repr(k)])
	return null
