## ScfgEngine (engine.gd): tests/unit/test_engine.py (the Solana half: test_engine_solana.gd),
## plus the JS core's checks (rpgmaker/src/core/engine.test.ts): the default RPC factory, the
## contained crash, sequential ticks and the injected entropy.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"

static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


## [engine, chains {key: FakeChain}, clock] (test_engine.py `make_engine`).
func _make_engine(cfg_kw: Dictionary = {}, opts: Dictionary = {}) -> Array:
	var kw := {"wallet": DEV}
	kw.merge(cfg_kw, true)
	var cfg: Variant = ScfgConfig.build_config(kw)
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var chains := {}
	var factory := func(chain: Dictionary) -> Variant:
		if not chains.has(chain["key"]):
			chains[chain["key"]] = F.FakeChain.new(1000)
		chains[chain["key"]].chain_obj = chain
		return chains[chain["key"]]
	var clock := F.FakeClock.new(100.0)
	var o := {"rpc_factory": factory, "clock": clock.fn}
	o.merge(opts, true)
	return [ScfgEngine.new(cfg, o), chains, clock]


func _statuses(outcomes: Variant) -> Array:
	return (outcomes as Array).map(func(o: Dictionary) -> String: return o["status"])


func test_start_unlock_session_uses_item_defaults() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	T.ok(rec is Dictionary, "a record")
	if not rec is Dictionary:
		return T.done()
	T.eq([rec["kind"], rec["item"], rec["token"], rec["title"], rec["code"]], ["unlock", "chapter2", "USDC", "Chapter 2", 37],
		"item defaults")
	T.eq(rec["price"], "4.99", "price")
	T.eq(rec["start_block"], 1000 - ScfgEvmBackend.START_BLOCK_MARGIN, "start block")
	T.eq(rec["created_at"], 100.0, "created at the Engine's clock")
	T.ok(e.pending.has(rec["id"]), "stored")
	return T.done()


func test_start_session_returns_a_copy() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	rec["state"] = "hacked"
	T.eq(e.pending[rec["id"]]["state"], "awaiting", "the Engine's record is untouched")
	return T.done()


func test_start_tip_session() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var rec: Variant = await e.start_session("tip", "base", {"amount": "3", "token": "USDC"})
	T.eq([rec["code"], rec["title"], rec["price"], rec["item"]], [0, "Tip", "3.00", null], "a tip")
	return T.done()


func test_start_session_rejects() -> bool:
	var below_min: ScfgDec = ScfgPayments.MIN_TIP.sub(ScfgDec.parse("0.01"))
	var cases := [
		["unlock", "arbitrum", {"item": "missing"}, "item.unknown"],
		["unlock", "arbitrum", {"item": "chapter2", "amount": "1.00"}, "payment.below_min_price"],
		["tip", "arbitrum", {"token": "USDC"}, "tip.needs_amount_and_token"],
		["tip", "arbitrum", {"amount": "1"}, "tip.needs_amount_and_token"],
		["tip", "arbitrum", {"amount": str(below_min), "token": "USDC"}, "tip.below_minimum"],
		["refund", "arbitrum", {}, "payment.bad_kind"],
		["tip", "base", {"amount": "3", "token": "USDT"}, "chain.token_unavailable"],  # no USDT on base
		["tip", "solana", {"amount": "3", "token": "USDC"}, "chain.token_unavailable"],
	]
	for c: Array in cases:
		var e: ScfgEngine = _make_engine()[0]
		var got: Variant = await e.start_session(c[0], c[1], c[2])
		T.is_err(got, "ConfigError", "%s" % [c])
		T.eq(got.code if got is ScfgErr else null, c[3], "code of %s" % [c])
		T.eq(e.pending, {}, "nothing stored for %s" % [c])
	return T.done()


func test_start_session_rejects_float_amount() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	T.is_err(await e.start_session("tip", "base", {"amount": 3.0, "token": "USDC"}), "TypeError", "3.0")
	return T.done()


func test_start_session_rejects_unknown_keyword() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	T.is_err(await e.start_session("tip", "base", {"amount": "3", "token": "USDC", "colour": "red"}), "TypeError",
		"an unexpected keyword argument")
	T.eq(e.pending, {}, "nothing stored")
	return T.done()


func test_start_session_rejects_disabled_chain() -> bool:
	var e: ScfgEngine = _make_engine({"chains": ["base"]})[0]
	T.is_err(await e.start_session("unlock", "arbitrum", {"item": "chapter2"}), "ConfigError", "arbitrum is off")
	return T.done()


