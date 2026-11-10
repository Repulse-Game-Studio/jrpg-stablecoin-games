## The payment watcher: advances one pending session per call. Mirrors scfg_core/watcher.py.
##
##     awaiting --(hinted tx verified | exact amount found by scan)--> confirming
##     confirming --(N confirmations, block hash unchanged)--> paid (receipt stored)
##     confirming --(block hash changed: reorg)--> awaiting
##     awaiting --(hinted tx pays less than min_price)--> underpaid
##     awaiting --(24 h, nothing found)--> closed
##
## `advance` is logic over plain records plus an RPC object (ScfgRpcClient, or a fake with the
## same method names), so it is testable with a fake chain. State is only mutated after RPC calls
## succeed, so a network outage never corrupts a session. `advance` is a coroutine: `var out =
## await ScfgWatcher.advance(...)`.
##
## A StepOutcome is a Dictionary with Python's dataclass fields in order (STEP_OUTCOME_FIELDS):
## session_id, status (missing | waiting | confirming | reorged | paid | underpaid | closed |
## expired), confirmations (0), required (0), result (a PaymentResult or null), error (null, or
## network_unavailable | tx_reverted | tx_not_a_payment | tx_not_landed (Solana) | internal_error
## (ScfgEngine.tick: the session's step failed)) and hint_tx (the hint a tx_reverted /
## tx_not_a_payment / tx_not_landed verdict is about, or null).
##
## Errors: an RpcError or RpcUnavailable becomes `error: "network_unavailable"` (Python's
## `except (RpcUnavailable, RpcError)`); any other ScfgErr is returned for the Engine to contain.
## Record fields are read the way Python indexes them (a missing one is a KeyError), and values
## compare with Python's `==` (ScfgPy.py_eq), so a malformed record is an error, never a guess.
##
## Port limit (M1 Ruling 3): `match.value` is persisted as a JSON number, so a hinted transfer of
## 2^53 units or more (about 9e9 tokens) cannot be recorded exactly. `_match_dict` returns a
## ValueError instead of rounding, before anything is mutated; the Engine contains it as
## `internal_error` on every tick until the 24 h horizon closes the session. Python would store
## the exact int; no vector can record it. Raw log values stay ScfgBigInt until then (an Arc
## value above 2^63 floors exactly).
class_name ScfgWatcher
extends RefCounted

const SCAN_STEP_BLOCKS := 10_000  # upper bound on blocks scanned per advance()
const MAX_SCAN_CALLS := 5  # eth_getLogs calls per advance(), to be gentle with public RPCs
const STEP_OUTCOME_FIELDS: Array[String] = ["session_id", "status", "confirmations", "required", "result", "error",
	"hint_tx"]
const _MATCH_FIELDS: Array[String] = ["tx_hash", "log_index", "block", "block_hash", "value", "payer", "recipient"]


## Python `StepOutcome(session_id, status, **fields)` with the dataclass defaults. An unknown
## field is a TypeError.
## @fallible
static func step_outcome(session_id: Variant, status: String, fields: Dictionary = {}) -> Variant:
	return ScfgModels.dataclass("StepOutcome", STEP_OUTCOME_FIELDS, {"confirmations": 0, "required": 0,
		"result": null, "error": null, "hint_tx": null}, _merged({"session_id": session_id, "status": status}, fields))


static func _merged(a: Dictionary, b: Dictionary) -> Dictionary:
	var out := a.duplicate()
	out.merge(b, true)
	return out


## The least a hinted payment must pay: an unlock's item min_price (its stored price when the
## item is gone), or 0.000001 for a tip.
## @fallible
static func min_amount(rec: Variant, items: Dictionary) -> Variant:
	var kind: Variant = ScfgPy.py_get(rec, "kind")
	if kind is ScfgErr:
		return kind
	if ScfgPy.py_eq(kind, "unlock"):
		var key: Variant = ScfgPy.py_get(rec, "item")
		if key is ScfgErr:
			return key
		if key is Array or key is Dictionary:
			return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(key))
		var item: Variant = items.get(key) if (key is String or key is StringName) else null
		if item is Dictionary:
			return item["min_price"]
		return ScfgSessions.session_price(rec)
	return ScfgDec.parse("0.000001")


