## The single writer: one command queue plus a periodic tick loop that owns the ScfgEngine.
##
## Twin of scfg_core.worker.Worker (threads) and scfg_core.pump_worker.PumpWorker, and of the TS
## core's rpgmaker/src/core/worker.ts, as one cooperative object (the Godot glue hosts it in the
## `Scfg` autoload).
## The game loop never touches the Engine directly. It:
##   - submits commands with `submit(fn)` and polls the returned ScfgJob every frame (`fn` gets
##     the engine, may be a coroutine, and fails its job by returning an ScfgErr),
##   - reads the latest detached snapshot from `.snapshot` (replaced after each run; the Engine's
##     own error value if its snapshot() failed, so check it),
##   - collects StepOutcomes with `drain_outcomes()` (a "waiting" outcome only when it has an error).
##
## ONE RUN AT A TIME. `run_once()` is a coroutine (callers await it): the queued commands
## (in order, until none is left), `reverify()` on the very first run, `tick()`, then the snapshot. It returns null, or
## the first ScfgErr a pass produced (a failing reverify or tick ends that pass, so no snapshot;
## the reverify is not retried). A command, wake() or run_once() arriving while a run is in flight
## (including one made from inside a command) only marks `_again`: exactly ONE follow-up pass runs
## when the current one ends, and a joining run_once() resolves when the whole chain is done with
## the chain's first failure. The in-flight flag is set before the first await, so a command that
## calls these synchronously can never start an overlapping run. A command must not AWAIT run_once()
## (the chain waits for the command: deadlock); call it without await, or just use wake().
##
## A command submitted while the commands run joins them (the queue is drained until empty, as in
## Python); one submitted during the tick is executed by the follow-up run, never inside the tick
## (but the tick yields to it: see COMMANDS COME FIRST). stop()
## cancels the timer and queued jobs and prevents loop and follow-up runs (a pass already in flight
## finishes); a submit after stop() is cancelled at once, before the first start() it queues.
## An explicit run_once() after stop() still runs one pass.
##
## ABANDONING. Commands run one at a time, so a session start the player moved away from (another
## network, another amount, Cancel) would hold up the start that replaced it until its RPC requests
## ran out. `ScfgJob.abandon()` prevents that: a job that has not started is cancelled, and a
## running one ends with an Abandoned ScfgErr (not a TransportError: no endpoint cools down) at
## once, its request in flight included (ScfgEngine.refuse_in_flight: the transport is told to stop
## it, and its answer is dropped).
##
## COMMANDS COME FIRST. A tick is background work (watching pending payments), and every command is
## the player's (a start they are waiting on, a hint, a restore): a command submitted while
## Engine.tick runs makes the tick yield the same way, its request in flight ending with Abandoned
## and every further one refused while a command waits. The tick keeps the outcomes it already has
## (ScfgEngine.tick stops between or inside a session's step, as a network failure would; the
## interrupted session is stepped again by the next tick), and the command runs next, in the
## follow-up run, whose tick comes after it. Only the tick yields, never the first run's reverify,
## and a yielded tick is no error (on_error never hears of it). Both need an Engine with
## `refuse_requests` (ScfgEngine's); another engine's jobs and ticks just run to their end.
##
## Options: `interval` (Callable -> seconds, default 15.0), `on_error` (Callable taking the ScfgErr
## a timer-driven run failed with), `scheduler` (an object with schedule(callable, seconds) ->
## handle and cancel(handle)) or `tree` (a SceneTree for the default ScfgTreeScheduler, else the
## main loop).
class_name ScfgWorker
extends RefCounted

var engine: Variant
var interval: Callable
var on_error: Callable
var snapshot: Variant
## True between start() and stop().
var running: bool:
	get:
		return _started and not _stopped

var _scheduler: Variant = null
var _tree: Variant = null
var _commands: Array = []  # [{"fn": Callable, "job": ScfgJob}]
var _outcomes: Array = []
var _first := true
var _ticking := false  # Engine.tick runs, refusing its requests while a command waits
var _in_flight := false  # a run (plus its follow-ups) is in progress
var _again := false  # a wake()/submit()/run_once() landed mid-run
var _chain_failure: Variant = null
var _started := false
var _stopped := false
var _timer: Variant = null
var _timer_set := false

## Emitted when a chain of runs ends; joiners of run_once() wait on it.
signal _chain_done


func _init(engine_: Variant, opts: Dictionary = {}) -> void:
	engine = engine_
	interval = opts.get("interval", func() -> float: return 15.0)
	on_error = opts.get("on_error", Callable())
	_scheduler = opts.get("scheduler", null)
	_tree = opts.get("tree", null)
	snapshot = engine.snapshot()  # err-ok: an ScfgErr stays visible in .snapshot (see the header)


