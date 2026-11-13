## ScfgWorker / ScfgJob (worker.gd): the twin of tests/unit/test_worker.py (9 tests), the
## non-frame-pumping part of test_pump_worker.py, and the JS core's rpgmaker/src/core/worker.test.ts
## (re-entrancy, cancelled exception, submit after stop, timer during a wake-triggered run).
extends RefCounted

const T := preload("res://tests/t.gd")


## An Engine stand-in: tick/reverify/snapshot as coroutines, with an optional tick hook.
class FakeEngine:
	extends RefCounted
	var ticks := 0
	var reverified := 0
	var statuses: Array = ["waiting"]
	var tick_impl: Callable = Callable()  # may be a coroutine; returns the outcomes (or an ScfgErr)
	var reverify_impl: Callable = Callable()
	var log: Array = []

	func tick() -> Variant:
		ticks += 1
		log.append("tick")
		if tick_impl.is_valid():
			return await tick_impl.call()
		var out := []
		for s: String in statuses:
			out.append(ScfgWatcher.step_outcome("ps_1", s))  # err-ok: valid fields
		return out

	func reverify() -> Variant:
		reverified += 1
		log.append("reverify")
		if reverify_impl.is_valid():
			return await reverify_impl.call()
		return []

	func snapshot() -> Variant:
		return {"owned": ["tick%d" % ticks]}


## A manual scheduler: schedule() records, fire() runs the pending callback.
class FakeTimer:
	extends RefCounted
	var pending: Variant = null  # {"cb": Callable, "seconds": float}
	var scheduled: Array = []
	var cancelled := 0

	func schedule(cb: Callable, seconds: float) -> Variant:
		scheduled.append(seconds)
		pending = {"cb": cb, "seconds": seconds}
		return pending

	func cancel(handle: Variant) -> void:
		cancelled += 1
		if is_same(pending, handle):
			pending = null

	func fire() -> void:
		var p: Variant = pending
		pending = null
		if p != null:
			(p["cb"] as Callable).call()


## An awaitable latch.
class Gate:
	extends RefCounted
	signal opened
	var is_open := false

	func wait() -> void:
		if not is_open:
			await opened

	func open() -> void:
		is_open = true
		opened.emit()


static func _settle() -> void:
	for i in 5:
		await (Engine.get_main_loop() as SceneTree).process_frame


static func _worker(e: FakeEngine, opts: Dictionary = {}) -> ScfgWorker:
	return ScfgWorker.new(e, opts)


static func _timed(e: FakeEngine, t: FakeTimer, interval: float = 10.0, extra: Dictionary = {}) -> ScfgWorker:
	var opts := {"interval": func() -> float: return interval, "scheduler": t}
	opts.merge(extra, true)
	return ScfgWorker.new(e, opts)


# --- test_worker.py ----------------------------------------------------------------------------


func test_initial_snapshot_taken_at_construction() -> bool:
	var w := _worker(FakeEngine.new())
	T.eq(w.snapshot, {"owned": ["tick0"]})
	return T.done()


func test_run_once_reverifies_only_first_time_and_publishes_snapshot() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	T.eq(await w.run_once(), null)
	await w.run_once()
	T.eq(e.reverified, 1)
	T.eq(e.ticks, 2)
	T.eq(w.snapshot, {"owned": ["tick2"]})
	return T.done()


func test_waiting_outcomes_without_error_are_not_published() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	e.statuses = ["waiting", "confirming", "paid"]
	await w.run_once()
	var got: Array = w.drain_outcomes().map(func(o: Dictionary) -> String: return o["status"])
	T.eq(got, ["confirming", "paid"])
	T.eq(w.drain_outcomes(), [])
	return T.done()


func test_waiting_outcome_with_error_is_published() -> bool:
	var e := FakeEngine.new()
	e.tick_impl = func() -> Array:
		return [ScfgWatcher.step_outcome("ps_1", "waiting", {"error": "network_unavailable"})]  # err-ok: valid fields
	var w := _worker(e)
	await w.run_once()
	T.eq(w.drain_outcomes()[0]["error"], "network_unavailable")
	return T.done()


func test_submitted_commands_run_before_tick_and_resolve_jobs() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	var ok := w.submit(func(eng: FakeEngine) -> int: return eng.ticks)
	var bad := w.submit(func(_eng: Variant) -> Variant: return ScfgErr.make("ZeroDivisionError", "division by zero"))
	T.ok(not ok.done())
	await w.run_once()
	T.eq(ok.result(), 0)  # ran before the tick
	T.ok(bad.done())
	T.is_err(bad.result(), "ZeroDivisionError")
	T.is_err(bad.exception(), "ZeroDivisionError")
	T.eq(ok.exception(), null)
	return T.done()


