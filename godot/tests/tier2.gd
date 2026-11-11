## Tier 2 runner: replays conformance/vectors/tier2/**.json against ScfgEngine. A mirror of
## conformance/python/scfg_conformance/replay.py (build_config_from, build_engine, run_action,
## encode_returns, snapshot_json, ScriptedEntropy, ReplayTransport, replay) and of
## conformance/js/tier2.ts; see the README's "Tier 2" section for the contract.
##
## `ENABLED` admits a vector whose every action is one the harness knows, which is every vector
## (test_tier2.gd pins the count against the vector files).
##
## GDScript specifics:
## - The vector is read once as Python's `json.load` sees it (ScfgPy.from_json: an integral
##   number is an int), so setup state, args and draws carry ints where Python holds ints.
## - `response_text` is parsed by the same reader the real transport uses (ScfgCoio.parse_body:
##   numbers are floats), so the core meets exactly what a node's answer gives it.
## - Request params match by JSON value (T.first_diff, which is T.json_eq's rule): objects
##   ignore key order, numbers compare by value, a bool is never a number.
## - A failed draw or an unexpected request is an ScfgErr of kind AssertionError handed to the
##   core (Python raises AssertionError); the problem is recorded too, so the step fails even when
##   the Engine contains the error.
## - A void action (report_hint, set_ui_status, discard_session) returns null, Python's None; any
##   other action returning null was aborted (a script error or a dropped coroutine) and fails.
extends RefCounted

const T := preload("res://tests/t.gd")
const Tier1 := preload("res://tests/tier1.gd")

const ACTIONS: Array[String] = ["start_session", "build_solana_payment", "report_hint", "set_ui_status",
	"discard_session", "tick", "reverify", "restore_lookup", "restore_apply"]
## The actions ScfgEngine implements: all of them.
const ENGINE_ACTIONS: Array[String] = ["start_session", "build_solana_payment", "report_hint", "set_ui_status",
	"discard_session", "tick", "reverify", "restore_lookup", "restore_apply"]
const _VOID_ACTIONS: Array[String] = ["report_hint", "set_ui_status", "discard_session"]


## Whether this core can replay `vector` (see the header).
static func ENABLED(vector: Variant) -> bool:
	if not (vector is Dictionary and vector.get("steps") is Array and vector.get("setup") is Dictionary):
		return false
	for step: Variant in vector["steps"]:
		if not (step is Dictionary and step.get("action") in ENGINE_ACTIONS):
			return false
	return vector["setup"].get("config") is Dictionary


# --- values that JSON cannot carry exactly -------------------------------------------------------


## `{"float": repr}` -> a float (Python's float(repr)); anything else as is.
## @fallible
static func decode_value(v: Variant) -> Variant:
	return Tier1._value(v)


## {k: decode_value(v)} (a new Dictionary).
## @fallible
static func _decoded(d: Variant) -> Variant:
	if not d is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'items'" % ScfgPy.py_type(d))
	var out := {}
	for k: Variant in d:
		var v: Variant = decode_value(d[k])
		if v is ScfgErr:
			return v
		out[k] = v
	return out


# --- engine ----------------------------------------------------------------------------------------


## The Config of a vector's setup: build_chain for each custom chain, build_config, then add_item
## in order.
## @fallible
static func build_config_from(setup: Dictionary) -> Variant:
	var kw: Variant = _decoded(setup["config"])
	if kw is ScfgErr:
		return kw
	if not (setup["custom_chains"] as Array).is_empty():
		var built := []
		for c: Variant in setup["custom_chains"]:
			var ckw: Variant = _decoded(c)
			if ckw is ScfgErr:
				return ckw
			var key: Variant = ckw.get("key")
			(ckw as Dictionary).erase("key")
			var chain: Variant = ScfgConfig.build_chain(key, ckw)
			if chain is ScfgErr:
				return chain
			built.append(chain)
		kw["custom_chains"] = built
	var config: Variant = ScfgConfig.build_config(kw)
	if config is ScfgErr:
		return config
	for item: Variant in setup["items"]:
		var ikw: Variant = _decoded(item)
		if ikw is ScfgErr:
			return ikw
		var item_id: Variant = ikw.get("item_id")
		(ikw as Dictionary).erase("item_id")
		var added: Variant = ScfgConfig.add_item(config, item_id, ikw)
		if added is ScfgErr:
			return added
	return config


