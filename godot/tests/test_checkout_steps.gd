## The stepped payment sheet: ScfgCheckoutController.step and its navigation
## (tests/unit/test_checkout_steps.py, same tests, same names).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
const SERVER_URL := "http://127.0.0.1:1234/c/secret/"
const ONLY_MANUAL := {"browser": {"enabled": false}, "qr": {"enabled": false}}
static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


## {ctl, engine, fakes, clock} (test_checkout_steps.py `setup`). `opts`: kind, chains, methods,
## browser_wallet.
func _setup(opts: Dictionary = {}) -> Dictionary:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV, "chains": opts.get("chains"), "methods": opts.get("methods")})
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
	var sub := F.ImmediateSubmitter.new(engine)
	var ctl_opts := {"config": cfg, "spec": spec, "submit": sub.submit,
		"server_factory": func(kw: Dictionary) -> Variant: return F.FakeServer.new(kw)}
	if opts.has("browser_wallet"):
		ctl_opts["browser_wallet"] = opts["browser_wallet"]
	return {"ctl": ScfgCheckoutController.new(ctl_opts), "engine": engine, "fakes": fakes, "clock": clock, "sub": sub}


func _on_method_step(opts: Dictionary = {}) -> Dictionary:
	var m := _setup(opts)
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.choose_network("arbitrum")
	ctl.poll()
	return m


func _deliver(ctl: ScfgCheckoutController, engine: ScfgEngine) -> void:
	for o: Variant in await engine.tick():
		ctl.on_outcome(o)


func _log(ctl: ScfgCheckoutController) -> Dictionary:
	return F.transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"], {"block": 999, "tx_hash": TX})


# --- where the sheet starts ---------------------------------------------------------

func test_unlock_starts_on_network_and_tip_on_amount() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.eq(ctl.step, "network")
	T.eq(ctl.can_go_back, false)
	ctl = _setup({"kind": "tip"})["ctl"]
	T.eq(ctl.step, "amount")
	T.eq(ctl.can_go_back, false)
	return T.done()


func test_step_path_counts_the_steps_shown() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.eq(ctl.step_path(), ["network", "method", "browser", "waiting"])
	T.eq(ctl.step_dots(), [0, 4])
	ctl = _setup({"kind": "tip"})["ctl"]
	T.eq(ctl.step_path(), ["amount", "network", "method", "browser", "waiting"])
	return T.done()


# --- forward ------------------------------------------------------------------------

func test_network_waits_for_the_session_then_moves_to_method() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.choose_network("arbitrum")
	T.eq(ctl.step, "network")
	T.eq(ctl.state, "starting")
	ctl.poll()
	T.eq(ctl.step, "method")
	T.eq(ctl.nav_dir, 1)
	T.ok((m["engine"] as ScfgEngine).pending.has(ctl.session_id), "the session is held")
	T.eq(ctl.step_dots(), [1, 4])
	return T.done()


func test_choosing_a_method_opens_its_page() -> bool:
	for name: String in ["browser", "qr", "manual"]:
		var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
		ctl.choose_method(name)
		T.eq(ctl.step, name, name + " step")
		T.eq(ctl.method, name, name + " method")
	return T.done()


func test_unknown_method_is_ignored() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step({"methods": {"qr": {"enabled": false}}})["ctl"]
	ctl.choose_method("qr")
	T.eq(ctl.step, "method")
	return T.done()


func test_opening_the_browser_moves_to_waiting() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("browser")
	T.eq(ctl.open_browser(), SERVER_URL)
	T.eq(ctl.step, "waiting")
	T.eq(ctl.can_go_back, true)
	return T.done()


func test_browser_failure_returns_to_method() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("browser")
	ctl.server_factory = func(_kw: Dictionary) -> Variant: return ScfgErr.make("RuntimeError", "no sockets")
	T.eq(ctl.open_browser(), null)
	T.eq(ctl.step, "method")
	T.eq(ctl.nav_dir, -1)
	T.eq(ctl.error, "browser_unavailable")
	T.eq(ctl.method, "qr")
	return T.done()