func test_chain_applies_config_overrides() -> bool:
	var e: ScfgEngine = _make_engine({"rpc_urls": {"arbitrum": ["https://mine"]}, "confirmations": {"arbitrum": 2}})[0]
	var c: Variant = e.chain("arbitrum")
	T.eq([c["rpc"][0], c["confirmations"]], ["https://mine", 2], "overrides")
	T.eq((c["tokens"] as Dictionary).keys(), ["USDC", "USDT"], "the full registry: old receipts stay verifiable")
	T.eq(ScfgChains.MAINNET["arbitrum"]["rpc"][0], "https://arb1.arbitrum.io/rpc", "the registry is untouched")
	T.is_err(e.chain("nochain"), "KeyError", "an unknown chain")
	return T.done()


func test_rpc_clients_are_cached_per_chain() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	T.ok(is_same(e.rpc("arbitrum"), e.rpc("arbitrum")), "cached")
	T.ok(not is_same(e.rpc("arbitrum"), e.rpc("base")), "per chain")
	return T.done()


## Python's default factory is `backend_for(chain).make_rpc(chain)`: the RpcClient keeps its
## default clock, time.monotonic, for endpoint cooldowns, not the Engine's wall clock.
func _assert_monotonic_clock(c: ScfgRpcClient, engine_clock: Callable, what: String) -> void:
	T.ok(c.clock != engine_clock, "%s: not on the Engine's clock" % what)
	var before := Time.get_ticks_usec() / 1000000.0
	var now: float = c.clock.call()
	var after := Time.get_ticks_usec() / 1000000.0
	T.ok(now >= before and now <= after, "%s: on the monotonic ticks clock (%f in [%f, %f])" % [what, now, before, after])


func test_default_rpc_factory_builds_an_rpc_client_on_a_monotonic_clock() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	var transport := F.FakeTransport.new({})
	var clock := func() -> float: return 42.0
	var e := ScfgEngine.new(cfg, {"transport": transport, "clock": clock})
	var rpc: Variant = e.rpc("arbitrum")
	T.ok(rpc is ScfgRpcClient, "an ScfgRpcClient")
	if rpc is ScfgRpcClient:
		var c: ScfgRpcClient = rpc
		_assert_monotonic_clock(c, clock, "EVM")
		T.ok(is_same(c.transport, transport), "with the Engine's transport")
		T.eq(Array(c.urls), ScfgChains.MAINNET["arbitrum"]["rpc"], "the chain's urls")
		T.eq(c.log_range, ScfgChains.MAINNET["arbitrum"]["max_log_range"], "the chain's log range")
	var sol: Variant = e.rpc("solana")
	T.ok(sol is ScfgSolanaRpc and (sol as ScfgSolanaRpc).client is ScfgRpcClient, "a Solana client")
	if sol is ScfgSolanaRpc:
		_assert_monotonic_clock((sol as ScfgSolanaRpc).client, clock, "Solana")
	return T.done()


func test_end_to_end_tick_pays_and_updates_snapshot() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var chain: F.FakeChain = r[1]["arbitrum"]
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, rec["units"], {"block": 999, "tx_hash": TX}))
	T.eq(_statuses(await e.tick()), ["confirming"], "confirming")
	T.ok(not "chapter2" in e.snapshot()["owned"], "not owned yet")
	chain.head = 999 + 19
	var outcomes: Variant = await e.tick()
	T.eq(outcomes[0]["status"], "paid", "paid")
	var snap: Variant = e.snapshot()
	T.eq(snap["owned"], ["chapter2"], "owned")
	T.eq(snap["pending"], {}, "no pending")
	T.eq((snap["receipts"] as Dictionary).size(), 1, "one receipt")
	T.eq(str(snap["tip_total"]), "0", "no tips")
	T.eq(snap["unready"], [], "nothing unready")
	T.eq((snap as Dictionary).keys(), ["pending", "receipts", "owned", "tip_total", "unready"], "Snapshot fields")
	return T.done()


func test_report_hint_and_ui_status() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	T.eq(e.report_hint(rec["id"], TX.to_upper().replace("0X", "0x")), null, "report_hint")
	T.eq(e.set_ui_status(rec["id"], "pending"), null, "set_ui_status")
	var stored: Dictionary = e.pending[rec["id"]]
	T.eq([stored["hint_tx"], stored["hint_at"], stored["ui_status"]], [TX, 100.0, "pending"], "stored")
	return T.done()


func test_report_hint_rejects_bad_hash_and_unknown_session() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	T.is_err(e.report_hint(rec["id"], "0x1234"), "ValueError", "a bad hash")
	T.is_err(e.report_hint("ps_missing", TX), "KeyError", "an unknown session")
	T.eq(e.pending[rec["id"]]["hint_tx"], null, "no hint stored")
	return T.done()


