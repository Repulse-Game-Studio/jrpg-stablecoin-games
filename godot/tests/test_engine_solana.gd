## ScfgEngine with Solana sessions: tests/unit/test_engine_solana.py, plus the JS core's checks
## (rpgmaker/src/core/engine_solana.test.ts): readiness hides nothing on network doubt, and a game
## with only previous Solana wallets still judges its Solana receipts.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV_EVM := "0x52908400098527886E0F7030069857D2E4169EE7"
const DEV := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const OLD := "4SELZ24sTinbBYo27VvsUAHepTRXGVBghc8WTUpvx6Pn"
const PAYER := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"

static var MINT: String = ScfgChains.MAINNET["solana"]["tokens"]["USDC"]["address"]
static var USDT_MINT: String = ScfgChains.MAINNET["solana"]["tokens"]["USDT"]["address"]
static var SIG: String = F.sig(7)
static var DEV_USDC_ATA: Variant = ScfgSolanaPda.associated_token_address(DEV, MINT)
static var DEV_USDT_ATA: Variant = ScfgSolanaPda.associated_token_address(DEV, USDT_MINT)


## {e, sol, evm, clock} (test_engine_solana.py `make`).
func _make() -> Dictionary:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV_EVM, "solana_wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var sol := F.FakeSolanaChain.new(1000)
	var evm := F.FakeChain.new(1000)
	sol.accounts[DEV_USDC_ATA] = true
	sol.accounts[DEV_USDT_ATA] = true
	var clock := F.FakeClock.new(100.0)
	var factory := func(chain: Dictionary) -> Variant: return sol if chain["family"] == "solana" else evm
	return {"e": ScfgEngine.new(cfg, {"rpc_factory": factory, "clock": clock.fn}), "sol": sol, "evm": evm, "clock": clock}


func _statuses(outcomes: Variant, sid: Variant) -> Array:
	var out := []
	for o: Dictionary in outcomes:
		if o["session_id"] == sid:
			out.append(o["status"])
	return out


func test_solana_payment_end_to_end() -> bool:
	var m := _make()
	var e: ScfgEngine = m["e"]
	var sol: F.FakeSolanaChain = m["sol"]
	var rec: Variant = await e.start_session("unlock", "solana", {"item": "chapter2"})
	T.ok(rec is Dictionary, "a session")
	if not rec is Dictionary:
		return T.done()
	T.eq(rec["dest"], DEV_USDC_ATA, "the developer's token account")
	T.ok(ScfgPy.py_truthy(rec["reference"]), "a reference")
	sol.pay(SIG, {"mint": MINT, "payer": PAYER, "owner": DEV, "amount": rec["units"],
		"addresses": [rec["reference"], rec["dest"]]})
	T.eq(_statuses(await e.tick(), rec["id"]), ["confirming"], "confirming")
	sol.finalize(SIG)
	var paid := (await e.tick() as Array).filter(func(o: Dictionary) -> bool: return o["status"] == "paid")
	T.eq(paid.size(), 1, "paid")
	if paid.size() == 1:
		T.eq(paid[0]["result"]["chain"], "solana", "on Solana")
	T.eq(e.snapshot()["owned"], ["chapter2"], "owned")
	T.eq(await e.reverify(), [], "the receipt is genuine")
	T.eq(e.snapshot()["unready"], [], "both token accounts exist")
	e.receipts.clear()
	var found: Variant = await e.restore_lookup(SIG)
	T.eq((found as Array).map(func(c: Dictionary) -> Variant: return c["item"]) if found is Array else found, ["chapter2"],
		"restored from the signature")
	return T.done()


func test_hint_on_solana_session_must_be_a_signature() -> bool:
	var e: ScfgEngine = _make()["e"]
	var rec: Variant = await e.start_session("tip", "solana", {"amount": "1", "token": "USDC"})
	T.eq(e.report_hint(rec["id"], SIG), null, "a signature")
	T.eq(e.pending[rec["id"]]["hint_tx"], SIG, "stored")
	T.is_err(e.report_hint(rec["id"], "0x" + "ab".repeat(32)), "ValueError", "an EVM hash")
	return T.done()