## A TransferLog's match record: its value must fit a JSON number exactly (see the header).
## @fallible
static func _match_dict(t: Dictionary) -> Variant:
	var value: Variant = _stored_int(t["value"], "transfer value")
	if value is ScfgErr:
		return value
	return {"tx_hash": t["tx_hash"], "log_index": t["log_index"], "block": t["block_number"],
		"block_hash": t["block_hash"], "value": value, "payer": t["sender"], "recipient": t["recipient"]}


## An ScfgBigInt (or int) below 2^53 as an int; a ValueError for anything larger.
## @fallible
static func _stored_int(v: Variant, what: String) -> Variant:
	if v is int:
		if v >= ScfgPy.TWO_53 or v <= -ScfgPy.TWO_53:
			return ScfgErr.make("ValueError", "%s %d does not fit a JSON number exactly" % [what, v])
		return v
	var big: ScfgBigInt = v
	if big.bit_length() > 53:
		return ScfgErr.make("ValueError", "%s %s does not fit a JSON number exactly" % [what, big.to_decimal()])
	return big.to_int()


## Python `result_from_match(rec, status, chain_key, m)`: the PaymentResult of a match.
## @fallible
static func result_from_match(rec: Variant, status: String, chain_key: Variant, m: Variant) -> Variant:
	var fields := {"status": status}
	for pair: Array in [["kind", rec, "kind"], ["item", rec, "item"], ["amount", m, "value"], ["token", rec, "token"],
			["tx_hash", m, "tx_hash"], ["payer", m, "payer"], ["session_id", rec, "id"]]:
		var v: Variant = ScfgPy.py_get(pair[1], pair[2])
		if v is ScfgErr:
			return v
		if pair[0] == "amount":
			v = ScfgAmounts.units_to_decimal(v)
			if v is ScfgErr:
				return v
		fields[pair[0]] = v
	fields["chain"] = chain_key
	return ScfgModels.make_payment_result(fields)


## `chain.tokens[rec["token"]]`: the session's Token.
## @fallible
static func _token(rec: Variant, chain: Dictionary) -> Variant:
	var name: Variant = ScfgPy.py_get(rec, "token")
	if name is ScfgErr:
		return name
	if name is Array or name is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(name))
	return ScfgPy.py_get(chain["tokens"], name)


## Python `_qualifies(t, rec, chain, wallet, receipts)`, with its short-circuit order: the right
## token, to the wallet, at or after start_block, and not already a receipt.
## @fallible
static func _qualifies(t: Variant, rec: Variant, chain: Dictionary, wallet: Variant, receipts: Dictionary) -> Variant:
	if t == null:
		return false
	var token: Variant = _token(rec, chain)
	if token is ScfgErr:
		return token
	if not ScfgPy.py_eq(t["token"], token["address"]):
		return false
	if not ScfgPy.py_eq(t["recipient"], wallet):
		return false
	var start: Variant = ScfgSessions.number_field(rec, "start_block")
	if start is ScfgErr:
		return start
	if not t["block_number"] >= start:
		return false
	var key: Variant = ScfgReceipts.receipt_key(chain["key"], t["tx_hash"], t["log_index"])
	if key is ScfgErr:
		return key
	return not receipts.has(key)


## `rec[key]` used as a block number (sent as hex): an int; an integral float is read as that int
## (Python never stores one; a JSON loader might). Otherwise a TypeError.
## @fallible
static func _block_field(rec: Variant, key: String) -> Variant:
	var v: Variant = ScfgSessions.number_field(rec, key)
	if v is ScfgErr:
		return v
	if v is float:
		var f: float = v
		if is_nan(f) or is_inf(f) or f != floorf(f):
			return ScfgErr.make("TypeError", "'float' object cannot be interpreted as an integer (%s)" % key)
		var n: Variant = ScfgPy.json_int(f)
		if n is ScfgErr:
			return n
		return n
	return v