## The Engine of a vector's setup. One clock drives the Engine and every RPC client: the
## rpc_factory builds each chain's client as the default one does, then pushes `clock` into it
## (a Solana client wraps its ScfgRpcClient as `client`).
## @fallible
static func build_engine(setup: Dictionary, transport: Variant, clock: Callable, on_error: Callable,
		entropy: Variant) -> Variant:
	var config: Variant = build_config_from(setup)
	if config is ScfgErr:
		return config
	var rpc_factory := func(chain: Dictionary) -> Variant:
		var backend: Variant = ScfgBackends.backend_for(chain)
		if backend is ScfgErr:
			return backend
		var rpc: Variant = (backend as ScfgBaseBackend).make_rpc(chain, transport, clock)
		if rpc is ScfgErr:
			return rpc
		var client: Variant = (rpc as Object).get("client")
		if client is ScfgRpcClient:
			(client as ScfgRpcClient).clock = clock
		else:
			(rpc as Object).set("clock", clock)
		return rpc
	return ScfgEngine.create(config, {"transport": transport, "pending": (setup["pending"] as Dictionary).duplicate(true),
		"receipts": (setup["receipts"] as Dictionary).duplicate(true), "clock": clock, "rpc_factory": rpc_factory,
		"on_error": on_error, "entropy": entropy})


# --- restore candidates --------------------------------------------------------------------------


## candidate_from_json: the inverse of the encoding of `restore_lookup`'s returns. The transfer's
## `class` (Transfer or TransferLog) picks the dataclass; wrong fields are a TypeError, an unknown
## class a KeyError, as in Python. A TransferLog's int value becomes the ScfgBigInt its contract
## names (ScfgEvm.parse_transfer_log); a Transfer's int value stays an int (its contract takes
## either); any other value is kept as it is, as Python's dataclass keeps it.
## @fallible
static func candidate_from_json(d: Variant) -> Variant:
	var transfer_json: Variant = ScfgPy.py_get(d, "transfer")
	if transfer_json is ScfgErr:
		return transfer_json
	if not transfer_json is Dictionary:
		return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(transfer_json))
	var fields: Dictionary = (transfer_json as Dictionary).duplicate()
	var cls: Variant = ScfgPy.py_get(fields, "class")
	if cls is ScfgErr:
		return cls
	fields.erase("class")
	var transfer: Variant
	if ScfgPy.py_eq(cls, "Transfer"):
		transfer = ScfgTransfers.make_transfer(fields)
	elif ScfgPy.py_eq(cls, "TransferLog"):
		if fields.get("value") is int and fields["value"] >= 0:
			fields["value"] = ScfgBigInt.from_int(fields["value"])  # err-ok: a non-negative int
		transfer = ScfgModels.dataclass("TransferLog", ScfgEvm.TRANSFER_LOG_FIELDS, {}, fields)
	else:
		transfer = ScfgErr.make("KeyError", ScfgPy.py_repr(cls))
	if transfer is ScfgErr:
		return transfer
	var out := {}
	for f: String in ["chain", "token"]:
		var v: Variant = ScfgPy.py_get(d, f)
		if v is ScfgErr:
			return v
		out[f] = v
	out["transfer"] = transfer
	for f: String in ["kind", "item", "choices", "already_restored"]:
		var v: Variant = ScfgPy.py_get(d, f)
		if v is ScfgErr:
			return v
		if f == "choices":
			v = ScfgPy.py_iter(v)  # tuple(d["choices"])
			if v is ScfgErr:
				return v
			v = (v as Array).duplicate()
		out[f] = v
	return out


