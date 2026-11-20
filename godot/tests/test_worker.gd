## ScfgWorker / ScfgJob (worker.gd): the twin of tests/unit/test_worker.py (9 tests), the
## non-frame-pumping part of test_pump_worker.py, and the JS core's rpgmaker/src/core/worker.test.ts
## (re-entrancy, cancelled exception, submit after stop, timer during a wake-triggered run).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")


## An Engine stand-in: tick/reverify/snapshot as coroutines, with an optional tick hook. Like
## ScfgEngine it has refuse_requests and refuse_in_flight(): a hook's request() is refused, or ended
## in flight, as the Engine's RefusingTransport would (test_pump_worker.py's FakeBridge).
class FakeEngine:
	extends RefCounted
	var ticks := 0
	var reverified := 0
	var statuses: Array = ["waiting"]
	var tick_impl: Callable = Callable()  # may be a coroutine; returns the outcomes (or an ScfgErr)
	var reverify_impl: Callable = Callable()
	var log: Array = []
	var refuse_requests: Callable = Callable()
	var flights: Array = []  # {label, gate, value} of the requests in flight, oldest first
	var cancelled: Array = []  # the labels of the requests refuse_in_flight() ended

	## A request: refused at once while refuse_requests says so, else in flight until answer()
	## (its value) or refuse_in_flight() (an Abandoned, resumed at the end of the frame).
	func request(label: String) -> Variant:
		if refuse_requests.is_valid() and refuse_requests.call():
			return ScfgErr.make("Abandoned", "abandoned before its request (%s)" % label)
		var flight := {"label": label, "gate": Gate.new(), "value": null}
		flights.append(flight)
		await (flight["gate"] as Gate).wait()
		return flight["value"]

	func in_flight() -> Array:
		return flights.map(func(f: Dictionary) -> String: return f["label"])

	## Answers the oldest request in flight.
	func answer(value: Variant) -> void:
		var flight: Dictionary = flights.pop_front()
		flight["value"] = value
		(flight["gate"] as Gate).open()

	func refuse_in_flight() -> void:
		if not (refuse_requests.is_valid() and refuse_requests.call()):
			return
		var ending := flights
		flights = []
		for flight: Dictionary in ending:
			cancelled.append(flight["label"])
			flight["value"] = ScfgErr.make("Abandoned", "abandoned while its request (%s) was in flight" % flight["label"])
			(flight["gate"] as Gate).open.call_deferred()

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


## The player's command does not wait behind a background tick. The tick's request in
## flight is ended (its answer never kept), the command runs next, and the next tick comes after
## it. No error is reported for the yield. (`tick` in the log is a tick starting, `tick:<v>` one
## finishing with its answer.)
func test_a_command_submitted_mid_tick_makes_the_tick_yield() -> bool:
	var e := FakeEngine.new()
	var errors := []
	e.tick_impl = func() -> Variant:
		var r: Variant = await e.request("tick")
		if r is ScfgErr:
			return r
		e.log.append("tick:%s" % r)
		return []
	var t := FakeTimer.new()
	var w := _timed(e, t, 10.0, {"on_error": func(x: Variant) -> void: errors.append(x)})
	w.start()
	t.fire()
	await _settle()
	T.eq(e.in_flight(), ["tick"])
	var job := w.submit(func(eng: Variant) -> Variant:
		var r: Variant = await eng.request("choose")
		e.log.append("choose:%s" % r)
		return r)
	T.eq(e.cancelled, ["tick"])
	await _settle()
	T.eq(e.in_flight(), ["choose"])
	e.answer("chosen")
	await _settle()
	T.eq(job.result(), "chosen")
	T.eq(e.in_flight(), ["tick"], "woken by the command")
	e.answer(1)
	await _settle()
	T.eq(e.log, ["reverify", "tick", "choose:chosen", "tick", "tick:1"])
	T.eq(errors, [])
	T.eq(t.pending["seconds"], 10.0, "then the loop waits the interval")
	w.stop()
	e.tick_impl = Callable()  # the hook captures e: break the cycle
	return T.done()


## Only Engine.tick yields: the launch reverify (run once) finishes first, and the tick then yields
## before its first request.
func test_the_first_run_reverify_does_not_yield() -> bool:
	var e := FakeEngine.new()
	e.reverify_impl = func() -> Variant:
		e.log.append("reverify:%s" % await e.request("reverify"))
		return []
	e.tick_impl = func() -> Variant:
		var r: Variant = await e.request("tick")
		if r is ScfgErr:
			return r
		e.log.append("tick:%s" % r)
		return []
	var w := _worker(e)
	w.run_once()
	await _settle()
	T.eq(e.in_flight(), ["reverify"])
	var job := w.submit(func(eng: Variant) -> Variant: return await eng.request("choose"))
	await _settle()
	T.eq([e.cancelled, e.in_flight(), job.done()], [[], ["reverify"], false])
	e.answer("ok")
	await _settle()
	T.eq([e.cancelled, e.in_flight()], [[], ["choose"]], "the tick yields before its request")
	T.eq(e.log, ["reverify", "reverify:ok", "tick"])
	e.answer("chosen")
	await _settle()
	e.answer(1)  # the follow-up run's tick
	await _settle()
	T.eq(job.result(), "chosen")
	T.eq(e.log, ["reverify", "reverify:ok", "tick", "tick", "tick:1"])
	e.tick_impl = Callable()  # the hooks capture e: break the cycle
	e.reverify_impl = Callable()
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