func test_engine_copies_initial_state() -> bool:
	var pending := {"ps_x": {"chain": "arbitrum"}}
	var receipts := {"k": {"kind": "tip"}}
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	var e := ScfgEngine.new(cfg, {"pending": pending, "receipts": receipts,
		"rpc_factory": func(_c: Dictionary) -> Variant: return F.FakeChain.new()})
	e.pending["ps_x"]["chain"] = "base"
	e.receipts["k"]["kind"] = "unlock"
	T.eq([pending["ps_x"]["chain"], receipts["k"]["kind"]], ["arbitrum", "tip"], "deep copies")
	return T.done()


func test_snapshot_is_detached_from_engine_state() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	await e.start_session("tip", "base", {"amount": "3", "token": "USDC"})
	var snap: Variant = e.snapshot()
	(snap["pending"] as Dictionary).values()[0]["state"] = "x"
	T.eq(e.pending.values()[0]["state"], "awaiting", "detached")
	return T.done()


func test_unknown_engine_option_is_a_type_error() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	T.is_err(ScfgEngine.create(cfg, {"colour": "red"}), "TypeError", "create")
	var e := ScfgEngine.new(cfg, {"colour": "red"})
	T.is_err(e.init_error, "TypeError", "init_error")
	T.is_err(await e.tick(), "TypeError", "every method returns it")
	T.ok(ScfgEngine.create(cfg, {}) is ScfgEngine, "no options")
	return T.done()


# --- expiry, discard ---------------------------------------------------------------------------


func test_window_expiry_is_reported_once_and_scanning_continues() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var clock: F.FakeClock = r[2]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	clock.advance(30 * 60)
	var expired := (await e.tick() as Array).filter(func(o: Dictionary) -> bool: return o["status"] == "expired")
	T.eq(expired.size(), 1, "one expiry")
	T.eq(expired[0], {"session_id": rec["id"], "status": "expired", "confirmations": 0, "required": 0,
		"result": {"status": "expired", "kind": "unlock", "item": "chapter2", "amount": null, "token": "USDC",
			"chain": "arbitrum", "tx_hash": null, "payer": null, "session_id": rec["id"], "error": null},
		"error": null, "hint_tx": null}, "the expiry outcome")
	T.eq(e.pending[rec["id"]]["ui_status"], "expired", "marked")
	var again := (await e.tick() as Array).filter(func(o: Dictionary) -> bool: return o["status"] == "expired")
	T.eq(again, [], "not repeated")
	T.ok(e.pending.has(rec["id"]), "still watched")
	return T.done()


func test_cancelled_sessions_do_not_report_expiry() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	e.set_ui_status(rec["id"], "cancelled")
	(r[2] as F.FakeClock).advance(31 * 60)
	var expired := (await e.tick() as Array).filter(func(o: Dictionary) -> bool: return o["status"] == "expired")
	T.eq(expired, [], "no expiry")
	return T.done()


func test_set_ui_status_ignores_finished_sessions() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	T.eq(e.set_ui_status("ps_gone", "cancelled"), null, "no KeyError")
	return T.done()


func test_discard_session_only_drops_untouched_sessions() -> bool:
	var e: ScfgEngine = _make_engine()[0]
	var a: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var b: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	e.report_hint(b["id"], TX)
	e.discard_session(a["id"])
	e.discard_session(b["id"])
	T.eq(e.discard_session("ps_gone"), null, "an unknown session")
	T.eq(e.pending.keys(), [b["id"]], "only the hinted one is kept")
	return T.done()


func test_unwatchable_session_waits_for_its_horizon_then_closes() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	var rec := {"id": "ps_gone", "chain": "nochain", "created_at": 0, "ui_status": "cancelled", "kind": "tip",
		"item": null, "token": "USDC"}
	var clock := F.FakeClock.new(100.0)
	var e := ScfgEngine.new(cfg, {"pending": {"ps_gone": rec}, "clock": clock.fn,
		"rpc_factory": func(_c: Dictionary) -> Variant: return F.FakeChain.new()})
	T.eq(await e.tick(), [], "kept until its horizon")
	clock.advance(86400 - 100)
	T.eq(await e.tick(), [{"session_id": "ps_gone", "status": "closed", "confirmations": 0, "required": 0,
		"result": null, "error": null, "hint_tx": null}], "then closed")
	T.eq(e.pending, {}, "removed")
	return T.done()


# --- containment ----------------------------------------------------------------------------------


