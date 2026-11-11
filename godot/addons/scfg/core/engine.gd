## The Engine composes config, sessions, watcher, receipts and restore. Mirrors
## scfg_core/engine.py.
##
## It owns all plugin state (deep copies of what the game persists) and is NOT re-entrant: only
## the single-writer worker calls it, one command at a time. Readers get Snapshots, which are
## detached copies.
##
## `ScfgEngine.new(config, opts)`, where `opts` holds Python's keyword arguments:
##   - transport: the transport every default RPC client posts through (see ScfgCoio);
##   - pending, receipts: the initial state (deep-copied; null is {}). It is normalised as
##     Python's json.load would read it (ScfgPy.from_json): Godot's JSON reader makes every number
##     a float, so state reloaded from user://scfg/state.json has `units`, `log_index`, `block`...
##     as floats; each integral one below 2^53 becomes an int again, nested values included. A
##     fractional float is left alone and fails validation as it would in Python;
##   - clock: a Callable returning seconds (the wall clock by default);
##   - rpc_factory: a Callable(chain: Dictionary) returning the chain's RPC object or an ScfgErr
##     (by default the chain's backend's make_rpc on the transport, whose RPC clients keep their
##     own monotonic clock for endpoint cooldowns, as Python's RpcClient defaults to
##     time.monotonic: the Engine's clock is the wall clock, which can jump);
##   - on_error: a Callable(err: ScfgErr) told of every crash it contained: a session step, a stored
##     receipt reverify could not check, a restore transfer it could not classify (by default the
##     error is printed to stderr, as Python's traceback hook does). It is read each time an
##     error is reported, so a handler replaced while a pass is in flight is the one told (the
##     Godot autoload detaches its own in `_exit_tree`);
##   - entropy: Python's `secrets` (see ScfgSessions; OS randomness by default).
## An unknown option, or one of the wrong type, is a TypeError: `create(config, opts)` returns it,
## and `new` keeps it in `init_error`, which every method then returns.
## Every injected Callable (clock, rpc_factory, on_error) must be synchronous: the Engine calls it
## without `await`, so one that awaits hands the Engine a function state instead of its value.
##
## Methods return their value or an ScfgErr. The ones that need the network are coroutines
## (`await` them): start_session, build_solana_payment, tick, reverify and restore_lookup. The
## others are plain: chain, rpc, report_hint, set_ui_status, discard_session, restore_apply,
## snapshot. (Python's `run`/`*_co` pairs collapse into one coroutine each: await replaces
## run_sync.)
##
## A Snapshot is a Dictionary with Python's fields in order: pending, receipts (deep copies),
## owned (the owned item ids, sorted: Python's frozenset), tip_total (an ScfgDec) and unready
## (the [chain, token] pairs whose token the developer cannot receive, sorted).
class_name ScfgEngine
extends RefCounted

const START_BLOCK_MARGIN := ScfgEvmBackend.START_BLOCK_MARGIN
const _OPTS: Array[String] = ["transport", "pending", "receipts", "clock", "rpc_factory", "on_error", "entropy"]
const _START_OPTS: Array[String] = ["item", "amount", "token", "title"]

static var MIN_TIP: ScfgDec = ScfgPayments.MIN_TIP

var config: Dictionary
var on_error: Callable
var pending: Dictionary = {}
var receipts: Dictionary = {}
var clock: Callable
var transport: Variant = null
var entropy: Variant = null
## The routes the readiness check found unusable: {[chain, token]: true} (Python's set).
var unready: Dictionary = {}
## The construction error (a TypeError), or null.
var init_error: Variant = null
var _rpc_factory: Callable
var _rpcs: Dictionary = {}


