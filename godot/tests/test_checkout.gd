## ScfgCheckoutController: tests/unit/test_checkout.py (the same tests under the same names),
## plus test_session_info_is_the_exact_python_dict and test_messages_match_catalogue.
## The controller is engine-free and synchronous: the fake submitter runs each command at once.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


## {ctl, engine, fakes, clock, servers} (test_checkout.py `setup`). `opts`: kind, mobile, methods,
## appearance (extra build_config options).
func _setup(opts: Dictionary = {}) -> Dictionary:
	var cfg_opts := {"wallet": DEV, "methods": opts.get("methods")}
	cfg_opts.merge(opts.get("appearance", {}))
	var cfg: Variant = ScfgConfig.build_config(cfg_opts)
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var fakes := {}
	var factory := func(chain: Dictionary) -> Variant:
		if not fakes.has(chain["key"]):
			fakes[chain["key"]] = F.FakeChain.new(1000)
		return fakes[chain["key"]]
	var clock := F.FakeClock.new(100.0)
	var engine := ScfgEngine.new(cfg, {"rpc_factory": factory, "clock": clock.fn})
	var spec: Variant
	if opts.get("kind", "unlock") == "unlock":
		spec = ScfgPayments.prepare_payment(cfg, "unlock", {"item": "chapter2"})
	else:
		spec = ScfgPayments.prepare_payment(cfg, "tip", {"amount": "3", "token": "USDC"})
	var servers := []
	var server_factory := func(kw: Dictionary) -> Variant:
		var s := F.FakeServer.new(kw)
		servers.append(s)
		return s
	var sub := F.ImmediateSubmitter.new(engine)
	var ctl := ScfgCheckoutController.new({"config": cfg, "spec": spec, "submit": sub.submit,
		"mobile": opts.get("mobile", false), "server_factory": server_factory})
	return {"ctl": ctl, "engine": engine, "fakes": fakes, "clock": clock, "servers": servers, "sub": sub}


func _started(opts: Dictionary = {}) -> Dictionary:
	var m := _setup(opts)
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.select_chain("arbitrum")
	ctl.poll()
	return m


func _deliver(ctl: ScfgCheckoutController, engine: ScfgEngine) -> void:
	for o: Variant in await engine.tick():
		ctl.on_outcome(o)


func _keys(options: Array) -> Array:
	return options.map(func(o: Array) -> Variant: return o[0])


func test_initial_state_and_chain_options() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.eq(ctl.state, "choose")
	T.eq(ctl.session, null)
	T.eq(_keys(ctl.chain_options()), ["arbitrum", "base", "arc", "polygon", "ethereum"], "display order")
	T.eq(ctl.chain_options()[0], ["arbitrum", "Arbitrum", ScfgChains.MAINNET["arbitrum"]["fee_hint"]], "(key, name, fee hint)")
	T.eq(ctl.pay_amount_text, "4.99")
	return T.done()


func test_select_chain_starts_session() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	T.eq(ctl.state, "waiting")
	T.ok(engine.pending.has(ctl.session["id"]), "the engine holds the session")
	T.eq(ctl.method, "browser")
	T.eq(ctl.required, 20)
	T.ok(ctl.pay_amount_text.begins_with("4.9937"), "the exact amount: " + str(ctl.pay_amount_text))
	T.eq(ctl.price_text, "4.99")
	return T.done()


func test_select_unknown_chain_raises() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.is_err(ctl.select_chain("solana"), "ValueError", "not offered for this payment")
	T.eq(ctl.state, "choose", "nothing started")
	return T.done()


func test_switching_chain_discards_old_session() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var old: Variant = ctl.session_id
	ctl.select_chain("base")
	ctl.poll()
	T.ok(not engine.pending.has(old), "the old session is gone")
	T.ok(engine.pending.has(ctl.session_id), "the new one is held")
	T.eq(ctl.session["chain"], "base")
	return T.done()


func test_start_failure_returns_to_choose_with_error() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var down := F.FakeChain.new()
	down.down = true
	m["fakes"]["arbitrum"] = down
	ctl.select_chain("arbitrum")
	ctl.poll()
	T.eq(ctl.state, "choose")
	T.eq(ctl.error, "network_unavailable")
	T.ok(ctl.message() != null and ctl.message() != "", "a message")
	return T.done()