## Answers eth_blockNumber until `exc` is set, then returns it from post_json (test_engine.py
## `_Transport`).
class _Transport:
	extends ScfgCoio.Transport
	var exc: Variant = null

	func post_json(_url: String, payload: Dictionary, _timeout: float) -> Variant:
		if exc != null:
			return exc
		return {"jsonrpc": "2.0", "id": payload["id"], "result": "0x3e8"}


func test_tick_survives_any_transport_exception_in_each_session() -> bool:
	# A transport failure that is not a TransportError (a dropped HTTP body, say) must still end up
	# in the engine's per-session handler: no crash, no session left out.
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var transport := _Transport.new()
	var errors := []
	var e := ScfgEngine.new(cfg, {"transport": transport, "clock": F.FakeClock.new(100.0).fn,
		"on_error": func(x: Variant) -> void: errors.append(x)})
	var a: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var b: Variant = await e.start_session("tip", "arbitrum", {"amount": "3", "token": "USDC"})
	transport.exc = ScfgErr.make("RuntimeError", "incomplete read")
	var out: Variant = await e.tick()
	var sids := (out as Array).map(func(o: Dictionary) -> String: return o["session_id"])
	sids.sort()
	var want := [a["id"], b["id"]]
	want.sort()
	T.eq(sids, want, "every session reported")
	T.ok((out as Array).all(func(o: Dictionary) -> bool: return o["status"] == "waiting" and o["error"] == "internal_error"),
		"contained")
	T.eq(errors.size(), 2, "on_error twice")
	T.ok(errors.all(func(x: Variant) -> bool: return x is ScfgErr and (x as ScfgErr).kind == "RuntimeError"), "the errors")
	return T.done()


func test_tick_contains_a_session_crash() -> bool:
	# One session's step fails (a malformed log); the others still advance, in pending order, and
	# the failed one is reported with Python's StepOutcome defaults.
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var errors := []
	e.on_error = func(x: Variant) -> void: errors.append(x)
	var a: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var b: Variant = await e.start_session("tip", "base", {"amount": "3", "token": "USDC"})
	var c: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var bad := F.transfer_log(ScfgChains.MAINNET["base"]["tokens"]["USDC"]["address"], PAYER, DEV, 1,
		{"block": 999, "tx_hash": TX})
	bad["logIndex"] = "0xzz"
	(r[1]["base"] as F.FakeChain).logs.append(bad)
	var out: Variant = await e.tick()
	T.eq((out as Array).map(func(o: Dictionary) -> String: return o["session_id"]), [a["id"], b["id"], c["id"]],
		"pending order")
	T.eq(out[1], {"session_id": b["id"], "status": "waiting", "confirmations": 0, "required": 0, "result": null,
		"error": "internal_error", "hint_tx": null}, "the contained outcome")
	T.eq([out[0]["status"], out[2]["status"]], ["waiting", "waiting"], "the others advanced")
	T.eq(errors.size(), 1, "on_error once")
	T.is_err(errors[0] if errors.size() > 0 else null, "ValueError", "the hex parse of logIndex 0xzz")
	T.ok(e.pending.has(b["id"]), "the session is kept")
	T.eq(e.pending[b["id"]]["cursor"], 995, "and untouched")
	return T.done()


## An EVM backend whose advance and check_ready are scripted, swapped into the registry by
## _with_backend (the Engine reaches its backend through ScfgBackends).
class StubBackend:
	extends ScfgEvmBackend
	## What advance returns: "null" (a coroutine a script error aborted), or an ScfgErr.
	var advance_result: Variant = null
	## What check_ready returns per call (an ScfgErr, null, a Dictionary or a non-Dictionary).
	var ready_result: Variant = null
	var ready_calls: Array = []

	func advance(_pending: Dictionary, _receipts: Dictionary, _sid: Variant, _opts: Dictionary) -> Variant:
		return advance_result

	func check_ready(opts: Dictionary) -> Variant:
		ready_calls.append(opts["chain"]["key"])
		return ready_result


## The registry as it was before a _with_backend run that has not put it back yet, or {}.
static var _saved_registry := {}


## Runs `body` (a Callable(stub)) with the registry's EVM backend replaced by a StubBackend, then
## puts the whole registry back, whatever the body did to it. A body a script error aborts still
## returns (null) here, so the registry is restored then too; and a run that never came back (a
## hung body, which fails the runner) is undone by the next call before it starts.
func _with_backend(body: Callable) -> void:
	_restore_registry()
	_saved_registry = ScfgBackends._load().duplicate()
	var stub := StubBackend.new()
	ScfgBackends._BACKENDS["evm"] = stub
	await body.call(stub)
	_restore_registry()