## Run `fn(engine)` in the next run, before its tick; returns a Job for the result.
func submit(fn: Callable) -> ScfgJob:
	var job := ScfgJob.new()
	if _stopped:
		job.cancel()
		return job
	_commands.append({"fn": fn, "job": job})
	if _ticking:
		_refuse_in_flight()  # commands come first (see the header)
	wake()
	return job


## Tick as soon as the current run (if any) is over.
func wake() -> void:
	if _in_flight:
		_again = true
	elif running:
		_arm(0.0)


func drain_outcomes() -> Array:
	var out := _outcomes
	_outcomes = []
	return out


## One run (see the header). A coroutine.
## @fallible
func run_once() -> Variant:
	if _in_flight:
		_again = true
		await _chain_done
		return _chain_failure
	_in_flight = true  # before the first await: a re-entrant call from a command joins as a follow-up
	var failure: Variant = null
	while true:
		_again = false
		var r: Variant = await _pass()
		if r is ScfgErr and failure == null:
			failure = r
		if not (_again and not _stopped):
			break
	_in_flight = false
	_again = false
	_chain_failure = failure
	_chain_done.emit()
	return failure


## @fallible
func _pass() -> Variant:
	# Drain until empty, as Python's workers do: a command submitted by a running command runs
	# in this pass, before the tick.
	while not _commands.is_empty():
		var entry: Dictionary = _commands.pop_front()
		var job: ScfgJob = entry["job"]
		if not job._start():
			continue
		var fn: Callable = entry["fn"]
		if not fn.is_valid():
			job._fail(ScfgErr.make("RuntimeError", "command is not a valid Callable"))
			continue
		var refusing: bool = "refuse_requests" in engine
		if refusing:
			engine.refuse_requests = func() -> bool: return job.abandoned  # this job's requests, refused once abandoned
			job._on_abandon = _refuse_in_flight
		var value: Variant = await fn.call(engine)
		if refusing:
			job._on_abandon = Callable()
			engine.refuse_requests = Callable()
		if value is ScfgErr:
			job._fail(value)
		else:
			job._succeed(value)
	if _first:
		_first = false
		var removed: Variant = await engine.reverify()
		if removed is ScfgErr:
			return removed
	var yields: bool = "refuse_requests" in engine
	if yields:
		engine.refuse_requests = func() -> bool: return not _commands.is_empty()  # commands come first
		_ticking = true
	var outcomes: Variant = await engine.tick()
	if outcomes is ScfgErr and (outcomes as ScfgErr).is_a("Abandoned"):
		outcomes = []  # an engine whose tick does not keep its outcomes when it yields: no error
	if yields:
		_ticking = false
		engine.refuse_requests = Callable()
	if outcomes is ScfgErr:
		return outcomes
	for o: Variant in outcomes:
		if not (o is Dictionary):
			continue
		if o.get("status") != "waiting" or ScfgPy.py_truthy(o.get("error")):
			_outcomes.append(o)
	var snap: Variant = engine.snapshot()
	if snap is ScfgErr:
		return snap
	snapshot = snap
	return null


## The running job was abandoned, or a command came in during the tick: end the request in flight.
func _refuse_in_flight() -> void:
	if engine.has_method("refuse_in_flight"):
		engine.refuse_in_flight()


func start() -> void:
	if running:
		return
	_started = true
	_stopped = false
	_arm(0.0)


## Cancels the timer and queued jobs; no further loop or follow-up runs start.
func stop() -> void:
	_stopped = true
	_started = false
	_disarm()
	var queued := _commands
	_commands = []
	for entry: Dictionary in queued:
		(entry["job"] as ScfgJob).cancel()


func _sched() -> Variant:
	if _scheduler == null:
		_scheduler = ScfgTreeScheduler.new(_tree as SceneTree)
	return _scheduler


func _disarm() -> void:
	if _timer_set:
		_timer_set = false
		var handle: Variant = _timer
		_timer = null
		_sched().cancel(handle)


func _arm(seconds: float) -> void:
	_disarm()
	_timer_set = true
	_timer = _sched().schedule(_fire, maxf(0.0, seconds))


func _fire() -> void:
	_timer_set = false
	_timer = null
	if not running:
		return
	var failure: Variant = await run_once()
	if failure is ScfgErr and on_error.is_valid():
		on_error.call(failure)
	_rearm()


func _rearm() -> void:
	if not running or _timer_set:
		return
	_arm(0.0 if not _commands.is_empty() else float(interval.call()))