## The head an RPC object reported, as an int. Port limit: a head beyond 64 bits (an ScfgBigInt)
## is an OverflowError.
## @fallible
static func _head(head: Variant) -> Variant:
	if head is int:
		return head
	if head is ScfgBigInt:
		return ScfgErr.make("OverflowError", "block number %s is beyond the port's 64-bit ints" % (head as ScfgBigInt).to_decimal())
	return ScfgErr.make("TypeError", "a block number must be an int, not %s" % ScfgPy.py_type(head))


## Check the tx hash reported by the browser page or pasted by the player. Returns "matched",
## "underpaid", or null (keep waiting / hint discarded); `out` gets the verdict's error and hint.
## @fallible
static func _verify_hint(rec: Dictionary, rpc: Variant, chain: Dictionary, wallet: Variant, items: Dictionary,
		receipts: Dictionary, now: Variant, out: Dictionary) -> Variant:
	var receipt: Variant = await rpc.get_transaction_receipt(rec["hint_tx"])
	if receipt is ScfgErr:
		return receipt
	if receipt == null:  # not mined yet (or unknown): give it up to 30 minutes
		var at: Variant = ScfgSessions.number_field(rec, "hint_at")
		if at is ScfgErr:
			return at
		if now - at >= ScfgSessions.HINT_TIMEOUT_S:
			ScfgSessions.clear_hint(rec)
		return null
	if not receipt is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'get'" % ScfgPy.py_type(receipt))
	if ScfgPy.py_eq((receipt as Dictionary).get("status"), "0x0"):
		out["hint_tx"] = rec["hint_tx"]  # the verdict is about this hint
		ScfgSessions.clear_hint(rec)
		out["error"] = "tx_reverted"
		return null
	var logs: Variant = (receipt as Dictionary).get("logs")
	var each: Variant = []
	if ScfgPy.py_truthy(logs):  # receipt.get("logs") or []
		each = ScfgPy.py_iter(logs)
		if each is ScfgErr:
			return each
	var candidates := []
	for lg: Variant in each:
		var t: Variant = ScfgChains.parse_transfer(chain, lg)
		if t is ScfgErr:
			return t
		var q: Variant = _qualifies(t, rec, chain, wallet, receipts)
		if q is ScfgErr:
			return q
		if q and not (t["value"] as ScfgBigInt).is_zero():
			candidates.append(t)
	if candidates.is_empty():
		out["hint_tx"] = rec["hint_tx"]  # the verdict is about this hint
		ScfgSessions.clear_hint(rec)
		out["error"] = "tx_not_a_payment"
		return null
	var best: Dictionary = candidates[0]  # Python's max(): the first of the largest values
	for c: Dictionary in candidates:
		if (c["value"] as ScfgBigInt).cmp(best["value"]) > 0:
			best = c
	var m: Variant = _match_dict(best)
	if m is ScfgErr:
		return m
	rec["match"] = m
	var paid: Variant = ScfgAmounts.units_to_decimal(best["value"])
	if paid is ScfgErr:
		return paid
	var least: Variant = min_amount(rec, items)
	if least is ScfgErr:
		return least
	if (paid as ScfgDec).cmp(least) < 0:
		return "underpaid"
	return "matched"


