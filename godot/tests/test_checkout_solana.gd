## ScfgCheckoutController on Solana: tests/unit/test_checkout_solana.py (same tests, same names).
## Not here: test_restore_accepts_signatures (test_restore_flow.gd) and
## test_local_server_accepts_a_signature_hint (M4, the localhost server).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV_EVM := "0x52908400098527886E0F7030069857D2E4169EE7"
const DEV := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const EVM_TX := "0xabababababababababababababababababababababababababababababababab"

static var MINT: String = ScfgChains.MAINNET["solana"]["tokens"]["USDC"]["address"]
static var SIG: String = F.sig(7)
static var SIG2: String = F.sig(8)


## {ctl, e} (test_checkout_solana.py `setup`). `unready` is the worker snapshot's unready routes:
## an Array of [chain, token] pairs.
func _setup(unready: Array = []) -> Dictionary:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV_EVM, "solana_wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var sol := F.FakeSolanaChain.new()
	var evm := F.FakeChain.new()
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(c: Dictionary) -> Variant: return sol if c["family"] == "solana" else evm,
		"clock": F.FakeClock.new(0.0).fn})
	var spec: Variant = ScfgPayments.prepare_payment(cfg, "unlock", {"item": "chapter2"})
	var sub := F.ImmediateSubmitter.new(e)
	var ctl := ScfgCheckoutController.new({"config": cfg, "spec": spec, "submit": sub.submit,
		"unready": func() -> Array: return unready, "game_name": "Ruby Dreams"})
	return {"ctl": ctl, "e": e, "sub": sub}


func _started(chain: String) -> Dictionary:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.select_chain(chain)
	ctl.poll()
	return m


func test_solana_is_offered_and_unready_tokens_are_hidden() -> bool:
	var ctl: ScfgCheckoutController = _setup()["ctl"]
	T.ok(ctl.chain_options().has(["solana", "Solana", "lowest"]), "Solana is offered")
	ctl = _setup([["solana", "USDC"]])["ctl"]
	T.ok(not ctl.chain_options().any(func(o: Array) -> bool: return o[0] == "solana"), "an unready route is hidden")
	T.eq(ctl.dev_warnings(), ["Your Solana wallet can't receive USDC yet: receive any amount once."])
	return T.done()


func test_only_unready_routes_shows_the_unavailable_message() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"solana_wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var spec: Variant = ScfgPayments.prepare_payment(cfg, "unlock", {"item": "chapter2"})
	var ctl := ScfgCheckoutController.new({"config": cfg, "spec": spec,
		"submit": func(_fn: Callable) -> Variant: return null,
		"unready": func() -> Array: return [["solana", "USDC"]]})
	T.eq(ctl.chain_options(), [])
	T.eq(ctl.message(), ScfgCheckoutController.MESSAGES["solana_unavailable"])
	return T.done()


func test_solana_recipient_and_payment_link() -> bool:
	var ctl: ScfgCheckoutController = _started("solana")["ctl"]
	T.eq(ctl.family, "solana")
	T.eq(ctl.recipient, DEV)
	T.eq(ctl.token_address, MINT)
	var uri: String = ctl.payment_uri()
	T.ok(uri.begins_with("solana:%s?amount=4.9937" % DEV), "the link: " + uri)
	T.ok(("spl-token=%s" % MINT) in uri, "the mint")
	T.ok(("reference=%s" % ctl.session["reference"]) in uri, "the reference")
	T.ok("label=Ruby%20Dreams" in uri, "the label")
	T.eq(ctl.session["dest"], ScfgSolanaPda.associated_token_address(DEV, MINT))
	return T.done()


func test_evm_chain_still_uses_eip681() -> bool:
	var ctl: ScfgCheckoutController = _started("base")["ctl"]
	T.eq(ctl.family, "evm")
	T.eq(ctl.recipient, DEV_EVM)
	T.ok(str(ctl.payment_uri()).begins_with("ethereum:"), "EIP-681")
	return T.done()