func test_ive_sent_it_moves_qr_and_manual_to_waiting() -> bool:
	for name: String in ["qr", "manual"]:
		var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
		ctl.choose_method(name)
		ctl.mark_sent()
		T.eq(ctl.step, "waiting", name)
		T.eq(ctl.can_go_back, true, name)
	return T.done()


func test_ive_sent_it_clears_a_bad_hash_error() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("manual")
	ctl.submit_hash("garbage")
	T.eq(ctl.error, "invalid_hash")
	ctl.mark_sent()
	T.eq(ctl.step, "waiting")
	T.eq(ctl.error, null)
	return T.done()


func test_tip_amount_step_applies_a_typed_amount() -> bool:
	var ctl: ScfgCheckoutController = _setup({"kind": "tip"})["ctl"]
	ctl.custom_amount = "0.10"
	T.eq(ctl.next_step(), false)
	T.eq(ctl.step, "amount")
	T.eq(ctl.error, "invalid_amount")
	ctl.custom_amount = "7.50"
	T.eq(ctl.next_step(), true)
	T.eq(ctl.step, "network")
	T.eq(str(ctl.amount), "7.50")
	return T.done()


## tests/unit/test_checkout_steps.py::test_a_typed_amount_is_stripped_as_str_strip_does
func test_a_typed_amount_is_stripped_as_str_strip_does() -> bool:
	var ctl: ScfgCheckoutController = _setup({"kind": "tip"})["ctl"]
	ctl.custom_amount = char(0x85) + char(0x1c)
	T.eq(ctl.next_step(), true)
	T.eq(ctl.step, "network")
	ctl = _setup({"kind": "tip"})["ctl"]
	ctl.custom_amount = char(0xfeff) + "7.50"
	T.eq(ctl.next_step(), false)
	T.eq(ctl.error, "invalid_amount")
	T.eq(ctl.apply_custom_amount(), false)
	ctl.custom_amount = char(0x85) + "7.50" + char(0x1c)
	T.eq(ctl.apply_custom_amount(), true)
	T.eq(str(ctl.amount), "7.50")
	return T.done()


# --- skipping single-option steps -------------------------------------------------------

func test_single_network_is_skipped_for_unlocks() -> bool:
	var m := _setup({"chains": ["base"]})
	var ctl: ScfgCheckoutController = m["ctl"]
	T.eq(ctl.state, "starting")
	T.eq(ctl.chain_key, "base")
	ctl.poll()
	T.eq(ctl.step, "method")
	T.eq(ctl.can_go_back, false)
	T.eq(ctl.step_path(), ["method", "browser", "waiting"])
	return T.done()


func test_single_network_is_skipped_for_tips_both_ways() -> bool:
	var ctl: ScfgCheckoutController = _setup({"kind": "tip", "chains": ["base"]})["ctl"]
	ctl.next_step()
	ctl.poll()
	T.eq(ctl.step, "method")
	ctl.back()
	T.eq(ctl.step, "amount")
	T.eq(ctl.nav_dir, -1)
	return T.done()


func test_single_method_is_skipped_both_ways() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step({"methods": ONLY_MANUAL})["ctl"]
	T.eq(ctl.step, "manual")
	ctl.back()
	T.eq(ctl.step, "network")
	return T.done()


# --- back -------------------------------------------------------------------------------

func test_back_walks_the_path_and_keeps_the_session() -> bool:
	var m := _on_method_step()
	var ctl: ScfgCheckoutController = m["ctl"]
	var sid: Variant = ctl.session_id
	ctl.choose_method("qr")
	ctl.mark_sent()
	ctl.back()
	T.eq(ctl.step, "qr")
	T.eq(ctl.nav_dir, -1)
	ctl.back()
	T.eq(ctl.step, "method")
	ctl.back()
	T.eq(ctl.step, "network")
	T.eq(ctl.can_go_back, false)
	ctl.back()  # nothing before network on an unlock
	T.eq(ctl.step, "network")
	T.eq(ctl.session_id, sid)
	T.ok((m["engine"] as ScfgEngine).pending.has(sid), "the session is kept")
	return T.done()


