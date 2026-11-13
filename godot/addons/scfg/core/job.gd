## The future a command submitted to the ScfgWorker hands back: the twin of Python's
## concurrent.futures.Future for the UI's per-frame poll. The game loop calls done() every frame
## and reads result() / exception() once it is true; `finished` is for code that would rather wait.
##
## A job ends in one of three states:
##   - succeeded: result() is the command's value, exception() is null;
##   - failed: the command returned an ScfgErr; result() and exception() are that ScfgErr;
##   - cancelled (before it started): result() and exception() are a CancelledError ScfgErr, as
##     Future.exception() raises one in Python, so ported poll() code lands in its failure branch.
## result() of a job that is not done is a RuntimeError ScfgErr and exception() is null.
##
## WAITING. `await job.finished` never resumes on a job that is already done (the signal has
## fired), so code that may meet a finished job awaits `job.wait()` instead: it returns result()
## at once for a done job, else when the job finishes.
class_name ScfgJob
extends RefCounted

## Emitted once, when the job succeeds, fails or is cancelled.
signal finished

enum State { PENDING, RUNNING, OK, FAILED, CANCELLED }

var _state: State = State.PENDING
var _value: Variant = null
var _error: Variant = null


func done() -> bool:
	return _state == State.OK or _state == State.FAILED or _state == State.CANCELLED


func cancelled() -> bool:
	return _state == State.CANCELLED


## The value, or the job's ScfgErr (see the header).
## @fallible
func result() -> Variant:
	match _state:
		State.OK:
			return _value
		State.FAILED, State.CANCELLED:
			return _error
	return ScfgErr.make("RuntimeError", "job is not done")


## null while pending or running and after a success; otherwise the ScfgErr.
## @fallible
func exception() -> Variant:
	return _error if (_state == State.FAILED or _state == State.CANCELLED) else null


## The job's outcome once it is done (see the header): result(). Returns at once when the job is
## already done, otherwise suspends until `finished`. A coroutine.
## @fallible
func wait() -> Variant:
	if not done():
		await finished
	return result()


## Cancels a job that has not started. False once it is running or finished (true if it already
## was cancelled).
func cancel() -> bool:
	if _state == State.CANCELLED:
		return true
	if _state != State.PENDING:
		return false
	_state = State.CANCELLED
	var err := ScfgErr.new()  # built directly: cancel() returns a bool, the error is only stored
	err.kind = "CancelledError"
	err.message = "job cancelled"
	_error = err
	finished.emit()
	return true


## The worker is about to run it: false when it was cancelled first.
func _start() -> bool:
	if _state != State.PENDING:
		return false
	_state = State.RUNNING
	return true


func _succeed(value: Variant) -> void:
	_state = State.OK
	_value = value
	finished.emit()


func _fail(error: Variant) -> void:
	_state = State.FAILED
	_error = error
	finished.emit()