func test_pasting_the_wrong_family_is_an_invalid_hash() -> bool:
	# Review focus 1.
	var m := _started("solana")
	var ctl: ScfgCheckoutController = m["ctl"]
	T.eq(ctl.submit_hash(EVM_TX), false)
	T.eq(ctl.error, "invalid_hash")
	T.eq(ctl.submit_hash(SIG), true)
	T.eq((m["e"] as ScfgEngine).pending[ctl.session_id]["hint_tx"], SIG)
	ctl = _started("base")["ctl"]
	T.eq(ctl.submit_hash(SIG), false)
	T.eq(ctl.error, "invalid_hash")
	T.ok("signature" in ScfgCheckoutController.MESSAGES["invalid_hash"], "the message names signatures")
	return T.done()


func test_transaction_that_never_landed_lets_the_player_pay_again() -> bool:
	var ctl: ScfgCheckoutController = _started("solana")["ctl"]
	T.eq(ctl.submit_hash(SIG), true)
	T.eq(ctl.hint_tx, SIG)
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": "tx_not_landed"}))
	T.eq(ctl.hint_tx, null)
	T.eq(ctl.state, "waiting")
	T.eq(ctl.message(), "That transaction didn't reach the network. You can pay again.")
	T.eq(ctl.status_info()["tx_hash"], null)
	T.eq(ctl.status_info()["error"], "tx_not_landed")
	return T.done()


func _info(ctl: ScfgCheckoutController) -> Array:
	var info: Dictionary = ctl.status_info()
	return [info["error"], info["tx_hash"], info["dropped_tx"]]


func test_status_names_the_signature_that_never_landed() -> bool:
	var ctl: ScfgCheckoutController = _started("solana")["ctl"]
	T.eq(ctl.status_info()["dropped_tx"], null)
	ctl.submit_hash(SIG)
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": "tx_not_landed", "hint_tx": SIG}))
	T.eq(_info(ctl), ["tx_not_landed", null, SIG])
	return T.done()


func test_a_new_hint_clears_the_old_not_landed_status() -> bool:
	var m := _started("solana")
	var ctl: ScfgCheckoutController = m["ctl"]
	ctl.submit_hash(SIG)
	ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": "tx_not_landed", "hint_tx": SIG}))
	ctl._report_hint(SIG2, DEV)  # the page's POST /tx for the next send
	T.eq(_info(ctl), [null, SIG2, null])
	T.eq(ctl.message(), null)
	T.eq((m["e"] as ScfgEngine).pending[ctl.session_id]["hint_tx"], SIG2)
	return T.done()


func test_a_verdict_on_an_older_hint_does_not_touch_the_newer_one() -> bool:
	# The worker may judge the old hint just before the page reports the next one.
	for error: String in ["tx_not_landed", "tx_reverted", "tx_not_a_payment"]:
		var ctl: ScfgCheckoutController = _started("solana")["ctl"]
		ctl.submit_hash(SIG)
		ctl._report_hint(SIG2, DEV)
		ctl.on_outcome(ScfgWatcher.step_outcome(ctl.session_id, "waiting", {"error": error, "hint_tx": SIG}))
		T.eq(_info(ctl), [null, SIG2, null], error)
	return T.done()


## What the Solana page is handed: the shared keys in Python's order, then wallet_chain, mint and
## reference, and the factory also gets build_tx.
func test_solana_session_info_key_order() -> bool:
	var m := _started("solana")
	var ctl: ScfgCheckoutController = m["ctl"]
	var seen := []
	ctl.server_factory = func(kw: Dictionary) -> Variant:
		var s := F.FakeServer.new(kw)
		seen.append(s)
		return s
	ctl.start_browser()
	T.eq(seen.size(), 1, "the page was built")
	if seen.is_empty():
		return T.done()
	var s: F.FakeServer = seen[0]
	T.eq(s.kw.keys(), ["session_info", "on_tx", "get_status", "build_tx"], "the factory's keys")
	T.eq(s.session_info.keys(), ["title", "kind", "token", "family", "decimals", "amount_units", "amount_text",
		"price_text", "recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent",
		"on_accent", "wallet_chain", "mint", "reference"], "session_info keys, in order")
	T.eq(s.session_info["family"], "solana")
	T.eq(s.session_info["mint"], MINT)
	T.eq(s.session_info["reference"], ctl.session["reference"])
	T.eq(typeof(s.session_info["amount_units"]), TYPE_STRING)
	return T.done()