static func _restore_registry() -> void:
	if _saved_registry.is_empty():
		return
	ScfgBackends._BACKENDS.clear()
	ScfgBackends._BACKENDS.merge(_saved_registry)
	_saved_registry = {}


func test_with_backend_restores_the_whole_registry() -> bool:
	var before := ScfgBackends._load().duplicate()
	await _with_backend(func(stub: StubBackend) -> void:
		T.ok(ScfgBackends.backend_for_family("evm") == stub, "the stub is in place")
		ScfgBackends._BACKENDS["solana"] = stub  # the body changes another family too
		ScfgBackends._BACKENDS["other"] = stub)
	T.eq(ScfgBackends._BACKENDS.keys(), before.keys(), "the same families")
	for family: String in before:
		T.ok(ScfgBackends._BACKENDS[family] == before[family], "%s is the original backend" % family)
	_saved_registry = before.duplicate()  # a run that never came back...
	ScfgBackends._BACKENDS["evm"] = StubBackend.new()
	await _with_backend(func(_stub: StubBackend) -> void: pass)
	T.ok(ScfgBackends._BACKENDS["evm"] == before["evm"], "...is undone by the next run")
	return T.done()


## A backend step whose coroutine was aborted by a script error comes back as null: contained as
## a RuntimeError, like a returned ScfgErr. Exercised through tick(), the only caller.
func test_tick_contains_a_null_step() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var errors := []
	e.on_error = func(x: Variant) -> void: errors.append(x)
	var a: Variant = await e.start_session("tip", "base", {"amount": "3", "token": "USDC"})
	await _with_backend(func(stub: StubBackend) -> void:
		stub.advance_result = null
		var out: Variant = await e.tick()
		T.ok(out is Array and out.size() == 1, "one outcome")
		if out is Array and out.size() == 1:
			T.eq([out[0]["session_id"], out[0]["status"], out[0]["error"]], [a["id"], "waiting", "internal_error"],
				"the default outcome")
		T.eq(errors.size(), 1, "reported once")
		T.is_err(errors[0] if errors.size() > 0 else null, "RuntimeError", "as a RuntimeError")
		stub.advance_result = ScfgErr.make("ValueError", "boom")
		out = await e.tick()
		T.eq(errors.size(), 2, "a returned error is reported too")
		T.is_err(errors[1] if errors.size() > 1 else null, "ValueError", "as itself")
		T.eq(out[0]["error"], "internal_error", "contained"))
	T.ok(e.pending.has(a["id"]), "the session is kept")
	return T.done()


func test_tick_never_runs_sessions_concurrently() -> bool:
	# Every FakeChain call suspends for frames and is journaled in one shared list: a concurrent
	# tick would interleave the second session's calls with the first's.
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	await e.start_session("tip", "base", {"amount": "3", "token": "USDC"})
	var journal: Array[String] = []
	for key: String in r[1]:
		var fake: F.FakeChain = r[1][key]
		fake.frames = 2
		fake.journal = journal
		fake.tag = key
	var out: Variant = await e.tick()
	T.eq(_statuses(out), ["waiting", "waiting"], "both stepped")
	T.eq(journal, ["arbitrum:block_number", "arbitrum:get_logs", "base:block_number", "base:get_logs"],
		"one session after another, in pending order")
	return T.done()


func test_injected_entropy_supplies_id_and_nonce() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var entropy := F.RecordingEntropy.new()
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(_c: Dictionary) -> Variant: return F.FakeChain.new(),
		"entropy": entropy, "clock": func() -> int: return 1})
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	T.eq(entropy.draws, ["randbelow(100)", "token_hex(8)"], "Python's draws")
	T.eq([rec["id"], rec["units"]], ["ps_" + "ab".repeat(8), 4_993_742], "id and nonce")
	return T.done()


# --- reverify and restore (tests/unit/test_engine.py) ---------------------------------------


func test_reverify_uses_accepted_wallets() -> bool:
	var seen := []
	var r := _make_engine({"previous_wallets": [PAYER]}, {"rpc_factory": func(c: Dictionary) -> Variant:
		seen.append(c["key"])
		return F.FakeChain.new(1000)})
	var e: ScfgEngine = r[0]
	e.receipts["k"] = {"chain": "arbitrum", "tx_hash": TX, "log_index": 0, "block": 900, "amount": "1.000000",
		"token": "USDC", "kind": "tip", "item": null}
	T.eq(await e.reverify(), ["k"], "no matching log in the fake chain -> revoked")
	T.eq(seen, ["arbitrum"], "only the receipt's chain was asked (readiness needs no client for EVM)")
	T.eq(e.receipts, {}, "removed")
	return T.done()