func test_a_command_submitted_by_a_command_runs_before_the_same_tick() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	var inner: Array[ScfgJob] = []
	w.submit(func(_eng: Variant) -> Variant:
		inner.append(w.submit(func(eng: FakeEngine) -> int: return eng.ticks))
		return null)
	await w.run_once()
	T.eq(inner.size(), 1)
	T.eq(inner[0].result(), 0)  # drained in the same pass, as in Python
	return T.done()


func test_cancelled_job_is_skipped() -> bool:
	var w := _worker(FakeEngine.new())
	var ran := []
	var job := w.submit(func(_e: Variant) -> void: ran.append(1))
	T.eq(job.cancel(), true)
	await w.run_once()
	T.eq(ran, [])
	T.is_err(job.result(), "CancelledError")
	return T.done()


func test_timer_loop_wake_and_stop() -> bool:
	var e := FakeEngine.new()
	var t := FakeTimer.new()
	var w := _timed(e, t, 60.0)
	w.start()
	T.ok(w.running)
	T.eq(t.pending["seconds"], 0.0)  # first run at once
	t.fire()
	await _settle()
	T.eq(e.ticks, 1)
	T.eq(t.pending["seconds"], 60.0)  # long interval: only wake() can trigger tick 2
	var job := w.submit(func(_e: Variant) -> String: return "done")  # submit wakes the loop
	T.eq(t.pending["seconds"], 0.0)
	t.fire()
	await _settle()
	T.eq(job.result(), "done")
	T.eq(e.ticks, 2)
	T.ok(w.running)
	w.stop()
	T.ok(not w.running)
	T.eq(t.pending, null)
	return T.done()


func test_loop_survives_errors() -> bool:
	var e := FakeEngine.new()
	var errors := []
	var n := {"n": 0}
	e.tick_impl = func() -> Variant:
		n["n"] += 1
		if n["n"] == 1:
			return ScfgErr.make("RuntimeError", "boom")
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t, 0.01, {"on_error": func(x: Variant) -> void: errors.append(x)})
	w.start()
	t.fire()
	await _settle()
	T.eq(errors.size(), 1)
	T.is_err(errors[0], "RuntimeError")
	T.eq((errors[0] as ScfgErr).message, "boom")
	t.fire()  # the loop rescheduled itself
	await _settle()
	T.eq(n["n"], 2)
	w.stop()
	return T.done()


func test_async_commands_are_awaited() -> bool:
	var w := _worker(FakeEngine.new())
	var job := w.submit(func(_e: Variant) -> int:
		await (Engine.get_main_loop() as SceneTree).process_frame
		return 42)
	await w.run_once()
	T.eq(job.result(), 42)
	return T.done()


# --- test_pump_worker.py: the semantics that are not frame pumping ------------------------------


func test_pump_a_command_submitted_mid_tick_waits_for_the_tick() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	e.tick_impl = func() -> Array:
		await gate.wait()
		return []
	var w := _worker(e)
	w.run_once()
	await _settle()
	var job := w.submit(func(_e: Variant) -> String:
		e.log.append("choose")
		return "chosen")
	await _settle()
	T.ok(not job.done())
	gate.open()
	e.tick_impl = Callable()
	await _settle()
	T.eq(job.result(), "chosen")
	T.eq(e.log, ["reverify", "tick", "choose", "tick"])
	return T.done()


func test_pump_commands_run_before_a_due_tick_and_plain_results_resolve() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	var plain := w.submit(func(_e: Variant) -> String: return "now")
	w.submit(func(_e: Variant) -> void: e.log.append("first"))
	await w.run_once()
	T.eq(plain.result(), "now")
	T.eq(e.log, ["first", "reverify", "tick"])
	return T.done()


func test_pump_a_submit_landing_mid_run_gets_its_follow_up_at_once() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	e.tick_impl = func() -> Array:
		await gate.wait()
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t, 10.0)
	w.start()
	t.fire()
	await _settle()
	var job := w.submit(func(_e: Variant) -> String: return "hint")
	e.tick_impl = Callable()
	gate.open()
	await _settle()
	T.eq(job.result(), "hint")
	T.eq(e.ticks, 2)  # the follow-up tick it asked for ran immediately
	T.eq(t.pending["seconds"], 10.0)  # then the loop waits the interval
	w.stop()
	return T.done()