## candidate_to_json for one candidate: the candidate as plain JSON, its transfer tagged with the
## Python class name (a TransferLog carries a block_hash, a Transfer does not).
## @fallible
static func candidate_to_json(c: Variant) -> Variant:
	var out: Variant = to_json(c)
	if out is ScfgErr:
		return out
	var transfer: Dictionary = out["transfer"]
	transfer["class"] = "TransferLog" if (c["transfer"] as Dictionary).has("block_hash") else "Transfer"
	return out


# --- actions -------------------------------------------------------------------------------------


## Run one action: [result, raises, problem]. `raises` is null or the vector's `raises` value;
## `problem` is "" or why the harness itself could not run the action. `steps` are the steps
## recorded before this one (restore_apply reads one).
static func run_action(engine: ScfgEngine, action: String, args: Dictionary, steps: Array) -> Array:
	var decoded: Variant = _decoded(args)
	if decoded is ScfgErr:
		return [null, null, "args: %s" % str(decoded)]  # Python decodes outside its try
	var a: Dictionary = decoded
	var result: Variant = null
	match action:
		"start_session":
			var kind: Variant = ScfgPy.py_get(a, "kind")
			var chain: Variant = ScfgPy.py_get(a, "chain")
			if kind is ScfgErr or chain is ScfgErr:
				result = kind if kind is ScfgErr else chain
			else:
				a.erase("kind")
				a.erase("chain")
				result = await engine.start_session(kind, chain, a)
		"build_solana_payment":
			var sid: Variant = ScfgPy.py_get(a, "sid")
			var account: Variant = ScfgPy.py_get(a, "account")
			if sid is ScfgErr or account is ScfgErr:
				result = sid if sid is ScfgErr else account
			else:
				result = await engine.build_solana_payment(sid, account)
		"report_hint", "set_ui_status":
			var sid: Variant = ScfgPy.py_get(a, "sid")
			var second: Variant = ScfgPy.py_get(a, "tx" if action == "report_hint" else "status")
			if sid is ScfgErr or second is ScfgErr:
				result = sid if sid is ScfgErr else second
			elif action == "report_hint":
				result = engine.report_hint(sid, second)
			else:
				result = engine.set_ui_status(sid, second)
		"discard_session":
			var sid: Variant = ScfgPy.py_get(a, "sid")
			result = sid if sid is ScfgErr else engine.discard_session(sid)
		"tick":
			result = await engine.tick()
		"reverify":
			result = await engine.reverify()
		"restore_lookup":
			var tx: Variant = ScfgPy.py_get(a, "tx")
			result = tx if tx is ScfgErr else await engine.restore_lookup(tx)
		"restore_apply":
			result = _restore_apply(engine, a, steps)
		_:
			if action in ACTIONS:
				return [null, null, "action %s is not implemented by the GDScript Engine yet" % action]
			return [null, null, "unknown action %s" % ScfgPy.py_repr(action)]
	if result is ScfgErr:
		var e: ScfgErr = result
		if e.is_a("ConfigError"):
			return [null, {"type": "ConfigError", "code": e.code}, ""]
		if e.is_a("RestoreError"):
			return [null, {"type": "RestoreError", "code": e.code}, ""]
		return [null, {"type": "other"}, ""]
	if result == null and not action in _VOID_ACTIONS:
		return [null, null, "%s returned null (aborted by a script error, or a coroutine not awaited?)" % action]
	return [result, null, ""]


