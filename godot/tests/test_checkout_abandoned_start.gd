## A session start the player moved away from (another network, another tip amount, Cancel) must
## not leave a session behind: it would be watched for 24 h and reported `expired` to on_result 30
## minutes later, for a payment the player never saw.
## tests/unit/test_checkout_abandoned_start.py, same tests, same names. The deferred submitter
## runs a queued command only when `run()` is awaited.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"


## {ctl, engine, submit, clock} (test_checkout_abandoned_start.py `setup`).
func _setup(kind: String = "unlock") -> Dictionary:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var fakes := {}
	var clock := F.FakeClock.new(100.0)
	var factory := func(c: Dictionary) -> Variant:
		if not fakes.has(c["key"]):
			fakes[c["key"]] = F.FakeChain.new(1000)
		return fakes[c["key"]]
	var engine := ScfgEngine.new(cfg, {"rpc_factory": factory, "clock": clock.fn})
	var spec: Variant
	if kind == "unlock":
		spec = ScfgPayments.prepare_payment(cfg, "unlock", {"item": "chapter2"})
	else:
		spec = ScfgPayments.prepare_payment(cfg, "tip", {"amount": "3", "token": "USDC"})
	var sub := F.DeferredSubmitter.new(engine)
	var ctl := ScfgCheckoutController.new({"config": cfg, "spec": spec, "submit": sub.submit})
	return {"ctl": ctl, "engine": engine, "submit": sub, "clock": clock}


func test_switching_network_while_starting_discards_the_first_session() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var sub: F.DeferredSubmitter = m["submit"]
	ctl.choose_network("arbitrum")
	ctl.choose_network("base")
	await sub.run()
	ctl.poll()
	T.eq(ctl.state, "waiting")
	T.eq(ctl.chain_key, "base")
	await sub.run()  # the discard queued once the abandoned start finished
	T.eq(engine.pending.values().map(func(r: Dictionary) -> Variant: return r["chain"]), ["base"])
	return T.done()


func test_changing_the_tip_while_starting_discards_the_first_session() -> bool:
	var m := _setup("tip")
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var sub: F.DeferredSubmitter = m["submit"]
	ctl.next_step()
	ctl.choose_network("arbitrum")
	T.ok(ctl.set_tip_amount("5"), "a valid amount")
	await sub.run()
	ctl.poll()
	await sub.run()
	T.eq(engine.pending.size(), 1)
	T.eq(engine.pending.values()[0]["price"], "5.00")
	return T.done()


func test_cancel_while_starting_leaves_no_session_to_expire() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var sub: F.DeferredSubmitter = m["submit"]
	ctl.choose_network("arbitrum")
	T.eq(ctl.cancel()["status"], "cancelled")
	await sub.run()
	await sub.run()
	T.eq(engine.pending, {})
	(m["clock"] as F.FakeClock).advance(31 * 60)
	var expired: Array = (await engine.tick()).filter(func(o: Dictionary) -> bool: return o["status"] == "expired")
	T.eq(expired, [])
	return T.done()


func test_a_failed_abandoned_start_needs_no_discard() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var sub: F.DeferredSubmitter = m["submit"]
	ctl.choose_network("arbitrum")
	ctl.cancel()
	var entry: Array = sub.queue.pop_front()
	(entry[1] as ScfgJob)._start()
	(entry[1] as ScfgJob)._fail(ScfgErr.make("RuntimeError", "rpc down"))
	await sub.run()
	T.eq(engine.pending, {})
	T.eq(sub.queue, [], "no discard was queued")
	return T.done()


## The Python callback "runs at once when the future is already done": abandoning a start whose
## job has finished still discards its session.
func test_abandoning_an_already_finished_start_discards_at_once() -> bool:
	var m := _setup()
	var ctl: ScfgCheckoutController = m["ctl"]
	var engine: ScfgEngine = m["engine"]
	var sub: F.DeferredSubmitter = m["submit"]
	ctl.choose_network("arbitrum")
	await sub.run()  # the start finished, but poll() has not picked it up
	T.eq(engine.pending.size(), 1, "the session exists")
	ctl.cancel()
	await sub.run()
	T.eq(engine.pending, {}, "and was discarded")
	return T.done()