## Look for a transfer of exactly the session's encoded amount since the cursor. Returns whether
## one was found (the cursor moves on either way).
## @fallible
static func _scan(rec: Dictionary, rpc: Variant, chain: Dictionary, wallet: Variant, receipts: Dictionary,
		head: int) -> Variant:
	var at: Variant = ScfgSessions.number_field(rec, "cursor")
	if at is ScfgErr:
		return at
	if at > head:  # this endpoint is behind the one that set start_block
		return false
	var cursor: Variant = _block_field(rec, "cursor")
	if cursor is ScfgErr:
		return cursor
	var span: Variant = (rpc as Object).get("log_range")  # Python's getattr(rpc, "log_range", None)
	var step: int = SCAN_STEP_BLOCKS
	if ScfgPy.py_truthy(span):
		step = mini(SCAN_STEP_BLOCKS, int(span) * MAX_SCAN_CALLS)
	var to: int = mini(head, cursor + step - 1)
	var token: Variant = _token(rec, chain)
	if token is ScfgErr:
		return token
	var topic: Variant = ScfgEvm.address_topic(wallet)
	if topic is ScfgErr:
		return topic
	var logs: Variant = await rpc.get_logs(ScfgChains.log_source(token), [ScfgEvm.TRANSFER_TOPIC, null, topic],
		cursor, to)
	if logs is ScfgErr:
		return logs
	var each: Variant = ScfgPy.py_iter(logs)
	if each is ScfgErr:
		return each
	var found := []
	for lg: Variant in each:
		var t: Variant = ScfgChains.parse_transfer(chain, lg)
		if t is ScfgErr:
			return t
		var q: Variant = _qualifies(t, rec, chain, wallet, receipts)
		if q is ScfgErr:
			return q
		if not q:
			continue
		var units: Variant = ScfgPy.py_get(rec, "units")  # read only for a qualifying log, as Python does
		if units is ScfgErr:
			return units
		if ScfgPy.py_eq(t["value"], units):
			found.append(t)
	# sorted by (block_number, log_index); a stable sort, as Python's
	var keyed := []
	for i in found.size():
		keyed.append([found[i]["block_number"], found[i]["log_index"], i])
	keyed.sort_custom(func(a: Array, b: Array) -> bool:
		return a[0] < b[0] or (a[0] == b[0] and (a[1] < b[1] or (a[1] == b[1] and a[2] < b[2]))))
	rec["cursor"] = to + 1
	if not keyed.is_empty():
		var m: Variant = _match_dict(found[keyed[0][2]])
		if m is ScfgErr:
			return m
		rec["match"] = m
		return true
	return false


## `out` after a failed RPC call: Python's `except (RpcUnavailable, RpcError)` sets
## network_unavailable and returns the outcome; any other error propagates.
## @fallible
static func _network(out: Dictionary, err: ScfgErr) -> Variant:
	if err.is_a("RpcUnavailable") or err.is_a("RpcError"):
		out["error"] = "network_unavailable"
		return out
	return err