## `restore_apply(from_step, choices)`: the candidates recorded as `returns` of step `from_step`;
## `choices` keys are candidate indexes as strings.
## @fallible
static func _restore_apply(engine: ScfgEngine, a: Dictionary, steps: Array) -> Variant:
	var from_step: Variant = ScfgPy.py_get(a, "from_step")
	if from_step is ScfgErr:
		return from_step
	var step: Variant = ScfgPy.py_get(steps, from_step)
	if step is ScfgErr:
		return step
	var recorded: Variant = ScfgPy.py_get(step, "returns")
	if recorded is ScfgErr:
		return recorded
	var seq: Variant = ScfgPy.py_iter(recorded)
	if seq is ScfgErr:
		return seq
	var candidates := []
	for d: Variant in seq:
		var c: Variant = candidate_from_json(d)
		if c is ScfgErr:
			return c
		candidates.append(c)
	var choices: Variant = ScfgPy.py_get(a, "choices")
	if choices is ScfgErr:
		return choices
	if choices != null:
		var items: Variant = ScfgPy.py_iter(choices)  # {int(k): v for k, v in choices.items()}
		if items is ScfgErr:
			return items
		if not choices is Dictionary:
			return ScfgErr.make("AttributeError", "'%s' object has no attribute 'items'" % ScfgPy.py_type(choices))
		var keyed := {}
		for k: Variant in choices:
			var i: Variant = ScfgPy.py_int(k)
			if i is ScfgErr:
				return i
			keyed[i] = choices[k]
		choices = keyed
	return engine.restore_apply(candidates, choices)


## canonical.to_jsonable for an Engine value: an ScfgDec is its `str()`, an ScfgBigInt or int
## below 2^53 in magnitude is a JSON int (anything larger cannot be recorded and fails), an
## integral float is an int, and a non-integral float, a non-string key, bytes or any other type
## fail.
## @fallible
static func to_json(v: Variant, path: String = "") -> Variant:
	var where := path if path != "" else "<root>"
	match typeof(v):
		TYPE_NIL, TYPE_BOOL, TYPE_STRING:
			return v
		TYPE_STRING_NAME:
			return str(v)
		TYPE_INT:
			if v >= ScfgPy.TWO_53 or v <= -ScfgPy.TWO_53:
				return ScfgErr.make("ValueError", "integer %d at %s is not safe in JSON (|n| >= 2**53)" % [v, where])
			return v
		TYPE_FLOAT:
			var f: float = v
			if is_nan(f) or is_inf(f) or f != floorf(f) or absf(f) >= float(ScfgPy.TWO_53):
				return ScfgErr.make("ValueError", "float %s at %s is not an integral JSON number" % [ScfgPy.py_repr(f), where])
			return int(f)
		TYPE_OBJECT:
			if v is ScfgDec:
				return str(v)
			if v is ScfgBigInt:
				var big: ScfgBigInt = v
				if big.bit_length() > 53:  # 2^53 itself has 54 bits
					return ScfgErr.make("ValueError", "integer %s at %s is not safe in JSON (|n| >= 2**53)" % [
						big.to_decimal(), where])
				return big.to_int()
		TYPE_ARRAY:
			var out := []
			for i in (v as Array).size():
				var x: Variant = to_json(v[i], "%s[%d]" % [path, i])
				if x is ScfgErr:
					return x
				out.append(x)
			return out
		TYPE_DICTIONARY:
			var out := {}
			for k: Variant in v:
				if not (k is String or k is StringName):
					return ScfgErr.make("TypeError", "non-string key %s at %s" % [ScfgPy.py_repr(k), where])
				var x: Variant = to_json(v[k], "%s.%s" % [path, k] if path != "" else str(k))
				if x is ScfgErr:
					return x
				out[str(k)] = x
			return out
	return ScfgErr.make("TypeError", "unsupported value %s (%s) at %s" % [str(v), ScfgPy.py_type(v), where])


## @fallible
static func encode_returns(action: String, result: Variant) -> Variant:
	if action == "restore_lookup" and result is Array:
		var out := []
		for c: Variant in result:
			var j: Variant = candidate_to_json(c)
			if j is ScfgErr:
				return j
			out.append(j)
		return out
	return to_json(result)