func test_reverify_reports_to_the_on_error_set_mid_pass() -> bool:
	# The Godot autoload replaces the Engine's on_error in _exit_tree while a pass may be in
	# flight; a receipt's contained error reported after that must reach the new handler.
	var first := []
	var second := []
	var box := {}
	var r := _make_engine({}, {"on_error": func(x: Variant) -> void: first.append(x),
		"rpc_factory": func(_c: Dictionary) -> Variant:
			(box["e"] as ScfgEngine).on_error = func(x: Variant) -> void: second.append(x)
			return ScfgErr.make("RuntimeError", "boom")})
	var e: ScfgEngine = r[0]
	box["e"] = e
	e.receipts["k"] = {"chain": "arbitrum", "tx_hash": TX, "log_index": 0, "block": 900, "amount": "1.000000",
		"token": "USDC", "kind": "tip", "item": null}
	T.eq(await e.reverify(), [], "an unverifiable receipt is kept")
	T.eq(first, [], "not told to the handler the pass started with")
	T.eq(second.map(func(x: Variant) -> String: return x.kind if x is ScfgErr else str(x)), ["RuntimeError"],
		"told to the current one")
	box.clear()
	return T.done()


## An on_error that is no longer a valid Callable (its object freed, or never set) when a pass
## reports: the error is printed (ScfgErr.report's fallback), not called into the void.
func test_reverify_prints_when_on_error_is_not_a_valid_callable() -> bool:
	var r := _make_engine({}, {"rpc_factory": func(_c: Dictionary) -> Variant:
		return ScfgErr.make("RuntimeError", "boom")})
	var e: ScfgEngine = r[0]
	e.receipts["k"] = {"chain": "arbitrum", "tx_hash": TX, "log_index": 0, "block": 900, "amount": "1.000000",
		"token": "USDC", "kind": "tip", "item": null}
	var gone := Node.new()
	var dead := gone.queue_free  # a Callable of an object that is freed below
	gone.free()
	for handler: Callable in [Callable(), dead]:
		e.on_error = handler
		ScfgErr.silence_printed = true
		ScfgErr.printed.clear()
		T.eq(await e.reverify(), [], "an unverifiable receipt is kept")
		T.eq(ScfgErr.printed.size(), 1, "printed instead (%s)" % [ScfgErr.printed])
		T.ok(ScfgErr.printed.size() == 1 and "RuntimeError" in ScfgErr.printed[0] and "boom" in ScfgErr.printed[0],
			"naming the error (%s)" % [ScfgErr.printed])
		ScfgErr.silence_printed = false
	ScfgErr.printed.clear()
	return T.done()


func test_restore_through_engine() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var chain := F.FakeChain.new()
	chain.receipts[TX] = {"status": "0x1", "logs": [F.transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, {"block": 10, "tx_hash": TX})]}
	for k: String in ScfgChains.MAINNET:
		r[1][k] = chain if k == "arbitrum" else F.FakeChain.new()
	var cands: Variant = await e.restore_lookup(TX)
	T.eq(cands[0]["item"] if cands is Array else cands, "chapter2", "classified")
	var added: Variant = e.restore_apply(cands)
	T.eq((added as Array).size() if added is Array else added, 1, "applied")
	var snap: Variant = e.snapshot()
	T.eq(snap["owned"], ["chapter2"], "owned")
	return T.done()


func test_restore_lookup_asks_the_enabled_chains_first_then_the_rest() -> bool:
	var asked := []
	var r := _make_engine({"chains": ["polygon", "base"]}, {"rpc_factory": func(c: Dictionary) -> Variant:
		var fake := F.FakeChain.new()
		fake.tag = c["key"]
		fake.journal = asked
		return fake})
	var e: ScfgEngine = r[0]
	var out: Variant = await e.restore_lookup(TX)
	T.is_err(out, "RestoreError", "nowhere")
	T.eq(out.code if out is ScfgErr else out, "tx_not_found", "not found")
	var keys := asked.map(func(x: String) -> String: return x.split(":")[0])
	T.eq(keys.slice(0, 2), ["polygon", "base"], "the enabled chains first, in config order")
	T.ok(keys.size() > 2, "then the rest of the registry")
	T.ok(not "solana" in keys, "an EVM hash is not a Solana signature")
	return T.done()