func test_choosing_the_same_network_again_keeps_the_session() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	var sid: Variant = ctl.session_id
	ctl.back()
	ctl.choose_network("arbitrum")
	T.eq(ctl.step, "method")
	T.eq(ctl.session_id, sid)
	T.eq(ctl.nav_dir, 1)
	return T.done()


func test_choosing_another_network_restarts_the_session() -> bool:
	var m := _on_method_step()
	var ctl: ScfgCheckoutController = m["ctl"]
	var sid: Variant = ctl.session_id
	ctl.back()
	ctl.choose_network("base")
	ctl.poll()
	T.eq(ctl.step, "method")
	T.ok(ctl.session_id != sid, "a new session")
	T.ok(not (m["engine"] as ScfgEngine).pending.has(sid), "the old one is gone")
	return T.done()


func test_back_to_amount_and_same_amount_keeps_the_session() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step({"kind": "tip"})["ctl"]
	var sid: Variant = ctl.session_id
	ctl.back()
	ctl.back()
	T.eq(ctl.step, "amount")
	T.eq(ctl.set_tip_amount("3"), true)
	T.eq(ctl.session_id, sid)
	T.eq(ctl.set_tip_amount("5"), true)
	ctl.poll()
	T.ok(ctl.session_id != sid, "a restarted session")
	T.eq(ctl.step, "amount")
	ctl.next_step()
	ctl.choose_network("arbitrum")
	T.eq(ctl.step, "method")
	return T.done()


func test_back_clears_the_message() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("manual")
	ctl.submit_hash("garbage")
	T.eq(ctl.error, "invalid_hash")
	ctl.back()
	T.eq(ctl.error, null)
	return T.done()


# --- automatic moves ----------------------------------------------------------------------

func test_a_seen_payment_moves_to_waiting_and_hides_back() -> bool:
	var m := _on_method_step()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.choose_method("qr")
	(m["fakes"]["arbitrum"] as F.FakeChain).logs.append(_log(ctl))
	await _deliver(ctl, m["engine"])
	T.eq(ctl.state, "confirming")
	T.eq(ctl.step, "waiting")
	T.eq(ctl.nav_dir, 1)
	T.eq(ctl.can_go_back, false)
	return T.done()


func test_a_pasted_hash_moves_to_waiting_and_hides_back() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("manual")
	T.eq(ctl.submit_hash(TX), true)
	T.eq(ctl.step, "waiting")
	T.eq(ctl.can_go_back, false)
	return T.done()


func test_a_hash_from_the_browser_page_moves_to_waiting() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("browser")
	ctl.open_browser()
	ctl._server.on_tx.call(TX, PAYER)
	T.eq(ctl.step, "waiting")
	T.eq(ctl.can_go_back, false)
	return T.done()


func test_a_dropped_transaction_gives_back_the_arrow() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.choose_method("manual")
	ctl.submit_hash(TX)
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": "tx_not_landed", "hint_tx": TX}))
	T.eq(ctl.step, "waiting")
	T.eq(ctl.can_go_back, true)
	ctl.back()
	T.eq(ctl.step, "manual")
	return T.done()


func test_final_states_move_to_done() -> bool:
	var m := _on_method_step()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.choose_method("qr")
	var fake: F.FakeChain = m["fakes"]["arbitrum"]
	fake.logs.append(_log(ctl))
	await _deliver(ctl, m["engine"])
	fake.head = 1100
	await _deliver(ctl, m["engine"])
	T.eq(ctl.state, "paid")
	T.eq(ctl.step, "done")
	T.eq(ctl.can_go_back, false)

	m = _on_method_step()
	ctl = m["ctl"]
	ctl.choose_method("browser")
	(m["clock"] as F.FakeClock).advance(1800)
	await _deliver(ctl, m["engine"])
	T.eq(ctl.state, "expired")
	T.eq(ctl.step, "done")
	return T.done()