func _init(config_: Dictionary, opts: Dictionary = {}) -> void:
	config = config_
	clock = _wall_clock
	on_error = _print_error
	_rpc_factory = _default_rpc_factory
	entropy = ScfgSessions.OsEntropy.new()
	var bad: Variant = _check_opts(opts)
	if bad is ScfgErr:
		init_error = bad
		return
	# (from_json also deep-copies: the Engine never shares the game's dictionaries)
	if opts.get("pending") != null:
		pending = ScfgPy.from_json(opts["pending"])
	if opts.get("receipts") != null:
		receipts = ScfgPy.from_json(opts["receipts"])
	if opts.has("clock"):
		clock = opts["clock"]
	if opts.has("on_error"):
		on_error = opts["on_error"]
	if opts.has("rpc_factory"):
		_rpc_factory = opts["rpc_factory"]
	if opts.get("entropy") != null:
		entropy = opts["entropy"]
	transport = opts.get("transport")


## null when `Engine(config, **opts)` would construct, else its TypeError.
## @fallible
static func _check_opts(opts: Dictionary) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in _OPTS):
			return ScfgErr.make("TypeError", "Engine() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	for k: String in ["pending", "receipts"]:
		if opts.get(k) != null and not opts[k] is Dictionary:
			return ScfgErr.make("TypeError", "%s must be a dict, not %s" % [k, ScfgPy.py_type(opts[k])])
	for k: String in ["clock", "on_error", "rpc_factory"]:
		if opts.has(k) and not opts[k] is Callable:
			return ScfgErr.make("TypeError", "%s must be a Callable, not %s" % [k, ScfgPy.py_type(opts[k])])
	return null


## Python's `Engine(config, **opts)`: the Engine, or its TypeError.
## @fallible
static func create(config_: Dictionary, opts: Dictionary = {}) -> Variant:
	var e := ScfgEngine.new(config_, opts)
	if e.init_error != null:
		return e.init_error
	return e


static func _wall_clock() -> float:
	return Time.get_unix_time_from_system()


static func _print_error(err: Variant) -> void:
	printerr("ScfgEngine: contained error: %s" % str(err))


## Python's `hash(key)` failing: a list or dict cannot be a dict key or set member.
## @fallible
static func _hashable(key: Variant) -> Variant:
	if key is Array or key is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(key))
	return null


# --- chains & RPC -----------------------------------------------------------------------------