## Move one pending session forward. `opts` holds Python's keyword arguments: rpc, chain (a Chain
## Dictionary), wallet, items ({item_id: Item}) and now. Returns a StepOutcome, or an ScfgErr the
## Engine contains (see the header).
## @fallible
static func advance(pending: Dictionary, receipts: Dictionary, sid: Variant, opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var wallet: Variant = opts["wallet"]
	var items: Dictionary = opts["items"]
	var now: Variant = opts["now"]
	var rec: Variant = pending.get(sid)
	var made: Variant = step_outcome(sid, "missing", {"required": chain["confirmations"]})
	if made is ScfgErr:
		return made
	var out: Dictionary = made
	if rec == null:
		return out
	var head: Variant = null
	var state: Variant = ScfgPy.py_get(rec, "state")
	if state is ScfgErr:
		return state
	if ScfgPy.py_eq(state, "awaiting"):
		out["status"] = "waiting"
		var past: Variant = ScfgSessions.is_past_horizon(rec, now)
		if past is ScfgErr:
			return past
		if past:
			ScfgSessions.remove_session(pending, sid)
			out["status"] = "closed"
			return out
		var outcome: Variant = null
		var hint: Variant = ScfgPy.py_get(rec, "hint_tx")
		if hint is ScfgErr:
			return hint
		if ScfgPy.py_truthy(hint):
			outcome = await _verify_hint(rec, rpc, chain, wallet, items, receipts, now, out)
			if outcome is ScfgErr:
				return _network(out, outcome)
		if outcome == null and not ScfgPy.py_truthy(rec["hint_tx"]):
			var reported: Variant = await rpc.block_number()
			if reported is ScfgErr:
				return _network(out, reported)
			head = _head(reported)
			if head is ScfgErr:
				return head
			var found: Variant = await _scan(rec, rpc, chain, wallet, receipts, head)
			if found is ScfgErr:
				return _network(out, found)
			outcome = "matched" if found else null
		if ScfgPy.py_eq(outcome, "underpaid"):
			ScfgSessions.remove_session(pending, sid)
			out["status"] = "underpaid"
			var underpaid: Variant = result_from_match(rec, "underpaid", chain["key"], rec["match"])
			if underpaid is ScfgErr:
				return underpaid
			out["result"] = underpaid
			return out
		if not ScfgPy.py_eq(outcome, "matched"):
			return out
		rec["state"] = "confirming"

	out["status"] = "confirming"
	var m: Variant = ScfgPy.py_get(rec, "match")
	if m is ScfgErr:
		return m
	if head == null:
		var reported: Variant = await rpc.block_number()
		if reported is ScfgErr:
			return _network(out, reported)
		head = _head(reported)
		if head is ScfgErr:
			return head
	var block: Variant = _block_field(m, "block")
	if block is ScfgErr:
		return block
	if head < block:  # lagging endpoint, not a reorg
		return out
	var current: Variant = await rpc.get_block_hash(block)
	if current is ScfgErr:
		return _network(out, current)
	var reorged: bool = current == null
	if not reorged:
		if not (current is String or current is StringName):
			return ScfgErr.make("AttributeError", "'%s' object has no attribute 'lower'" % ScfgPy.py_type(current))
		var recorded: Variant = ScfgPy.py_get(m, "block_hash")
		if recorded is ScfgErr:
			return recorded
		reorged = not ScfgPy.py_eq(str(current).to_lower(), recorded)
	if reorged:
		rec["state"] = "awaiting"
		rec["match"] = null
		var cursor: Variant = ScfgSessions.number_field(rec, "cursor")
		if cursor is ScfgErr:
			return cursor
		rec["cursor"] = cursor if cursor <= block else block  # min(): the first of equals
		out["status"] = "reorged"
		return out
	out["confirmations"] = head - block + 1
	if out["confirmations"] < chain["confirmations"]:
		return out
	var token: Variant = _token(rec, chain)
	if token is ScfgErr:
		return token
	var transfer := {"token": token["address"]}
	for pair: Array in [["sender", "payer"], ["recipient", "recipient"], ["value", "value"], ["tx_hash", "tx_hash"],
			["log_index", "log_index"]]:
		var v: Variant = ScfgPy.py_get(m, pair[1])
		if v is ScfgErr:
			return v
		transfer[pair[0]] = v
	transfer["block_number"] = block
	var block_hash: Variant = ScfgPy.py_get(m, "block_hash")
	if block_hash is ScfgErr:
		return block_hash
	transfer["block_hash"] = block_hash
	var receipt_opts := {"chain": chain["key"], "transfer": transfer, "source": "payment"}
	for f: String in ["kind", "item", "token"]:
		var v: Variant = ScfgPy.py_get(rec, f)
		if v is ScfgErr:
			return v
		receipt_opts[f] = v
	var receipt: Variant = ScfgReceipts.make_receipt(receipt_opts)
	if receipt is ScfgErr:
		return receipt
	var added: Variant = ScfgReceipts.add_receipt(receipts, receipt)
	if added is ScfgErr:
		return added
	ScfgSessions.remove_session(pending, sid)
	out["status"] = "paid"
	var paid: Variant = result_from_match(rec, "paid", chain["key"], m)
	if paid is ScfgErr:
		return paid
	out["result"] = paid
	return out