func test_start_failure_returns_to_network_with_the_message() -> bool:
	var m := _setup({"chains": ["base"]})  # (as in Python, a throwaway first setup)
	m = _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var down := F.FakeChain.new()
	down.down = true
	m["fakes"]["arbitrum"] = down
	ctl.choose_network("arbitrum")
	ctl.poll()
	T.eq(ctl.step, "network")
	T.eq(ctl.error, "network_unavailable")
	return T.done()


func test_start_failure_on_a_skipped_network_shows_the_network_step() -> bool:
	var m := _setup({"kind": "tip", "chains": ["base"]})
	var ctl: ScfgCheckoutController = m["ctl"]
	var down := F.FakeChain.new()
	down.down = true
	m["fakes"]["base"] = down
	ctl.next_step()
	ctl.poll()
	T.eq(ctl.step, "network")
	T.eq(ctl.error, "network_unavailable")
	T.eq(ctl.can_go_back, true)
	down.down = false
	ctl.choose_network("base")
	ctl.poll()
	T.eq(ctl.step, "method")
	return T.done()


func test_a_session_restarted_on_the_amount_step_waits_for_a_network_pick() -> bool:
	var ctl: ScfgCheckoutController = _setup({"kind": "tip"})["ctl"]
	ctl.next_step()
	ctl.choose_network("arbitrum")
	ctl.poll()
	ctl.back()
	ctl.back()
	T.eq(ctl.step, "amount")
	ctl.set_tip_amount("5")  # restarts the session on arbitrum
	ctl.next_step()
	ctl.poll()  # ... which is ready now
	T.eq(ctl.step, "network")  # but the player hasn't picked yet
	ctl.choose_network("arbitrum")
	T.eq(ctl.step, "method")
	return T.done()


func test_leaving_the_network_step_while_starting_stays_put() -> bool:
	var ctl: ScfgCheckoutController = _setup({"kind": "tip"})["ctl"]
	ctl.next_step()
	ctl.choose_network("arbitrum")
	ctl.back()
	ctl.poll()
	T.eq(ctl.step, "amount")
	T.eq(ctl.state, "waiting")
	return T.done()


# --- web build ----------------------------------------------------------------------

func test_web_offers_browser_wallet_only_when_the_page_has_one() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step({"browser_wallet": func(_chain: Dictionary) -> bool: return false})["ctl"]
	T.eq(ctl.methods(), ["qr", "manual"])
	ctl = _on_method_step({"browser_wallet": func(chain: Dictionary) -> bool: return chain["key"] == "arbitrum"})["ctl"]
	T.eq(ctl.methods(), ["browser", "qr", "manual"])
	return T.done()


func test_web_browser_wallet_ignores_the_mobile_flag() -> bool:
	var ctl: ScfgCheckoutController = _setup({"browser_wallet": func(_chain: Dictionary) -> bool: return true})["ctl"]
	ctl.mobile = true
	ctl.choose_network("arbitrum")
	ctl.poll()
	T.ok(ctl.methods().has("browser"), "browser")
	return T.done()


func test_the_controller_pumps_and_reopens_an_overlay_page() -> bool:
	var ctl: ScfgCheckoutController = _on_method_step()["ctl"]
	ctl.server_factory = func(kw: Dictionary) -> Variant: return F.PageWithPump.new(kw)
	ctl.choose_method("browser")
	ctl.open_browser()
	ctl.poll()
	T.eq(ctl._server.pumps, 1)
	ctl.open_browser()
	T.eq(ctl._server.reopens, 1)
	return T.done()