func test_pump_a_run_without_a_mid_run_wake_waits_the_interval() -> bool:
	var e := FakeEngine.new()
	var t := FakeTimer.new()
	var w := _timed(e, t, 10.0)
	w.wake()  # not started: nothing to wake
	T.eq(t.pending, null)
	w.start()
	t.fire()
	await _settle()
	T.eq(e.ticks, 1)
	T.eq(t.scheduled, [0.0, 10.0])
	w.stop()
	return T.done()


func test_pump_a_failing_tick_reports_and_schedules_the_next() -> bool:
	var e := FakeEngine.new()
	var errors := []
	var first := {"v": true}
	e.tick_impl = func() -> Variant:
		if first["v"]:
			first["v"] = false
			return ScfgErr.make("RuntimeError", "down")
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t, 10.0, {"on_error": func(x: Variant) -> void: errors.append(x)})
	w.start()
	t.fire()
	await _settle()
	T.eq(errors.size(), 1)
	t.fire()
	await _settle()
	T.eq(e.reverified, 1)  # reverify is not retried
	T.eq(e.ticks, 2)
	w.stop()
	return T.done()


func test_pump_stop_cancels_queued_commands_and_runs_nothing_more() -> bool:
	var e := FakeEngine.new()
	var t := FakeTimer.new()
	var w := _timed(e, t)
	w.start()
	var queued := w.submit(func(_e: Variant) -> String: return "later")
	w.stop()
	T.ok(queued.cancelled())
	T.ok(not w.running)
	T.eq(t.pending, null)
	T.eq(e.ticks, 0)
	return T.done()


# --- ordering, re-entrancy, stop and job failures -------------------------------------------------


func test_commands_never_interleave_with_a_tick() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	var st := {"in_tick": false, "during": null}
	e.tick_impl = func() -> Array:
		if e.ticks == 1:
			st["in_tick"] = true
			await gate.wait()
			st["in_tick"] = false
		return []
	var w := _worker(e)
	w.run_once()
	await _settle()
	T.eq(st["in_tick"], true)
	var job := w.submit(func(_e: Variant) -> int:
		st["during"] = st["in_tick"]
		e.log.append("cmd")
		return 1)
	await _settle()
	T.ok(not job.done())  # not executed while the tick awaits the network
	gate.open()
	await _settle()
	T.eq(job.result(), 1)
	T.eq(st["during"], false)
	# the command ran in the NEXT run: tick (1st run) ... cmd, then tick (2nd run)
	T.eq(e.log, ["reverify", "tick", "cmd", "tick"])
	T.eq(e.reverified, 1)
	e.tick_impl = Callable()  # the hook captures e: break the cycle
	return T.done()


func test_wake_during_a_run_schedules_exactly_one_follow_up() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	e.tick_impl = func() -> Array:
		if e.ticks == 1:
			await gate.wait()
		return []
	var w := _worker(e)
	w.run_once()
	await _settle()
	w.wake()
	w.wake()
	w.run_once()
	gate.open()
	await _settle()
	T.eq(e.ticks, 2)
	e.tick_impl = Callable()
	return T.done()


func test_first_run_reverifies_once() -> bool:
	var e := FakeEngine.new()
	var w := _worker(e)
	await w.run_once()
	T.eq(e.log, ["reverify", "tick"])  # reverify before the first tick
	await w.run_once()
	await w.run_once()
	T.eq(e.reverified, 1)
	return T.done()


func test_a_failing_reverify_ends_the_pass_and_is_not_retried() -> bool:
	var e := FakeEngine.new()
	e.reverify_impl = func() -> Variant: return ScfgErr.make("RuntimeError", "no chain")
	var w := _worker(e)
	var failure: Variant = await w.run_once()
	T.is_err(failure, "RuntimeError")
	T.eq(e.ticks, 0)
	T.eq(w.snapshot, {"owned": ["tick0"]})
	T.eq(await w.run_once(), null)
	T.eq(e.reverified, 1)
	T.eq(e.ticks, 1)
	return T.done()


func test_job_reports_exception() -> bool:
	var w := _worker(FakeEngine.new())
	var err := ScfgErr.make("TypeError", "bad")
	var bad := w.submit(func(_e: Variant) -> Variant: return err)
	var good := w.submit(func(_e: Variant) -> int: return 1)
	T.is_err(bad.result(), "RuntimeError")  # not done yet
	await w.run_once()
	T.ok(is_same(bad.exception(), err))
	T.eq(good.exception(), null)
	T.ok(is_same(bad.result(), err))
	return T.done()