func test_methods_on_mobile_and_config() -> bool:
	var ctl: ScfgCheckoutController = _started({"mobile": true})["ctl"]
	T.eq(ctl.methods(), ["qr", "manual"])
	ctl = _started({"methods": {"qr": {"chains": ["base"]}}})["ctl"]
	T.eq(ctl.methods(), ["browser", "manual"], "QR limited to base, we're on arbitrum")
	return T.done()


func test_payment_uri() -> bool:
	var ctl: ScfgCheckoutController = _started()["ctl"]
	T.eq(ctl.payment_uri(), "ethereum:%s@42161/transfer?address=%s&uint256=%d" % [USDC_ARB, DEV, ctl.session["units"]])
	return T.done()


func test_submit_hash_validation_and_hint() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	T.eq(ctl.submit_hash("garbage"), false)
	T.eq(ctl.error, "invalid_hash")
	ctl.tx_input = TX
	T.eq(ctl.submit_hash(), true)
	T.eq(engine.pending[ctl.session_id]["hint_tx"], TX)
	T.eq(ctl.hint_tx, TX)
	T.eq(ctl.tx_input, "")
	T.eq(ctl.error, null)
	return T.done()


func test_outcomes_drive_states_to_paid() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var fake: F.FakeChain = m["fakes"]["arbitrum"]
	fake.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"], {"block": 999, "tx_hash": TX}))
	await _deliver(ctl, engine)
	T.eq(ctl.state, "confirming")
	T.eq(ctl.confirmations, 2)
	fake.head = 1100
	await _deliver(ctl, engine)
	T.eq(ctl.state, "paid")
	T.eq(ctl.result["status"], "paid")
	T.eq(ctl.explorer_url(), "https://arbiscan.io/tx/" + TX)
	T.eq(ctl.finish()["status"], "paid")
	return T.done()


func test_outcomes_for_other_sessions_are_ignored() -> bool:
	var ctl: ScfgCheckoutController = _started()["ctl"]
	ctl.on_outcome(ScfgWatcher.step_outcome("ps_other", "paid"))
	T.eq(ctl.state, "waiting")
	return T.done()


func test_reorg_and_hint_errors() -> bool:
	var ctl: ScfgCheckoutController = _started()["ctl"]
	ctl.hint_tx = TX
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "confirming", {"confirmations": 3, "required": 20}))
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "reorged"))
	T.eq(ctl.state, "waiting")
	T.eq(ctl.confirmations, 0)
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": "tx_not_a_payment"}))
	T.eq(ctl.error, "tx_not_a_payment")
	T.eq(ctl.hint_tx, null)
	return T.done()


func test_expiry_shows_expired_state() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	(m["clock"] as F.FakeClock).advance(1800)
	await _deliver(ctl, m["engine"])
	T.eq(ctl.state, "expired")
	T.eq(ctl.finish()["status"], "expired")
	return T.done()


func test_cancel_marks_session_and_returns_result() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var result: Dictionary = ctl.cancel()
	T.eq(result["status"], "cancelled")
	T.eq(result["session_id"], ctl.session_id)
	T.eq((m["engine"] as ScfgEngine).pending[ctl.session_id]["ui_status"], "cancelled")
	return T.done()


func test_keep_playing() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	T.eq(ctl.keep_playing()["status"], "pending")
	T.eq((m["engine"] as ScfgEngine).pending[ctl.session_id]["ui_status"], "pending")
	return T.done()


func test_cancel_before_choosing_chain() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.eq(ctl.cancel()["status"], "cancelled")
	return T.done()


func test_tip_amount_changes_restart_session() -> bool:
	var m := _started({"kind": "tip"})
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var first: Variant = ctl.session_id
	T.eq(ctl.set_tip_amount("5"), true)
	ctl.poll()
	T.eq(ctl.amount.cmp(ScfgDec.parse("5.00")), 0, "the new amount")
	T.eq(ctl.session["price"], "5.00")
	T.ok(not engine.pending.has(first), "the first session is gone")
	T.eq(ctl.set_tip_amount("0.10"), false)
	T.eq(ctl.error, "invalid_amount")
	ctl.custom_amount = " 7.50 "
	T.eq(ctl.apply_custom_amount(), true)
	T.eq(ctl.amount.cmp(ScfgDec.parse("7.50")), 0)
	return T.done()


func test_tip_amount_ignored_for_unlocks() -> bool:
	var ctl: ScfgCheckoutController = _started()["ctl"]
	T.eq(ctl.set_tip_amount("1"), false)
	return T.done()