func test_missing_token_account_marks_token_unready() -> bool:
	var m := _make()
	var e: ScfgEngine = m["e"]
	(m["sol"] as F.FakeSolanaChain).accounts.clear()
	await e.reverify()
	T.eq(e.snapshot()["unready"], [["solana", "USDC"], ["solana", "USDT"]], "both tokens")
	return T.done()


func test_readiness_check_failure_assumes_ready() -> bool:
	var m := _make()
	var e: ScfgEngine = m["e"]
	var sol: F.FakeSolanaChain = m["sol"]
	sol.accounts.clear()
	sol.down = true
	await e.reverify()
	T.eq(e.snapshot()["unready"], [], "nothing hidden")
	return T.done()


func test_readiness_check_hides_nothing_on_network_doubt() -> bool:
	for kind: String in ["RpcUnavailable", "RpcError"]:
		var m := _make()
		var e: ScfgEngine = m["e"]
		var sol: F.FakeSolanaChain = m["sol"]
		e.unready = {["solana", "USDC"]: true}  # a stale verdict is replaced, not kept
		var errors := []
		e.on_error = func(x: Variant) -> void: errors.append(x)
		sol.accounts.clear()
		sol.patched["account_exists"] = func(_a: Variant) -> Variant:
			return ScfgErr.make(kind, "node trouble", -32005 if kind == "RpcError" else null)
		await e.reverify()
		T.eq(e.snapshot()["unready"], [], "%s: nothing hidden" % kind)
		T.eq(errors, [], "%s: not reported" % kind)
	return T.done()