## A command never runs inside a tick: one submitted while the tick awaits the network makes the
## tick yield (test_a_command_submitted_mid_tick_makes_the_tick_yield), and runs once it has ended.
func test_commands_never_interleave_with_a_tick() -> bool:
	var e := FakeEngine.new()
	var st := {"in_tick": false, "during": null}
	e.tick_impl = func() -> Variant:
		if e.ticks == 1:
			st["in_tick"] = true
			var r: Variant = await e.request("tick")
			st["in_tick"] = false
			if r is ScfgErr:
				return r
		return []
	var w := _worker(e)
	w.run_once()
	await _settle()
	T.eq(st["in_tick"], true)
	var job := w.submit(func(_e: Variant) -> int:
		st["during"] = st["in_tick"]
		e.log.append("cmd")
		return 1)
	T.ok(not job.done(), "not executed inside submit(), while the tick still runs")
	await _settle()
	T.eq(job.result(), 1)
	T.eq(st["during"], false)
	T.eq(e.cancelled, ["tick"], "the tick yielded")
	# the command ran in the NEXT run: tick (1st run, yielded) ... cmd, then tick (2nd run)
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


## test_worker.py's test_an_abandoned_running_job_is_refused_its_next_request, with the real Engine:
## the player picks another network while a start waits on its first endpoint, which fails.
func test_an_abandoned_running_job_is_refused_its_next_request() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7",
		"chains": ["base"], "testnet": true})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var urls: Array = ScfgChains.TESTNET["base"]["rpc"]
	T.ok(urls.size() >= 2, "Base Sepolia has a second endpoint to fall back to")
	var box := {}
	var handlers := {}
	for u: Variant in urls:
		handlers[u] = func(_m: String, _p: Variant) -> Variant: return {"jsonrpc": "2.0", "id": 1, "result": "0x10"}
	handlers[urls[0]] = func(_m: String, _p: Variant) -> Variant:
		(box["job"] as ScfgJob).abandon()  # while its first request is in flight
		return ScfgCoio.transport_error("timed out")
	var transport := F.FakeTransport.new(handlers)
	var engine := ScfgEngine.new(cfg, {"transport": transport, "clock": func() -> int: return 1700000000})
	var w := ScfgWorker.new(engine, {})
	var job := w.submit(func(e: Variant) -> Variant: return await e.start_session("unlock", "base", {"item": "chapter2"}))
	box["job"] = job
	await w.run_once()
	T.is_err(job.result(), "Abandoned")
	var asked := transport.calls.filter(func(c: Array) -> bool: return c[1] == "eth_blockNumber").map(
		func(c: Array) -> Variant: return c[0])
	T.eq(asked, [urls[0]], "the second endpoint is not asked")
	T.ok(not engine.refuse_requests.is_valid(), "cleared after the job")
	return T.done()


## test_worker.py's twin, with the real Engine: a superseded start whose request is
## in flight (a host that never answers) ends at once when abandoned, not at the request's 10 s
## timeout; the transport is told to stop that request (ScfgCoio's CANCELLING).
func test_an_abandoned_job_stops_waiting_for_its_request_in_flight() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var cfg: Variant = ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7",
		"chains": ["base"], "testnet": true})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var node := F.CancellableNode.new()
	node.hang = true
	var engine := ScfgEngine.new(cfg, {"transport": node, "clock": func() -> int: return 1700000000})
	var w := ScfgWorker.new(engine, {})
	var job := w.submit(func(e: Variant) -> Variant: return await e.start_session("unlock", "base", {"item": "chapter2"}))
	tree.create_timer(0.2).timeout.connect(job.abandon)
	tree.create_timer(5.0).timeout.connect(node.release)  # a job left waiting fails below, not by the runner's timeout
	var started := Time.get_ticks_msec()
	await w.run_once()
	var took := (Time.get_ticks_msec() - started) / 1000.0
	T.ok(took < 2.0, "the abandoned start waited %.1f s for its request" % took)
	T.is_err(job.result(), "Abandoned")
	T.ok("in flight" in str(job.result()), str(job.result()))
	T.eq(node.cancelled, ["eth_blockNumber"], "the request in flight was stopped")
	T.eq(node.methods, ["eth_blockNumber"], "no other endpoint was asked")
	T.ok(not engine.refuse_requests.is_valid(), "cleared after the job")
	node.release()
	return T.done()


func test_abandoning_a_queued_job_cancels_it() -> bool:
	var w := _worker(FakeEngine.new())
	var ran := []
	var job := w.submit(func(_e: Variant) -> void: ran.append(1))
	job.abandon()
	await w.run_once()
	T.eq([job.cancelled(), ran, job.abandoned], [true, [], false])
	return T.done()