func test_browser_server_lifecycle_and_hints() -> bool:
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var servers: Array = m["servers"]
	var url: Variant = ctl.start_browser()
	T.eq(url, "http://127.0.0.1:1234/c/secret/")
	T.eq(ctl.start_browser(), url, "reused")
	T.eq(servers.size(), 1)
	var s: F.FakeServer = servers[0]
	T.ok(s.started, "started")
	T.eq(s.session_info["amount_units"], str(ctl.session["units"]))
	T.eq(s.session_info["chain_id_hex"], "0xa4b1")
	T.eq(s.session_info["logo"], false)
	T.eq(s.session_info["accent"], "#0e7c74")
	T.eq(s.session_info["on_accent"], "#ffffff")
	s.on_tx.call(TX, PAYER)  # the page reports the tx
	T.eq(engine.pending[ctl.session_id]["hint_tx"], TX)
	T.eq(s.get_status.call()["tx_hash"], TX)
	ctl.cancel()
	T.ok(s.stopped, "stopped")
	return T.done()


func test_browser_failure_falls_back() -> bool:
	var ctl: ScfgCheckoutController = _started()["ctl"]
	ctl.server_factory = func(_kw: Dictionary) -> Variant: return ScfgErr.make("RuntimeError", "no sockets")
	T.eq(ctl.start_browser(), null)
	T.eq(ctl.error, "browser_unavailable")
	T.ok(not ctl.methods().has("browser"), "browser is no longer offered")
	T.eq(ctl.method, "qr")
	return T.done()


func test_browser_start_failure_falls_back() -> bool:
	# the factory builds the server but its start() fails (Python: start() raises OSError)
	var ctl: ScfgCheckoutController = _started()["ctl"]
	var made := []
	ctl.server_factory = func(kw: Dictionary) -> Variant:
		var s := FailingStart.new(kw)
		made.append(s)
		return s
	T.eq(ctl.start_browser(), null)
	T.eq(ctl.error, "browser_unavailable")
	T.eq(ctl.browser_failed, true)
	T.eq(ctl.method, "qr")
	return T.done()


class FailingStart:
	extends F.FakeServer

	func start() -> Variant:
		return ScfgErr.make("RuntimeError", "address already in use")


func test_browser_session_info_carries_appearance() -> bool:
	var m := _started({"appearance": {"logo": "gui/logo.png", "accent": "#ffcc00"}})
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.start_browser()
	var info: Dictionary = (m["servers"][0] as F.FakeServer).session_info
	T.eq(info["logo"], true)
	T.eq(info["accent"], "#ffcc00")
	T.eq(info["on_accent"], "#17202e")
	return T.done()


func test_browser_info_tells_the_page_how_gas_is_paid() -> bool:
	for c: Array in [["arc", true, "20000000000", "USDC"],  # USDC is Arc's gas coin; 20 gwei mempool floor
			["base", false, "0", "ETH"]]:
		var m := _setup()
		var ctl: ScfgCheckoutController = m["ctl"]
		ctl.select_chain(c[0])
		ctl.poll()
		ctl.start_browser()
		var info: Dictionary = (m["servers"][0] as F.FakeServer).session_info
		T.eq(info["gas_is_token"], c[1], "%s gas_is_token" % c[0])
		T.eq(info["min_fee_wei"], c[2], "%s min_fee_wei" % c[0])
		T.eq(info["native_symbol"], c[3], "%s native_symbol" % c[0])
		T.eq(info["chain_id"], ScfgChains.MAINNET[c[0]]["chain_id"], "%s chain_id" % c[0])
	return T.done()