func test_restore_apply_and_lookup_return_the_init_error() -> bool:
	var bad := ScfgEngine.new(ScfgConfig.build_config({"wallet": DEV}), {"nope": 1})
	T.is_err(await bad.reverify(), "TypeError", "reverify")
	T.is_err(await bad.restore_lookup(TX), "TypeError", "restore_lookup")
	T.is_err(bad.restore_apply([]), "TypeError", "restore_apply")
	return T.done()


# --- readiness -------------------------------------------------------------------------------


func test_reverify_replaces_unready_with_the_readiness_check() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	e.unready = {["arbitrum", "USDC"]: true}
	var errors := []
	e.on_error = func(x: Variant) -> void: errors.append(x)
	await e.reverify()
	T.eq(e.unready, {}, "EVM tokens are always ready")
	T.eq(errors, [], "no error")
	return T.done()


func test_readiness_marks_unready_routes_and_the_snapshot_sorts_them() -> bool:
	var r := _make_engine({"chains": ["base", "arbitrum"]})
	var e: ScfgEngine = r[0]
	await _with_backend(func(stub: StubBackend) -> void:
		stub.ready_result = {"USDC": true, "USDT": false}
		T.eq(await e.reverify(), [], "no receipts")
		T.eq(stub.ready_calls, ["base", "arbitrum"], "every enabled chain, in config order")
		T.eq(e.unready, {["base", "USDT"]: true, ["arbitrum", "USDT"]: true}, "the routes that cannot be received")
		T.eq(e.snapshot()["unready"], [["arbitrum", "USDT"], ["base", "USDT"]], "sorted in the snapshot")
		stub.ready_result = {"USDC": true, "USDT": true}
		await e.reverify()
		T.eq(e.unready, {}, "replaced by the new check"))
	return T.done()


## Network trouble never hides a payment option on doubt, and is not an error worth reporting.
func test_readiness_hides_nothing_on_rpc_unavailable_or_rpc_error() -> bool:
	for kind: String in ["RpcUnavailable", "RpcError"]:
		var r := _make_engine({"chains": ["base", "arbitrum"]})
		var e: ScfgEngine = r[0]
		var errors := []
		e.on_error = func(x: Variant) -> void: errors.append(x)
		e.unready = {["base", "USDT"]: true}
		await _with_backend(func(stub: StubBackend) -> void:
			stub.ready_result = ScfgErr.make(kind, "node trouble", -32000 if kind == "RpcError" else null)
			T.eq(await e.reverify(), [], "%s: reverify still answers" % kind)
			T.eq(e.unready, {}, "%s: nothing hidden" % kind)
			T.eq(stub.ready_calls, ["base", "arbitrum"], "%s: every chain was still asked" % kind)
			T.eq(errors, [], "%s: not reported" % kind))
	return T.done()


func test_readiness_reports_other_failures_and_carries_on() -> bool:
	var r := _make_engine({"chains": ["base", "arbitrum"]})
	var e: ScfgEngine = r[0]
	var errors := []
	e.on_error = func(x: Variant) -> void: errors.append(x)
	await _with_backend(func(stub: StubBackend) -> void:
		stub.ready_result = ScfgErr.make("ValueError", "bad token account")
		T.eq(await e.reverify(), [], "reverify still answers")
		T.eq(errors.size(), 2, "one report per chain")
		T.is_err(errors[0] if errors.size() > 0 else null, "ValueError", "as itself")
		stub.ready_result = null
		await e.reverify()
		T.eq(errors.size(), 4, "a null (an aborted coroutine) is reported too")
		T.is_err(errors[2] if errors.size() > 2 else null, "RuntimeError", "as a RuntimeError")
		T.eq(e.unready, {}, "nothing hidden"))
	return T.done()


func test_readiness_lets_a_cancellation_through_and_rejects_a_non_dict() -> bool:
	var r := _make_engine({"chains": ["base"]})
	var e: ScfgEngine = r[0]
	var errors := []
	e.on_error = func(x: Variant) -> void: errors.append(x)
	e.unready = {["base", "USDT"]: true}
	await _with_backend(func(stub: StubBackend) -> void:
		stub.ready_result = ScfgErr.make("CancelledError", "cancelled")
		T.is_err(await e.reverify(), "CancelledError", "not an Exception: not contained")
		T.eq(errors, [], "not reported")
		T.eq(e.unready, {["base", "USDT"]: true}, "unready is left as it was")
		stub.ready_result = ["USDC"]
		T.is_err(await e.reverify(), "AttributeError", "Python: ready.items() on a list"))
	return T.done()


## JSON.stringify then JSON.parse_string, as the glue's user://scfg/state.json save and load do: every
## stored number comes back a float.
func _json_round_trip(v: Variant) -> Variant:
	return JSON.parse_string(JSON.stringify(v))