func test_job_finished_signal_fires_once_per_job() -> bool:
	var w := _worker(FakeEngine.new())
	var seen := []
	var a := w.submit(func(_e: Variant) -> int: return 1)
	var b := w.submit(func(_e: Variant) -> int: return 2)
	a.finished.connect(func() -> void: seen.append("a"))
	b.finished.connect(func() -> void: seen.append("b"))
	b.cancel()
	T.eq(seen, ["b"])
	await w.run_once()
	T.eq(seen, ["b", "a"])
	T.eq(a.cancel(), false)  # finished: too late
	return T.done()


func test_stop_cancels_the_timer() -> bool:
	var t := FakeTimer.new()
	var w := _timed(FakeEngine.new(), t)
	w.start()
	T.ok(t.pending != null)
	w.stop()
	T.eq(t.cancelled, 1)
	T.eq(t.pending, null)
	T.ok(not w.running)
	return T.done()


func test_no_run_after_stop_even_if_a_run_is_in_flight() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	e.tick_impl = func() -> Array:
		await gate.wait()
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t)
	w.start()
	t.fire()
	await _settle()
	w.wake()  # would normally add a follow-up
	w.stop()
	gate.open()
	await _settle()
	T.eq(e.ticks, 1)
	T.eq(t.pending, null)
	return T.done()


func test_job_pending_result_is_an_error() -> bool:
	var j := ScfgJob.new()
	T.is_err(j.result(), "RuntimeError")
	T.eq(j.exception(), null)
	T.ok(not j.done())
	return T.done()


func test_reentrant_run_once_from_a_command_never_overlaps_ticks() -> bool:
	var e := FakeEngine.new()
	var st := {"live": 0, "max": 0}
	e.tick_impl = func() -> Array:
		st["live"] += 1
		st["max"] = maxi(st["max"], st["live"])
		await (Engine.get_main_loop() as SceneTree).process_frame
		st["live"] -= 1
		return []
	var w := _worker(e)
	w.submit(func(_e: Variant) -> void: w.run_once())
	await w.run_once()
	await _settle()
	T.eq(st["max"], 1)
	T.eq(e.ticks, 2)  # exactly one follow-up
	return T.done()


func test_wake_and_submit_from_a_command_give_one_follow_up() -> bool:
	var e := FakeEngine.new()
	var st := {"live": 0, "max": 0}
	e.tick_impl = func() -> Array:
		st["live"] += 1
		st["max"] = maxi(st["max"], st["live"])
		await (Engine.get_main_loop() as SceneTree).process_frame
		st["live"] -= 1
		return []
	var w := _worker(e)
	var inner := []
	w.submit(func(_e: Variant) -> void:
		w.wake()
		inner.append(w.submit(func(_e2: Variant) -> String: return "inner")))
	await w.run_once()
	T.eq(st["max"], 1)
	T.eq(e.ticks, 2)
	T.eq((inner[0] as ScfgJob).result(), "inner")
	T.eq(e.log.filter(func(x: String) -> bool: return x == "tick").size(), 2)
	return T.done()


func test_cancelled_job_exception_is_the_cancelled_error() -> bool:
	var j := ScfgJob.new()
	j.cancel()
	T.is_err(j.exception(), "CancelledError")
	T.ok(is_same(j.result(), j.exception()))
	T.ok(j.cancelled())
	return T.done()


func test_submit_on_a_stopped_worker_is_cancelled_but_before_start_it_queues() -> bool:
	var w := _worker(FakeEngine.new())
	var early := w.submit(func(_e: Variant) -> String: return "queued")  # never started: queues
	T.ok(not early.done())
	w.stop()
	T.ok(early.cancelled())
	var late := w.submit(func(_e: Variant) -> String: return "never")
	T.ok(late.cancelled())
	T.is_err(late.exception(), "CancelledError")
	w.start()  # a restart accepts work again
	var again := w.submit(func(_e: Variant) -> String: return "ok")
	T.ok(not again.done())
	w.stop()
	return T.done()