## An EVM session left in persistent by a build that accepted EVM, loaded by a build that is
## Solana-only: its chain is still in the registry but has no wallet.
func _evm_session_in_solana_only_game() -> Dictionary:
	var old: ScfgEngine = _make()["e"]
	var rec: Variant = await old.start_session("tip", "arbitrum", {"amount": "1", "token": "USDC"})
	var cfg: Variant = ScfgConfig.build_config({"solana_wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var sol := F.FakeSolanaChain.new(1000)
	var evm := F.FakeChain.new(1000)
	var factory := func(chain: Dictionary) -> Variant: return sol if chain["family"] == "solana" else evm
	var errors := []
	var clock := F.FakeClock.new(100.0)
	var e := ScfgEngine.new(cfg, {"rpc_factory": factory, "clock": clock.fn, "pending": old.pending,
		"on_error": func(x: Variant) -> void: errors.append(x)})
	return {"e": e, "sol": sol, "stale": rec, "clock": clock, "errors": errors}


func test_session_without_a_wallet_for_its_chain_does_not_stall_the_others() -> bool:
	var g: Dictionary = await _evm_session_in_solana_only_game()
	var e: ScfgEngine = g["e"]
	var rec: Variant = await e.start_session("unlock", "solana", {"item": "chapter2"})
	(g["sol"] as F.FakeSolanaChain).pay(SIG, {"mint": MINT, "payer": PAYER, "owner": DEV, "amount": rec["units"],
		"addresses": [rec["reference"], rec["dest"]]})
	T.eq(_statuses(await e.tick(), rec["id"]), ["confirming"], "confirming")
	(g["sol"] as F.FakeSolanaChain).finalize(SIG)
	var outcomes: Variant = await e.tick()
	T.eq(_statuses(outcomes, rec["id"]), ["paid"], "paid")
	T.eq(_statuses(outcomes, g["stale"]["id"]), [], "skipped silently")
	T.ok(e.pending.has(g["stale"]["id"]), "kept")
	T.eq(g["errors"], [], "no error")
	return T.done()


func test_session_on_an_unwatchable_chain_is_closed_after_the_horizon() -> bool:
	var g: Dictionary = await _evm_session_in_solana_only_game()
	var e: ScfgEngine = g["e"]
	var stale_id: String = g["stale"]["id"]
	e.pending[stale_id]["chain"] = "removed_custom_chain"  # gone from the registry too
	var other: Dictionary = (e.pending[stale_id] as Dictionary).duplicate(true)
	other["id"] = "ps_other"
	other["chain"] = "arbitrum"
	e.pending["ps_other"] = other
	(g["clock"] as F.FakeClock).advance(ScfgSessions.SCAN_HORIZON_S)
	var closed := []
	for o: Dictionary in await e.tick():
		if o["status"] == "closed":
			closed.append(o["session_id"])
	closed.sort()
	var want := [stale_id, "ps_other"]
	want.sort()
	T.eq(closed, want, "both closed")
	T.eq(e.pending, {}, "removed")
	return T.done()


func test_an_exception_in_one_session_does_not_stop_the_others() -> bool:
	var errors := []
	var m := _make()
	var e: ScfgEngine = m["e"]
	e.on_error = func(x: Variant) -> void: errors.append(x)
	var evm_rec: Variant = await e.start_session("tip", "arbitrum", {"amount": "1", "token": "USDC"})
	var rec: Variant = await e.start_session("unlock", "solana", {"item": "chapter2"})
	var boom := ScfgErr.make("RuntimeError", "boom")
	e._rpcs["arbitrum"] = BrokenChain.new(boom)
	(m["sol"] as F.FakeSolanaChain).pay(SIG, {"mint": MINT, "payer": PAYER, "owner": DEV, "amount": rec["units"],
		"addresses": [rec["reference"], rec["dest"]]})
	var outcomes := {}
	for o: Dictionary in await e.tick():
		outcomes[o["session_id"]] = o
	T.eq([outcomes[evm_rec["id"]]["status"], outcomes[evm_rec["id"]]["error"]], ["waiting", "internal_error"],
		"the EVM session failed")
	T.eq(outcomes[rec["id"]]["status"], "confirming", "the Solana one went on")
	T.eq(errors, [boom], "reported once, as itself")
	T.ok(e.pending.has(evm_rec["id"]), "kept")
	return T.done()


## A FakeChain whose block_number fails with `err` (Python's `e.rpc("arbitrum").block_number = broken`).
class BrokenChain:
	extends RefCounted
	var _err: ScfgErr

	func _init(err: ScfgErr) -> void:
		_err = err

	## @fallible
	func block_number() -> Variant:
		return _err


func test_readiness_check_crash_assumes_ready_and_is_reported() -> bool:
	var errors := []
	var m := _make()
	var e: ScfgEngine = m["e"]
	var sol: F.FakeSolanaChain = m["sol"]
	e.on_error = func(x: Variant) -> void: errors.append(x)
	sol.accounts.clear()
	sol.patched["account_exists"] = func(_a: Variant) -> Variant: return ScfgErr.make("KeyError", "'value'")
	await e.reverify()
	T.eq(e.snapshot()["unready"], [], "nothing hidden")
	T.eq(errors.size(), 1, "reported once")
	T.is_err(errors[0] if errors.size() > 0 else null, "KeyError", "as itself")
	return T.done()


## No solana_wallet any more, but Solana receipts paid to a previous one: the Solana backend judges
## them through Engine.reverify, and readiness skips Solana (it is not enabled).
func test_reverify_judges_solana_receipts_of_a_game_with_only_previous_solana_wallets() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV_EVM, "previous_wallets": [OLD]})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	T.eq(cfg["solana_wallet"], null, "no Solana wallet")
	var sol := F.FakeSolanaChain.new(1000)
	sol.pay(F.sig(1), {"mint": MINT, "payer": PAYER, "owner": OLD, "amount": 4_993_742, "slot": 950})
	var asked := []
	var receipt := func(n: int) -> Dictionary:
		return {"chain": "solana", "tx_hash": F.sig(n), "log_index": 2, "block": 950, "kind": "unlock", "item": "chapter2",
			"amount": "4.993742", "token": "USDC", "payer": PAYER, "recipient": OLD, "source": "payment"}
	var k1 := "solana:%s:2" % F.sig(1)
	var k2 := "solana:%s:2" % F.sig(2)  # invented, and history covers its slot
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(c: Dictionary) -> Variant:
		asked.append(c["key"])
		return sol if c["family"] == "solana" else F.FakeChain.new(), "clock": func() -> float: return 0.0,
		"receipts": {k1: receipt.call(1), k2: receipt.call(2)}})
	T.eq(await e.reverify(), [k2], "the invented receipt is removed")
	T.eq(e.receipts.keys(), [k1], "the genuine one kept")
	T.eq(e.snapshot()["owned"], ["chapter2"], "owned")
	T.eq(asked, ["solana"], "readiness never built another client")
	T.ok(not "account_exists" in sol.calls, "and never touched Solana")
	return T.done()