## The full registry chain (all tokens, so old receipts stay verifiable) with the config's
## overrides: its developer RPC urls first, its confirmations. A new Dictionary; the registry's is
## not touched. An unknown key is a KeyError.
## @fallible
func chain(key: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(key)
	if bad is ScfgErr:
		return bad
	var base: Variant = ScfgPy.py_get(ScfgConfig.registry(config), key)
	if base is ScfgErr:
		return base
	var c: Dictionary = (base as Dictionary).duplicate()
	c["rpc"] = (config["rpc_urls"].get(key, []) as Array) + (base["rpc"] as Array)
	c["confirmations"] = config["confirmations"].get(key, base["confirmations"])
	return c


## The chain's RPC object, built once per chain key by the rpc_factory.
## @fallible
func rpc(key: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(key)
	if bad is ScfgErr:
		return bad
	if not _rpcs.has(key):
		var c: Variant = chain(key)
		if c is ScfgErr:
			return c
		var made: Variant = _rpc_factory.call(c)
		if made is ScfgErr:
			return made
		if made == null:
			return ScfgErr.make("RuntimeError", "the rpc_factory returned null for %s" % ScfgPy.py_repr(key))
		_rpcs[key] = made
	return _rpcs[key]


## The default rpc_factory: the chain's backend's RPC object on this Engine's transport. Python's
## default factory passes no clock, so the RPC client keeps its own (ScfgRpcClient's monotonic
## clock, Python's time.monotonic) for endpoint cooldowns rather than the Engine's wall clock.
## @fallible
func _default_rpc_factory(c: Dictionary) -> Variant:
	var backend: Variant = ScfgBackends.backend_for(c)
	if backend is ScfgErr:
		return backend
	var typed: ScfgBaseBackend = backend
	return typed.make_rpc(c, transport, Callable())


# --- payments ---------------------------------------------------------------------------------


## Validate a payment request, open a session on `chain_key` and return a copy of its record.
## `opts` holds the keyword arguments item, amount, token and title (see
## ScfgPayments.prepare_payment); another key is a TypeError. A coroutine.
## @fallible
func start_session(kind: Variant, chain_key: Variant, opts: Dictionary = {}) -> Variant:
	if init_error != null:
		return init_error
	for k: Variant in opts:
		if not (k is String and k in _START_OPTS):
			return ScfgErr.make("TypeError", "start_session() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	var spec: Variant = ScfgPayments.prepare_payment(config, kind, {"item": opts.get("item"),
		"amount": opts.get("amount"), "token": opts.get("token"), "title": opts.get("title")})
	if spec is ScfgErr:
		return spec
	if not ScfgConfig.in_strs(spec["chains"], chain_key):
		return ScfgConfig.config_error("chain.token_unavailable", "%s is not available on chain %s" % [
			str(spec["token"]), ScfgPy.py_repr(chain_key)])
	var chain_obj: Variant = chain(chain_key)
	if chain_obj is ScfgErr:
		return chain_obj
	var backend: Variant = ScfgBackends.backend_for(chain_obj)
	if backend is ScfgErr:
		return backend
	var client: Variant = rpc(chain_key)
	if client is ScfgErr:
		return client
	var fields: Variant = await backend.start({"rpc": client, "chain": chain_obj,
		"wallet": ScfgConfig.wallet_for(config, chain_obj), "token": spec["token"], "entropy": entropy})
	if fields is ScfgErr:
		return fields
	if not fields is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'pop'" % ScfgPy.py_type(fields))
	var start: Variant = ScfgPy.py_get(fields, "start_block")
	if start is ScfgErr:
		return start
	(fields as Dictionary).erase("start_block")
	var rec: Variant = ScfgSessions.create_session(pending, {"kind": spec["kind"], "item": spec["item"],
		"title": spec["title"], "token": spec["token"], "chain": chain_key, "price": spec["price"], "code": spec["code"],
		"start_block": start, "now": clock.call(), "extra": fields}, entropy)
	if rec is ScfgErr:
		return rec
	return (rec as Dictionary).duplicate(true)


## The unsigned Solana transfer for a session (see ScfgSolanaBackend.build_payment): what the
## browser page hands the player's wallet, built for the player's `account`. An unknown session is
## a KeyError, a session on another family a ValueError. A coroutine.
## @fallible
func build_solana_payment(sid: Variant, account: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(sid)
	if bad is ScfgErr:
		return bad
	var rec: Variant = ScfgPy.py_get(pending, sid)
	if rec is ScfgErr:
		return rec
	var key: Variant = ScfgPy.py_get(rec, "chain")
	if key is ScfgErr:
		return key
	var c: Variant = chain(key)
	if c is ScfgErr:
		return c
	if not ScfgPy.py_eq(c["family"], "solana"):
		return ScfgErr.make("ValueError", "not a Solana session")
	var backend: Variant = ScfgBackends.backend_for(c)
	if backend is ScfgErr:
		return backend
	var client: Variant = rpc(c["key"])
	if client is ScfgErr:
		return client
	return await backend.build_payment(rec, {"rpc": client, "chain": c, "account": account})


## Record the transaction id the browser page reported (or the player pasted), normalized by
## the session's chain family. An unknown session is a KeyError; a malformed id a ValueError.
## @fallible
func report_hint(sid: Variant, tx_hash: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(sid)
	if bad is ScfgErr:
		return bad
	var rec: Variant = ScfgPy.py_get(pending, sid)
	if rec is ScfgErr:
		return rec
	var key: Variant = ScfgPy.py_get(rec, "chain")
	if key is ScfgErr:
		return key
	var c: Variant = chain(key)
	if c is ScfgErr:
		return c
	var backend: Variant = ScfgBackends.backend_for(c)
	if backend is ScfgErr:
		return backend
	var normalize: Callable = (backend as ScfgBaseBackend).normalize_tx
	return ScfgSessions.set_hint(pending, sid, tx_hash, clock.call(), normalize)


## Set a session's screen status; a session that is already gone is ignored.
## @fallible
func set_ui_status(sid: Variant, status: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(sid)
	if bad is ScfgErr:
		return bad
	if pending.has(sid):
		return ScfgSessions.set_ui_status(pending, sid, status)
	return null


## Drop a session the player abandoned before paying (e.g. switched network): only an awaiting
## one without a hint.
## @fallible
func discard_session(sid: Variant) -> Variant:
	if init_error != null:
		return init_error
	var bad: Variant = _hashable(sid)
	if bad is ScfgErr:
		return bad
	var rec: Variant = pending.get(sid)
	if rec == null:
		return null
	var state: Variant = ScfgPy.py_get(rec, "state")
	if state is ScfgErr:
		return state
	if not ScfgPy.py_eq(state, "awaiting"):
		return null
	var hint: Variant = ScfgPy.py_get(rec, "hint_tx")
	if hint is ScfgErr:
		return hint
	if not ScfgPy.py_truthy(hint):
		pending.erase(sid)
	return null


## Step every session, one after another in pending order (never concurrently), then report the
## payment windows that closed. Returns the StepOutcomes. A coroutine.
## @fallible
func tick() -> Variant:
	if init_error != null:
		return init_error
	var now: Variant = clock.call()
	var outcomes := []
	var items := []
	for sid: Variant in pending:
		items.append([sid, pending[sid]])
	for pair: Array in items:
		var outcome: Variant = await _advance(pair[0], pair[1], now)
		if outcome is ScfgErr:
			return outcome  # not an Exception (a CancelledError): Python does not contain it
		if outcome != null:
			outcomes.append(outcome)
	var expired: Variant = _expire(now)
	if expired is ScfgErr:
		return expired
	outcomes.append_array(expired)
	return outcomes


## One session's step. A session this build can no longer watch (its chain left the registry, or
## the game stopped accepting that family) is kept until its scan horizon (null: no outcome),
## then closed. A failure in one session never stops the others: see _contained.
## @fallible: only a CancelledError, which Python does not contain either
func _advance(sid: Variant, rec: Variant, now: Variant) -> Variant:
	var key: Variant = ScfgPy.py_get(rec, "chain")
	if key is ScfgErr:
		return _contained(sid, key)
	var bad: Variant = _hashable(key)
	if bad is ScfgErr:
		return _contained(sid, bad)
	var watchable := ScfgConfig.registry(config).has(key)
	if watchable:
		var c: Variant = chain(key)
		if c is ScfgErr:
			return _contained(sid, c)
		watchable = ScfgConfig.wallet_for(config, c) != null
	if not watchable:
		var past: Variant = ScfgSessions.is_past_horizon(rec, now)
		if past is ScfgErr:
			return _contained(sid, past)
		if not past:
			return null
		ScfgSessions.remove_session(pending, sid)
		return ScfgWatcher.step_outcome(sid, "closed")
	var chain_obj: Variant = chain(key)
	if chain_obj is ScfgErr:
		return _contained(sid, chain_obj)
	var backend: Variant = ScfgBackends.backend_for(chain_obj)
	if backend is ScfgErr:
		return _contained(sid, backend)
	var client: Variant = rpc(key)
	if client is ScfgErr:
		return _contained(sid, client)
	var result: Variant = await backend.advance(pending, receipts, sid, {"rpc": client, "chain": chain_obj,  # err-ok: _contained takes the ScfgErr
		"wallet": ScfgConfig.wallet_for(config, chain_obj), "items": config["items"], "now": now})
	return _contained(sid, result)


## Python's `except Exception as e: self.on_error(e); return StepOutcome(sid, "waiting",
## error="internal_error")`. A step's StepOutcome passes through. A returned ScfgErr is
## contained, and so is a step that returned anything else (null: a coroutine a script error
## aborted), as a RuntimeError. A CancelledError (not an Exception) is returned uncontained.
## @fallible: only a CancelledError
func _contained(sid: Variant, result: Variant) -> Variant:
	if result is Dictionary:
		return result
	var err: ScfgErr
	if result is ScfgErr:
		err = result
		if not err.is_a("Exception"):
			return err
	else:
		err = ScfgErr.make("RuntimeError", "the session step returned %s (aborted by a script error?)" % ScfgPy.py_type(result))
	on_error.call(err)
	var out: Variant = ScfgWatcher.step_outcome(sid, "waiting", {"error": "internal_error"})  # err-ok: valid fields
	return out


## Report the 30-minute payment window closing (scanning continues up to 24 h).
## @fallible
func _expire(now: Variant) -> Variant:
	var out := []
	for sid: Variant in pending:
		var rec: Variant = pending[sid]
		var ui: Variant = ScfgPy.py_get(rec, "ui_status")
		if ui is ScfgErr:
			return ui
		if not ScfgConfig.in_strs(["open", "pending"], ui):
			continue
		var past: Variant = ScfgSessions.is_past_window(rec, now)
		if past is ScfgErr:
			return past
		if not past:
			continue
		rec["ui_status"] = "expired"
		var fields := {"status": "expired", "session_id": sid}
		for f: String in ["kind", "item", "token", "chain"]:
			var v: Variant = ScfgPy.py_get(rec, f)
			if v is ScfgErr:
				return v
			fields[f] = v
		var result: Variant = ScfgModels.make_payment_result(fields)
		if result is ScfgErr:
			return result
		var outcome: Variant = ScfgWatcher.step_outcome(sid, "expired", {"result": result})
		if outcome is ScfgErr:
			return outcome
		out.append(outcome)
	return out


# --- receipts ---------------------------------------------------------------------------------


## Defers building a chain's RPC client until a backend actually calls it, so chains whose
## readiness needs no network (EVM) never construct a client. Python's `_LazyRpc` forwards every
## attribute with `__getattr__`; GDScript cannot, so each method a backend's check_ready calls has
## an explicit forwarder (Solana's: `account_exists`; add one here for a new check_ready). As in
## Python, the client is built (`engine.rpc(key)`: its error is returned) at the first call, and a
## method the client does not have is Python's AttributeError, never a script error.
class LazyRpc:
	extends RefCounted
	var _engine: ScfgEngine
	var _key: Variant

	func _init(engine_: ScfgEngine, key_: Variant) -> void:
		_engine = engine_
		_key = key_

	## `getattr(engine.rpc(key), name)(*args)`, awaited.
	## @fallible
	func _forward(name: String, args: Array) -> Variant:
		var client: Variant = _engine.rpc(_key)
		if client is ScfgErr:
			return client
		if not (client is Object and (client as Object).has_method(name)):
			return ScfgErr.make("AttributeError", "'%s' object has no attribute '%s'" % [ScfgPy.py_type(client), name])
		return await (client as Object).callv(name, args)

	## @fallible
	func account_exists(address: Variant) -> Variant:
		return await _forward("account_exists", [address])


## Launch checks: re-verify stored receipts, then check that the developer can receive each
## enabled token (Solana needs a token account). Returns the removed receipt keys, in store
## order. A coroutine.
## @fallible
func reverify() -> Variant:
	if init_error != null:
		return init_error
	var chains := {}
	for key: Variant in ScfgConfig.registry(config):
		var c: Variant = chain(key)
		if c is ScfgErr:
			return c
		chains[key] = c
	var wallets := {}
	for family: String in ["evm", "solana"]:
		wallets[family] = ScfgConfig.accepted_wallets_for(config, family)
	var removed: Variant = await ScfgReverify.reverify_receipts(receipts, {"rpc_for": rpc, "chains": chains,
		"wallets": wallets, "on_error": _report})
	if removed is ScfgErr:
		return removed
	var checked: Variant = await _check_ready()
	if checked is ScfgErr:
		return checked
	return removed


## Replace `unready` with the routes whose token the developer cannot receive. A chain whose check
## fails with network trouble is never hidden on doubt; any other failure is reported to
## `on_error` and the chain is skipped too. Only a CancelledError (not an Exception) propagates.
## @fallible
func _check_ready() -> Variant:
	var routes := {}
	var enabled := ScfgConfig.enabled_chains(config)
	for key: Variant in enabled:
		var chain_obj: Dictionary = enabled[key]
		var ready: Variant
		var backend: Variant = ScfgBackends.backend_for(chain_obj)
		if backend is ScfgErr:
			ready = backend
		else:
			ready = await backend.check_ready({"rpc": LazyRpc.new(self, key), "chain": chain_obj,
				"wallet": ScfgConfig.wallet_for(config, chain_obj)})
		if ready is ScfgErr or ready == null:
			var err: Variant = ready
			if err == null:
				err = ScfgErr.make("RuntimeError", "check_ready returned null (aborted by a script error?)")
			if err.is_a("RpcUnavailable") or err.is_a("RpcError"):
				continue  # never hide a payment option on doubt
			if not err.is_a("Exception"):
				return err
			on_error.call(err)
			continue
		if not ready is Dictionary:
			return ScfgErr.make("AttributeError", "'%s' object has no attribute 'items'" % ScfgPy.py_type(ready))
		for token: Variant in ready:
			if not ScfgPy.py_truthy(ready[token]):
				routes[[key, token]] = true
	unready = routes
	return null


## The purchases a transaction hash paid for, as RestoreCandidates (see ScfgRestore): the enabled
## chains are asked first, then the rest of the registry. A RestoreError when there are none. A
## coroutine.
## @fallible
func restore_lookup(tx_hash: Variant) -> Variant:
	if init_error != null:
		return init_error
	var registry := ScfgConfig.registry(config)
	var ordered: Array = (config["chains"] as Array).duplicate()
	for key: Variant in registry:
		if not ordered.has(key):
			ordered.append(key)
	var chains := {}
	for key: Variant in ordered:
		var c: Variant = chain(key)
		if c is ScfgErr:
			return c
		chains[key] = c
	return await ScfgRestore.lookup(tx_hash, {"config": config, "chains": chains, "rpc_for": rpc,
		"receipts": receipts, "on_error": _report})


## The on_error handed to reverify and restore_lookup, which report as they go: it calls `on_error`
## as it is when the error is reported, not as it was when the pass started (through
## ScfgErr.report, so one that is no longer a valid Callable prints the error instead).
func _report(err: ScfgErr) -> void:
	ScfgErr.report(on_error, err)


## Store receipts for the candidates (see ScfgRestore.apply); returns the ones added.
## @fallible
func restore_apply(candidates: Variant, choices: Variant = null) -> Variant:
	if init_error != null:
		return init_error
	return ScfgRestore.apply(receipts, candidates, choices)


## What readers see: detached copies (see the header). A stored tip total beyond Decimal's range
## is an Overflow, as in Python's compute_ownership.
## @fallible
func snapshot() -> Variant:
	if init_error != null:
		return init_error
	var ownership: Variant = ScfgReceipts.compute_ownership(receipts, config["items"])
	if ownership is ScfgErr:
		return ownership
	var owned: Array = (ownership[0] as Array).duplicate()
	owned.sort()
	var routes: Array = unready.keys().map(func(k: Array) -> Array: return k.duplicate())
	routes.sort_custom(func(x: Array, y: Array) -> bool: return x[0] < y[0] or (x[0] == y[0] and x[1] < y[1]))
	return {"pending": pending.duplicate(true), "receipts": receipts.duplicate(true), "owned": owned,
		"tip_total": ownership[1], "unready": routes}
