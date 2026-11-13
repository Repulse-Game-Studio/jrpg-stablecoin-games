## ScfgRestoreController (restore_flow.gd): tests/unit/test_restore_flow.py (8, same names),
## test_checkout_solana.py::test_restore_accepts_signatures, plus the job edges a GDScript job has
## (a cancelled job, a failure that is not a RestoreError, a success that is not a list).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"

static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


## [ctl, engine] (test_restore_flow.py `setup`). `logs` are the raw logs of TX on Arbitrum.
func _setup(logs: Array = [], status: String = "0x1") -> Array:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var chains := {}
	for k: String in ScfgChains.MAINNET:
		chains[k] = F.FakeChain.new()
	if not logs.is_empty() or status == "0x0":
		(chains["arbitrum"] as F.FakeChain).receipts[TX] = {"status": status, "logs": logs}
	var engine := ScfgEngine.new(cfg, {"rpc_factory": func(c: Dictionary) -> Variant: return chains[c["key"]]})
	var sub := F.ImmediateSubmitter.new(engine)
	return [ScfgRestoreController.new({"submit": sub.submit}), engine, sub]


func _payment(value: int) -> Array:
	return [F.transfer_log(USDC_ARB, PAYER, DEV, value, {"block": 5, "tx_hash": TX})]


## test_restore_flow.py `run`.
func _run(ctl: ScfgRestoreController, text: String = TX) -> void:
	ctl.tx_input = text
	ctl.check()
	ctl.poll()
	if ctl.state == "saving":
		ctl.poll()


func _owned(engine: ScfgEngine) -> Variant:
	return engine.snapshot()["owned"]


func test_automatic_restore() -> bool:
	var m := _setup(_payment(4_993_742))
	var ctl: ScfgRestoreController = m[0]
	_run(ctl)
	T.eq(ctl.state, "done")
	var res: Dictionary = ctl.result()
	T.eq(res["status"], "restored")
	T.eq(res["restored"][0]["item"], "chapter2")
	T.eq(_owned(m[1]), ["chapter2"])
	return T.done()


func test_already_restored_is_nothing_found() -> bool:
	var m := _setup(_payment(4_993_742))
	var ctl: ScfgRestoreController = m[0]
	_run(ctl)
	var ctl2 := ScfgRestoreController.new({"submit": ctl.submit})
	_run(ctl2)
	T.eq(ctl2.state, "done")
	T.eq(ctl2.result()["status"], "nothing_found")
	return T.done()


func test_ambiguous_amount_needs_choice() -> bool:
	var m := _setup(_payment(5_000_000))
	var ctl: ScfgRestoreController = m[0]
	_run(ctl)
	T.eq(ctl.state, "choose")
	T.eq(ctl.pending_choices(), [0])
	T.eq(ctl.candidates[0]["choices"], ["chapter2", ScfgRestore.TIP_CHOICE])
	ctl.choose(0, "chapter2")
	ctl.confirm()
	ctl.poll()
	T.eq(ctl.state, "done")
	T.eq(_owned(m[1]), ["chapter2"])
	return T.done()


func test_restore_waits_until_every_choice_is_answered() -> bool:
	var m := _setup(_payment(5_000_000))
	var ctl: ScfgRestoreController = m[0]
	_run(ctl)
	T.eq(ctl.can_confirm, false)
	ctl.confirm()  # e.g. a click that raced the screen: nothing happens
	T.eq(ctl.state, "choose")
	ctl.choose(0, ScfgRestore.TIP_CHOICE)
	T.eq(ctl.can_confirm, true)
	return T.done()


func test_errors_return_to_input() -> bool:
	var ctl: ScfgRestoreController = _setup()[0]
	_run(ctl, "nonsense")
	T.eq(ctl.state, "input")
	T.eq(ctl.error, "invalid_hash")
	_run(ctl)
	T.eq(ctl.state, "input")
	T.eq(ctl.error, "tx_not_found")
	T.ok(ctl.message() != null and ctl.message() != "", "a message")
	return T.done()