## pending, receipts, owned (sorted), tip_total (str), unready (sorted [chain, token] pairs).
## @fallible
static func snapshot_json(engine: ScfgEngine) -> Variant:
	var snap: Variant = engine.snapshot()
	if snap is ScfgErr:
		return snap
	var pending: Variant = to_json(snap["pending"], "pending")
	if pending is ScfgErr:
		return pending
	var receipts: Variant = to_json(snap["receipts"], "receipts")
	if receipts is ScfgErr:
		return receipts
	var owned: Array = (snap["owned"] as Array).duplicate()
	owned.sort()
	var unready: Array = (snap["unready"] as Array).duplicate(true)
	unready.sort_custom(func(x: Array, y: Array) -> bool: return x[0] < y[0] or (x[0] == y[0] and x[1] < y[1]))
	return {"pending": pending, "receipts": receipts, "owned": owned, "tip_total": str(snap["tip_total"]),
		"unready": unready}


# --- entropy -------------------------------------------------------------------------------------


## Hands out one step's recorded draws in order; any difference is a problem (and an
## AssertionError to the core).
class ScriptedEntropy:
	extends RefCounted
	var _draws: Array
	var problems: Array[String] = []

	func _init(draws: Array) -> void:
		_draws = draws.duplicate(true)

	## @fallible
	func _take(fn: String, n: int) -> Variant:
		if _draws.is_empty():
			problems.append("entropy exhausted at %s(%d)" % [fn, n])
			return ScfgErr.make("AssertionError", problems[-1])
		var draw: Dictionary = _draws.pop_front()
		if draw["fn"] != fn or T.first_diff(draw["n"], n) != "":
			problems.append("entropy mismatch: drew %s(%d), recorded %s(%s)" % [fn, n, draw["fn"], draw["n"]])
			return ScfgErr.make("AssertionError", problems[-1])
		return draw["value"]

	## @fallible
	func token_hex(n: int) -> Variant:
		return _take("token_hex", n)

	## @fallible
	func randbelow(n: int) -> Variant:
		return _take("randbelow", n)

	## @fallible
	func token_bytes(n: int) -> Variant:
		var v: Variant = _take("token_bytes", n)
		if v is ScfgErr:
			return v
		return Tier1.bytes_fromhex(v)

	func leftover() -> Array[String]:
		if _draws.is_empty():
			return []
		var left := PackedStringArray()
		for d: Dictionary in _draws:
			left.append("(%s, %s)" % [d["fn"], d["n"]])
		return ["unconsumed entropy: [%s]" % ", ".join(left)]


## Swaps the draws per step while the Engine keeps one entropy object.
class StepEntropy:
	extends RefCounted
	var current := ScriptedEntropy.new([])

	## @fallible
	func token_hex(n: int) -> Variant:
		return current.token_hex(n)

	## @fallible
	func randbelow(n: int) -> Variant:
		return current.randbelow(n)

	## @fallible
	func token_bytes(n: int) -> Variant:
		return current.token_bytes(n)


# --- transport -----------------------------------------------------------------------------------


