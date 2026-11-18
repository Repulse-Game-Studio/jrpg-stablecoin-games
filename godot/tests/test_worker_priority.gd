## The twin of tests/unit/test_worker_priority.py (testbed U15): a session start the player waits
## on comes before the background watching of pending payments. With pending sessions whose checks
## hang (a DNS that never answers), the start submitted while a tick runs makes the tick yield at
## once, its request in flight included, and starts in well under the per-request timeout; the
## yielded tick keeps its outcomes and the next tick steps the sessions again. A real ScfgEngine on
## a real ScfgWorker driven by the scene tree's timers.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"


func test_a_start_does_not_wait_behind_the_watching_of_pending_payments() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV, "chains": ["base", "arbitrum"], "testnet": true})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var node := F.HangingNode.new()  # cannot stop a request: the one in flight is left to finish
	var errors := []
	var engine := ScfgEngine.new(cfg, {"transport": node, "clock": func() -> int: return 1700000000,
		"on_error": func(x: Variant) -> void: errors.append(x)})
	for i in 3:  # three pending tips, as in the testbed
		var rec: Variant = await engine.start_session("tip", "base", {"amount": "1.00", "token": "USDC"})
		T.ok(rec is Dictionary, str(rec))
	T.eq(engine.pending.size(), 3)
	var w := ScfgWorker.new(engine, {"interval": func() -> float: return 60.0,
		"on_error": func(x: Variant) -> void: errors.append(x), "tree": tree})
	node.hang = true
	w.start()
	var waited := 0
	while node.hung == 0 and waited < 300:
		await tree.process_frame
		waited += 1
	T.ok(node.hung > 0, "the tick asked the node")
	node.hang = false  # the network is back for the start (the tick's request still hangs)
	tree.create_timer(5.0).timeout.connect(node.release)  # a start left waiting fails below
	var started := Time.get_ticks_msec()
	var job := w.submit(func(e: Variant) -> Variant: return await e.start_session("unlock", "arbitrum", {"item": "chapter2"}))
	var rec: Variant = await job.wait()
	var took := (Time.get_ticks_msec() - started) / 1000.0
	T.ok(took < 2.0, "the start waited %.1f s behind the tick" % took)
	T.ok(rec is Dictionary and rec.get("chain") == "arbitrum", str(rec))
	T.eq(engine.pending.size(), 4, "the pending payments are untouched")
	T.eq(errors, [])
	node.release()
	w.stop()
	return T.done()


## The hinted transaction reverted (eth_getTransactionReceipt status 0x0); every other request
## hangs while `hang` is set.
class RevertingNode:
	extends F.HangingNode

	func post_json(url: String, payload: Dictionary, timeout: float) -> Variant:
		if payload["method"] == "eth_getTransactionReceipt":
			methods.append(payload["method"])
			var receipt := {"status": "0x0", "blockNumber": "0x3e8", "logs": []}
			return {"jsonrpc": "2.0", "id": payload["id"], "result": receipt}
		return await super.post_json(url, payload, timeout)


## Review V1: the tick judged the hint (reverted) and cleared it, then waits on its next request;
## a command makes it yield. The verdict is still reported, not lost with the step.
func test_a_hint_verdict_reached_before_the_tick_yields_is_reported() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV, "chains": ["base"], "testnet": true})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var node := RevertingNode.new()
	var errors := []
	var engine := ScfgEngine.new(cfg, {"transport": node, "clock": func() -> int: return 1700000000,
		"on_error": func(x: Variant) -> void: errors.append(x)})
	var rec: Variant = await engine.start_session("unlock", "base", {"item": "chapter2"})
	T.ok(rec is Dictionary, str(rec))
	var sid: Variant = rec["id"]
	var tx := "0x" + "ab".repeat(32)
	T.eq(engine.report_hint(sid, tx), null, "the hint is recorded")
	var w := ScfgWorker.new(engine, {"interval": func() -> float: return 60.0,
		"on_error": func(x: Variant) -> void: errors.append(x), "tree": tree})
	node.hang = true
	w.start()
	var waited := 0
	while node.hung == 0 and waited < 300:
		await tree.process_frame
		waited += 1
	T.ok(node.hung > 0, "the tick went on to its next request after the verdict")
	T.eq(engine.pending[sid]["hint_tx"], null, "the hint is cleared already")
	tree.create_timer(5.0).timeout.connect(node.release)  # a command left waiting fails below
	var job := w.submit(func(_e: Variant) -> Variant: return "the player's command")
	T.eq(await job.wait(), "the player's command", "the command ran")
	var got := []
	for o: Dictionary in w.drain_outcomes():
		got.append([o["session_id"], o["error"], o["hint_tx"]])
	T.eq(got, [[sid, "tx_reverted", tx]], "the verdict is reported")
	T.eq(errors, [])
	node.release()
	w.stop()
	return T.done()