func test_timer_firing_during_a_wake_triggered_run_yields_one_follow_up() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	var st := {"live": 0, "max": 0}
	e.tick_impl = func() -> Array:
		st["live"] += 1
		st["max"] = maxi(st["max"], st["live"])
		if e.ticks == 1:
			await gate.wait()
		st["live"] -= 1
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t, 10.0)
	w.start()  # timer armed at 0
	w.run_once()  # a run in flight, started outside the timer
	await _settle()
	w.wake()  # asks for a follow-up
	t.fire()  # the timer fires mid-run: joins the same chain
	gate.open()
	await _settle()
	T.eq(st["max"], 1)
	T.eq(e.ticks, 2)
	w.stop()
	e.tick_impl = Callable()
	return T.done()


func test_joining_run_once_returns_the_chain_failure() -> bool:
	var e := FakeEngine.new()
	var gate := Gate.new()
	e.tick_impl = func() -> Variant:
		await gate.wait()
		return ScfgErr.make("RuntimeError", "first")
	var w := _worker(e)
	w.run_once()
	await _settle()
	e.tick_impl = Callable()
	var joined := {"v": null}
	var waiter := func() -> void: joined["v"] = await w.run_once()
	waiter.call()
	gate.open()
	await _settle()
	T.is_err(joined["v"], "RuntimeError")
	T.eq(e.ticks, 2)
	return T.done()


# --- ScfgJob.wait() -----------------------------------------------------------------------------------


func test_job_wait_returns_at_once_on_a_done_job() -> bool:
	var w := _worker(FakeEngine.new())
	var err := ScfgErr.make("TypeError", "bad")
	var good := w.submit(func(_e: Variant) -> int: return 7)
	var bad := w.submit(func(_e: Variant) -> Variant: return err)
	var gone := w.submit(func(_e: Variant) -> int: return 1)
	gone.cancel()
	await w.run_once()
	T.ok(good.done() and bad.done() and gone.done(), "all three are done")
	# `await job.finished` here would never resume: the signal fired before we asked
	T.eq(await good.wait(), 7, "the value of a job that succeeded long ago")
	T.ok(is_same(await bad.wait(), err), "the failure itself")
	T.is_err(await gone.wait(), "CancelledError", "a cancelled job")
	return T.done()


func test_job_wait_resumes_when_the_job_finishes() -> bool:
	var w := _worker(FakeEngine.new())
	var job := w.submit(func(_e: Variant) -> int: return 5)
	var seen := []
	var waiter := func() -> void: seen.append(await job.wait())
	waiter.call()  # statement form: suspends on the pending job
	T.eq(seen, [], "pending: still waiting")
	await w.run_once()
	T.eq(seen, [5], "resumed with the result")
	var cancelled := w.submit(func(_e: Variant) -> int: return 1)
	var seen2 := []
	var waiter2 := func() -> void: seen2.append(await cancelled.wait())
	waiter2.call()
	cancelled.cancel()
	T.eq(seen2.size(), 1, "a cancel resumes the waiter")
	if seen2.size() == 1:
		T.is_err(seen2[0], "CancelledError", "with the CancelledError")
	return T.done()


# --- the default scheduler --------------------------------------------------------------------------


func test_tree_scheduler_fires_and_cancels() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var sched := ScfgTreeScheduler.new(tree)
	var hits := []
	var h1: Variant = sched.schedule(func() -> void: hits.append(1), 0.05)
	var h2: Variant = sched.schedule(func() -> void: hits.append(2), 0.05)
	sched.cancel(h2)
	var t0 := Time.get_ticks_msec()
	while hits.is_empty() and Time.get_ticks_msec() - t0 < 3000:
		await tree.process_frame
	await tree.create_timer(0.15).timeout
	T.eq(hits, [1])
	T.ok(h1 != null)
	return T.done()


func test_tree_scheduler_survives_a_freed_target() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var sched := ScfgTreeScheduler.new(tree)
	var node := Node.new()
	var dead := Callable(node, "queue_free")
	node.free()
	T.ok(not dead.is_valid(), "the callable is invalid once its object is freed")
	var handle: Variant = sched.schedule(dead, 0.02)
	await tree.create_timer(0.1).timeout  # an unguarded call would log an engine error here
	T.eq((handle as Dictionary)["live"], false, "the timer fired and was retired")
	return T.done()


func test_worker_builds_the_default_scheduler_from_a_tree() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var e := FakeEngine.new()
	var w := ScfgWorker.new(e, {"tree": tree, "interval": func() -> float: return 30.0})
	w.start()
	var t0 := Time.get_ticks_msec()
	while e.ticks == 0 and Time.get_ticks_msec() - t0 < 3000:
		await tree.process_frame
	w.stop()
	T.eq(e.ticks, 1)
	T.eq(e.reverified, 1)
	return T.done()