## Answers each request from the current step's recorded calls, as a multiset: any order, each
## recorded call used once, identical requests take their copies in recorded order. The JSON-RPC
## id is ignored; params match by JSON value.
##
## Every request first waits `frames` process frames (1 by default), as a real transport does: a
## coroutine that never suspends returns its value even when the caller forgot `await`, so only
## a suspending transport lets the vectors catch a missing await.
class ReplayTransport:
	extends ScfgCoio.Transport
	var _calls: Array = []
	var _used: Array[bool] = []
	var problems: Array[String] = []
	var requests := 0
	var frames := 1

	func load(calls: Array) -> void:
		_calls = calls.duplicate(true)
		_used = []
		_used.resize(_calls.size())
		_used.fill(false)
		problems = []

	## @fallible
	func post_json(url: String, payload: Dictionary, _timeout: float) -> Variant:
		requests += 1
		for i in frames:
			await (Engine.get_main_loop() as SceneTree).process_frame
		for i in _calls.size():
			var c: Dictionary = _calls[i]
			if _used[i] or c["url"] != url or c["method"] != payload.get("method"):
				continue
			if T.first_diff(c["params"], payload.get("params")) != "":
				continue
			_used[i] = true
			if c.has("transport_error"):
				return ScfgCoio.transport_error(c["transport_error"]["message"], c["transport_error"]["status"])
			return ScfgCoio.response_for(200, ScfgCoio.parse_body((c["response_text"] as String).to_utf8_buffer()))
		problems.append("unexpected call %s %s %s" % [url, payload.get("method"), JSON.stringify(payload.get("params"))])
		return ScfgErr.make("AssertionError", problems[-1])

	func leftover() -> Array[String]:
		var out: Array[String] = []
		for i in _calls.size():
			if not _used[i]:
				var c: Dictionary = _calls[i]
				out.append("expected call not made: %s %s %s" % [c["url"], c["method"], JSON.stringify(c["params"])])
		return out


# --- replay --------------------------------------------------------------------------------------


## Re-run a Tier 2 vector: "" when every step agrees, otherwise "step {i} ({action}): ...".
static func replay(vector_json: Variant) -> String:
	if not (vector_json is Dictionary and T.first_diff(vector_json.get("tier"), 2) == ""):
		return "not a Tier 2 vector"
	var vector: Dictionary = ScfgPy.from_json(vector_json)
	var setup: Dictionary = vector["setup"]
	var cell := {"now": setup["clock"], "errors": 0}
	var clock := func() -> Variant: return cell["now"]
	var on_error := func(_e: Variant) -> void: cell["errors"] += 1
	var transport := ReplayTransport.new()
	var entropy := StepEntropy.new()
	var built: Variant = build_engine(setup, transport, clock, on_error, entropy)
	if built is ScfgErr:
		return "setup: %s" % str(built)
	var engine: ScfgEngine = built
	var steps: Array = vector["steps"]
	for i in steps.size():
		var step: Dictionary = steps[i]
		var at := "step %d (%s)" % [i, step["action"]]
		cell["now"] = step["at"]
		cell["errors"] = 0
		transport.load(step["calls"])
		entropy.current = ScriptedEntropy.new(step["entropy"])
		var ran: Array = await run_action(engine, step["action"], step["args"], steps.slice(0, i))
		var problems: Array[String] = []
		if ran[2] != "":
			problems.append(ran[2])
		problems.append_array(transport.problems)
		problems.append_array(transport.leftover())
		problems.append_array(entropy.current.problems)
		problems.append_array(entropy.current.leftover())
		if not problems.is_empty():
			return "%s: %s" % [at, "; ".join(PackedStringArray(problems))]
		var raises: Variant = ran[1]
		if T.first_diff(step["raises"], raises) != "":
			return "%s: raises: recorded %s, got %s" % [at, JSON.stringify(step["raises"]), JSON.stringify(raises)]
		var returns: Variant = null
		if raises == null:
			returns = encode_returns(step["action"], ran[0])
			if returns is ScfgErr:
				return "%s: returns: %s" % [at, str(returns)]
		var rd := T.first_diff(step["returns"], returns)
		if rd != "":
			return "%s: returns: %s (recorded vs got)" % [at, rd]
		if T.first_diff(step["internal_errors"], cell["errors"]) != "":
			return "%s: internal_errors: recorded %s, got %d" % [at, step["internal_errors"], cell["errors"]]
		var snap: Variant = snapshot_json(engine)
		if snap is ScfgErr:
			return "%s: snapshot: %s" % [at, str(snap)]
		var sd := T.first_diff(step["snapshot"], snap)
		if sd != "":
			return "%s: snapshot: %s (recorded vs got)" % [at, sd]
	return ""