## What the page is handed, key for key, in Python's order and with Python's value types.
func test_session_info_is_the_exact_python_dict() -> bool:
	var evm_keys := ["title", "kind", "token", "family", "decimals", "amount_units", "amount_text", "price_text",
		"recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent", "on_accent",
		"chain_id", "chain_id_hex", "rpc_url", "token_address", "gas_is_token", "min_fee_wei"]
	var m := _started()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.start_browser()
	var kw: Dictionary = (m["servers"][0] as F.FakeServer).kw
	T.eq(kw.keys(), ["session_info", "on_tx", "get_status"], "the factory's keys: no build_tx off Solana")
	var info: Dictionary = kw["session_info"]
	T.eq(info.keys(), evm_keys, "keys, in order")
	var units: int = ctl.session["units"]
	var expected := {"title": "Chapter 2", "kind": "unlock", "token": "USDC", "family": "evm", "decimals": 6,
		"amount_units": str(units), "amount_text": ctl.pay_amount_text,
		"price_text": "4.99", "recipient": DEV, "chain_name": "Arbitrum", "native_symbol": "ETH",
		"explorer_tx": "https://arbiscan.io/tx/{}", "testnet": false, "logo": false, "accent": "#0e7c74",
		"on_accent": "#ffffff", "chain_id": 42161, "chain_id_hex": "0xa4b1",
		"rpc_url": "https://arbitrum-one-rpc.publicnode.com", "token_address": USDC_ARB, "gas_is_token": false,
		"min_fee_wei": "0"}
	for k: String in evm_keys:
		T.eq(info.get(k), expected[k], "session_info[%s]" % k)
	T.eq(info["amount_text"], "%d.%06d" % [units / 1_000_000, units % 1_000_000], "six decimals")
	# Arc: the gas coin is the token; min_fee_wei and amount_units stay strings
	var arc := _setup()
	var actl: ScfgCheckoutController = arc["ctl"]
	actl.select_chain("arc")
	actl.poll()
	actl.start_browser()
	var ainfo: Dictionary = (arc["servers"][0] as F.FakeServer).session_info
	T.eq(ainfo["chain_id"], 5042)
	T.eq(ainfo["chain_id_hex"], "0x13b2", "Python's hex()")
	T.eq(ainfo["min_fee_wei"], "20000000000")
	T.eq(ainfo["gas_is_token"], true)
	T.eq(typeof(ainfo["amount_units"]), TYPE_STRING)
	return T.done()


func test_messages_match_catalogue() -> bool:
	var path: String = T.vectors_dir.path_join("messages.json")
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	T.ok(parsed is Dictionary and (parsed as Dictionary).has("messages"), "the catalogue loads: " + path)
	if not (parsed is Dictionary):
		return T.done()
	var catalogue: Dictionary = parsed["messages"]
	T.eq(ScfgCheckoutController.MESSAGES.size(), catalogue.size(), "the same number of messages")
	for key: String in catalogue:
		T.ok(ScfgCheckoutController.MESSAGES.has(key), "has " + key)
		T.eq(ScfgCheckoutController.MESSAGES.get(key), catalogue[key], "byte-identical text of " + key)
	T.eq(ScfgCheckoutController.FINAL_STATES, ["paid", "underpaid", "expired"])
	T.eq(ScfgCheckoutController.HINT_VERDICTS, ["tx_reverted", "tx_not_a_payment", "tx_not_landed"])
	T.eq(ScfgCheckoutController.STEP_ORDER, {"amount": 0, "network": 1, "method": 2, "browser": 3, "qr": 3,
		"manual": 3, "waiting": 4, "done": 5})
	T.eq(ScfgCheckoutController.STEP_ORDER.keys(), ["amount", "network", "method", "browser", "qr", "manual",
		"waiting", "done"], "Python's key order")
	return T.done()


# --- poll(): the failure branch (no Python twin: a Future raises, a ScfgJob answers) -------

## A started controller whose next command is the job `make` builds; polled once after select_chain.
func _polled_with(make: Callable) -> ScfgCheckoutController:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.submit = func(_fn: Callable) -> Variant: return make.call()
	ctl.select_chain("arbitrum")
	ctl.poll()
	return ctl


func _failed_start(ctl: ScfgCheckoutController, want_error: String, label: String) -> void:
	T.eq([ctl.state, ctl.error, ctl.chain_key, ctl.session], ["choose", want_error, null, null], label)


func test_poll_a_cancelled_job_is_start_failed() -> bool:
	_failed_start(_polled_with(func() -> Variant:
		var job := ScfgJob.new()
		job.cancel()
		return job), "start_failed", "cancelled")
	return T.done()


func test_poll_a_failure_that_is_not_rpc_unavailable_is_start_failed() -> bool:
	_failed_start(_polled_with(func() -> Variant:
		var job := ScfgJob.new()
		job._start()
		job._fail(ScfgErr.make("ValueError", "boom"))
		return job), "start_failed", "ValueError")
	_failed_start(_polled_with(func() -> Variant:
		var job := ScfgJob.new()
		job._start()
		job._fail(ScfgErr.make("RpcUnavailable", "down"))
		return job), "network_unavailable", "RpcUnavailable")
	return T.done()


func test_poll_a_success_that_is_not_a_session_is_start_failed() -> bool:
	for value: Variant in [null, "x", 7, [1], true]:
		var v: Variant = value
		_failed_start(_polled_with(func() -> Variant:
			var job := ScfgJob.new()
			job._start()
			job._succeed(v)
			return job), "start_failed", "result %s" % [v])
	return T.done()