func test_state_reloaded_from_json_finishes_a_confirming_session() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var clock: F.FakeClock = r[2]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var chain: F.FakeChain = r[1]["arbitrum"]
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, rec["units"], {"block": 999, "tx_hash": TX, "log_index": 3}))
	T.eq(_statuses(await e.tick()), ["confirming"], "confirming before the save")
	var pending: Variant = _json_round_trip(e.pending)
	var receipts: Variant = _json_round_trip(e.receipts)
	T.eq(typeof(pending[rec["id"]]["units"]), TYPE_FLOAT, "the reload made the stored units a float")
	var errors := []
	var reloaded := ScfgEngine.new(e.config, {"rpc_factory": func(_c: Dictionary) -> Variant: return chain,
		"clock": clock.fn, "pending": pending, "receipts": receipts,
		"on_error": func(x: Variant) -> void: errors.append(x)})
	var stored: Dictionary = reloaded.pending[rec["id"]]
	T.eq([typeof(stored["units"]), typeof(stored["start_block"])], [TYPE_INT, TYPE_INT], "the Engine normalised the numbers")
	T.eq(stored["units"], rec["units"], "the same units")
	chain.head = 999 + 19
	var outcomes: Variant = await reloaded.tick()
	T.eq(_statuses(outcomes), ["paid"], "paid after the reload")
	T.eq(errors, [], "no contained step failure")
	T.eq(reloaded.snapshot()["owned"], ["chapter2"], "the player is credited")
	var keys: Array = reloaded.receipts.keys()
	T.eq(keys, ["arbitrum:%s:3" % TX], "one receipt, keyed by the int log index")
	return T.done()


func test_a_receipt_reloaded_from_json_is_not_counted_twice() -> bool:
	var r := _make_engine()
	var e: ScfgEngine = r[0]
	var rec: Variant = await e.start_session("unlock", "arbitrum", {"item": "chapter2"})
	var chain: F.FakeChain = r[1]["arbitrum"]
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, rec["units"], {"block": 999, "tx_hash": TX, "log_index": 3}))
	chain.head = 999 + 19
	T.eq(_statuses(await e.tick()), ["paid"], "paid before the save")
	var receipts: Variant = _json_round_trip(e.receipts)
	var key: String = (receipts as Dictionary).keys()[0]
	T.eq(typeof(receipts[key]["log_index"]), TYPE_FLOAT, "the reload made log_index a float")
	var reloaded := ScfgEngine.new(e.config, {"rpc_factory": func(_c: Dictionary) -> Variant: return chain,
		"receipts": receipts})
	var receipt: Dictionary = reloaded.receipts[key]
	T.eq(ScfgReceipts.receipt_key(receipt["chain"], receipt["tx_hash"], receipt["log_index"]), key,
		"the reloaded receipt's fields give its stored key")
	T.eq(ScfgReceipts.add_receipt(reloaded.receipts, receipt.duplicate(true)), false, "adding it again is a no-op")
	T.eq(ScfgReceipts.add_receipt(reloaded.receipts, e.receipts[key].duplicate(true)), false,
		"so is adding the never-saved original")
	T.eq(reloaded.receipts.size(), 1, "still one receipt")
	T.eq(reloaded.snapshot()["owned"], ["chapter2"], "owned")
	return T.done()


func test_initial_state_is_normalised_from_json_numbers() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	var big: Variant = ScfgBigInt.from_int(7)
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(_c: Dictionary) -> Variant: return F.FakeChain.new(),
		"pending": {"ps_x": {"start_block": 12.0, "match": {"value": 4990000.0, "log_index": 0.0}, "created_at": 100.5,
			"list": [1.0, 2.5, "3"], "big": big}},
		"receipts": {"k": {"log_index": 2.0, "block": 999.0, "amount": "4.99"}}})
	var p: Dictionary = e.pending["ps_x"]
	T.eq([typeof(p["start_block"]), typeof(p["match"]["value"]), typeof(p["match"]["log_index"])],
		[TYPE_INT, TYPE_INT, TYPE_INT], "integral floats become ints, nested too")
	T.eq(p["created_at"], 100.5, "a fractional float stays a float")
	T.eq(typeof(p["created_at"]), TYPE_FLOAT, "(still a float)")
	T.eq(p["list"], [1, 2.5, "3"], "list items too; strings unchanged")
	T.ok(is_same(p["big"], big), "a non-JSON value passes through")
	T.eq([typeof(e.receipts["k"]["log_index"]), typeof(e.receipts["k"]["block"])], [TYPE_INT, TYPE_INT], "receipts too")
	return T.done()