## Engine.LazyRpc (Python's _LazyRpc): the chain's client is built only when a readiness check
## calls it, and a method the client lacks is Python's AttributeError, contained like any other
## readiness failure (never a script error).
func test_readiness_reaches_the_client_lazily_and_a_missing_method_is_an_attribute_error() -> bool:
	var m := _make()
	var e: ScfgEngine = m["e"]
	var lazy := ScfgEngine.LazyRpc.new(e, "solana")
	T.eq(e._rpcs.keys(), [], "nothing built yet")
	T.eq(await lazy.account_exists(DEV_USDC_ATA), true, "forwarded")
	T.eq(e._rpcs.keys(), ["solana"], "built on first use")
	var bare := ScfgEngine.new(e.config, {"rpc_factory": func(_c: Dictionary) -> Variant: return RefCounted.new()})
	var errors := []
	bare.on_error = func(x: Variant) -> void: errors.append(x)
	T.is_err(await ScfgEngine.LazyRpc.new(bare, "solana").account_exists(DEV_USDC_ATA), "AttributeError",
		"a client without account_exists")
	T.eq(await bare.reverify(), [], "reverify still answers")
	T.eq(errors.size(), 1, "reported once")
	T.is_err(errors[0] if errors.size() > 0 else null, "AttributeError", "as Python's getattr")
	T.eq(bare.unready, {}, "nothing hidden")
	var failing := ScfgEngine.new(e.config, {"rpc_factory": func(_c: Dictionary) -> Variant:
		return ScfgErr.make("ValueError", "no urls")})
	T.is_err(await ScfgEngine.LazyRpc.new(failing, "solana").account_exists(DEV_USDC_ATA), "ValueError",
		"the factory's error, raised where Python's getattr would")
	return T.done()


func test_default_error_hook_prints_the_traceback() -> bool:
	# Python checks stderr; GDScript cannot capture printerr, so this checks that the default hook
	# is installed and runs without failing the tick.
	var base := _make()
	var e := ScfgEngine.new((base["e"] as ScfgEngine).config, {"rpc_factory": func(c: Dictionary) -> Variant:
		return (base["e"] as ScfgEngine).rpc(c["key"]), "clock": func() -> float: return 100.0})
	T.ok(e.on_error == ScfgEngine._print_error, "the default hook")
	var rec: Variant = await e.start_session("tip", "arbitrum", {"amount": "1", "token": "USDC"})
	# the default hook prints this error to stderr on purpose: the line is expected on a passing run
	e._rpcs["arbitrum"] = BrokenChain.new(ScfgErr.make("RuntimeError", "boom (expected: test_default_error_hook)"))
	var errors := []
	for o: Dictionary in await e.tick():
		if o["session_id"] == rec["id"]:
			errors.append(o["error"])
	T.eq(errors, ["internal_error"], "contained")
	return T.done()