func test_reverted() -> bool:
	var ctl: ScfgRestoreController = _setup([], "0x0")[0]
	_run(ctl)
	T.eq(ctl.error, "tx_reverted")
	return T.done()


func test_cancel_result() -> bool:
	var ctl: ScfgRestoreController = _setup()[0]
	T.eq(ctl.result()["status"], "cancelled")
	return T.done()


func test_a_blocked_paste_shows_the_restore_wording() -> bool:
	var ctl: ScfgRestoreController = _setup()[0]
	ctl.error = "paste_blocked"
	T.ok("Type the hash instead" in str(ctl.message()), "the restore wording")
	return T.done()


## test_checkout_solana.py::test_restore_accepts_signatures
func test_restore_accepts_signatures() -> bool:
	var submitted := []
	var ctl := ScfgRestoreController.new({"submit": func(fn: Callable) -> Variant:
		submitted.append(fn)
		return ScfgJob.new()})
	T.eq(ctl.check(F.sig(7)), true)
	T.ok(not submitted.is_empty(), "the lookup was submitted")
	T.eq(ctl.check("nonsense"), false)
	T.eq(ctl.error, "invalid_hash")
	return T.done()


func test_messages_and_result_shapes() -> bool:
	T.eq(ScfgRestoreController.MESSAGES.keys(), ["invalid_hash", "tx_not_found", "tx_reverted", "tx_not_a_payment",
		"network_unavailable", "paste_blocked", "failed"])
	var ctl: ScfgRestoreController = _setup()[0]
	T.eq(ctl.message(), null, "no error, no message")
	ctl.error = "unheard_of"
	T.eq(ctl.message(), null, "an unknown code has no text (Python MESSAGES.get)")
	ctl.error = "tx_reverted"
	var res: Dictionary = ctl.result()
	T.eq([res["status"], res["error"], res["restored"]], ["cancelled", "tx_reverted", []])
	return T.done()


# --- the job edges (no Python twin: a Future raises, a ScfgJob answers) ---------------------

## A controller whose lookup job is the one `make` builds, already polled.
func _with_job(make: Callable) -> ScfgRestoreController:
	var ctl := ScfgRestoreController.new({"submit": func(_fn: Callable) -> Variant: return make.call()})
	ctl.tx_input = TX
	ctl.check()
	ctl.poll()
	return ctl


func test_a_cancelled_job_lands_in_failed() -> bool:
	var ctl := _with_job(func() -> Variant:
		var job := ScfgJob.new()
		job.cancel()
		return job)
	T.eq(ctl.state, "input")
	T.eq(ctl.error, "failed")
	return T.done()


func test_a_failure_that_is_not_a_restore_error_is_failed() -> bool:
	var ctl := _with_job(func() -> Variant:
		var job := ScfgJob.new()
		job._start()
		job._fail(ScfgErr.make("ValueError", "boom"))
		return job)
	T.eq([ctl.state, ctl.error], ["input", "failed"])
	return T.done()


func test_a_restore_error_code_is_the_error() -> bool:
	var ctl := _with_job(func() -> Variant:
		var job := ScfgJob.new()
		job._start()
		job._fail(ScfgRestore.restore_error("network_unavailable"))
		return job)
	T.eq([ctl.state, ctl.error], ["input", "network_unavailable"])
	return T.done()


func test_a_success_that_is_not_a_list_is_failed() -> bool:
	for value: Variant in [null, "x", {"a": 1}, 3, [1]]:
		var v: Variant = value
		var ctl := _with_job(func() -> Variant:
			var job := ScfgJob.new()
			job._start()
			job._succeed(v)
			return job)
		T.eq([ctl.state, ctl.error], ["input", "failed"], "lookup result %s" % [v])
	# the apply phase too
	var m := _setup(_payment(4_993_742))
	var ctl2: ScfgRestoreController = m[0]
	ctl2.tx_input = TX
	ctl2.check()
	ctl2.poll()
	T.eq(ctl2.state, "saving")
	var job := ScfgJob.new()
	job._start()
	job._succeed(null)
	ctl2._future = job
	ctl2.poll()
	T.eq([ctl2.state, ctl2.error], ["input", "failed"], "apply result null")
	return T.done()
