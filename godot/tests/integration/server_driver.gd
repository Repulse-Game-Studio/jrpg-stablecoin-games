## JSON-lines driver for the shared server security suite (tests/server_security/): runs one
## ScfgCheckoutServer (addons/scfg/local_server.gd) at a time in a headless Godot, with fake
## callbacks, and pumps it every frame as the glue does.
##
##   godot --headless --path godot --script res://tests/integration/server_driver.gd
##
## Protocol (tests/server_security/server_handles.py): one JSON command per stdin line, one reply
## line per command, `SCFG-REPLY {"ok": <result>}` or `SCFG-REPLY {"error": "<why>"}`:
##   start  {config}  stop the previous server, start a new one; ok = {url, port, secret}
##          config = {assets: {name: base64}, session_info, status,
##                    logo: null | "broken" | {b64, ctype},
##                    build: null | {mode: "ok", value, delay_s?} | {mode: "fail"} | {mode: "raise"}
##                           | {mode: "never"},
##                    build_timeout_s?}
##   events        ok = {calls: [[name, args]...], jobs: [{done, cancelled}...], running}: every
##                 callback the server made (on_tx, build_tx) and the state of each build job
##   stop          stop the server; ok = {running}
## End of stdin quits.
##
## Unlike engine_driver.gd, the main loop must keep turning between commands (the server is
## pumped every frame), so stdin is read on a Thread: OS.read_string_from_stdin blocks. Godot 4.3
## returns a line in pieces of at most 1023 bytes, newline kept; 4.4+ takes a buffer size and
## strips the newline (see engine_driver.gd). Commands are ASCII JSON.
extends SceneTree

const REPLY_PREFIX := "SCFG-REPLY "
const STDIN_BUFFER := 1 << 20

var _reader: Thread
var _mutex := Mutex.new()
var _inbox: Array = []  # command lines; null marks the end of stdin
var _server: ScfgCheckoutServer = null
var _calls: Array = []
var _jobs: Array[ScfgJob] = []
var _delayed: Array = []  # [job, value, due (ticks msec)] for build mode "ok" with delay_s
var _config: Dictionary = {}
var _quitting := false


func _initialize() -> void:
	Engine.max_fps = 500  # headless would otherwise spin a core
	_reader = Thread.new()
	_reader.start(_read_stdin)


func _process(_delta: float) -> bool:
	if _quitting:
		return true
	_mutex.lock()
	var lines := _inbox.duplicate()
	_inbox.clear()
	_mutex.unlock()
	for line: Variant in lines:
		if line == null:
			_finish()
			return true
		print(REPLY_PREFIX + _reply_for(line))
	var now := Time.get_ticks_msec()
	for d: Array in _delayed.duplicate():
		if now >= d[2]:
			_delayed.erase(d)
			var job: ScfgJob = d[0]
			if not job.done():
				job._succeed(d[1])
	if _server != null:
		_server.pump()
	return false


func _finish() -> void:
	_quitting = true
	if _server != null:
		_server.stop()
	_reader.wait_to_finish()


func _read_stdin() -> void:
	while true:
		var line := _read_line()
		_mutex.lock()
		_inbox.append(null if line == "" else line)
		_mutex.unlock()
		if line == "":
			return


## One command line without its newline, or "" at the end of stdin.
func _read_line() -> String:
	if Engine.get_version_info()["hex"] >= 0x040400:
		return str(OS.call("read_string_from_stdin", STDIN_BUFFER)).strip_edges()
	var buf := ""
	while not buf.ends_with("\n"):
		var piece := OS.read_string_from_stdin()
		if piece == "":
			break
		buf += piece
	return buf.strip_edges()


func _reply_for(line: String) -> String:
	var json := JSON.new()
	var cmd: Variant = json.data if json.parse(line) == OK else null
	if not cmd is Dictionary:
		return _error("not a JSON object: %s" % line.left(200))
	cmd = ScfgPy.from_json(cmd)
	match cmd.get("op"):
		"start":
			return _start(cmd.get("config", {}))
		"events":
			var jobs := _jobs.map(func(j: ScfgJob) -> Dictionary:
				return {"done": j.done(), "cancelled": j.cancelled()})
			return _ok({"calls": _calls, "jobs": jobs,
				"running": _server != null and _server.running})
		"stop":
			if _server != null:
				_server.stop()
			return _ok({"running": _server != null and _server.running})
	return _error("unknown op %s" % JSON.stringify(cmd.get("op")))


func _start(config: Variant) -> String:
	if not config is Dictionary:
		return _error("config must be an object")
	if _server != null:
		_server.stop()
	_config = config
	_calls = []
	_jobs = []
	_delayed = []
	var opts := {
		"load_asset": _load_asset,
		"session_info": config.get("session_info", {}),
		"on_tx": func(tx: Variant, sender: Variant = null) -> void: _calls.append(["on_tx", [tx, sender]]),
		"get_status": func() -> Variant: return _config.get("status", {}),
	}
	var logo: Variant = config.get("logo")
	if logo is String and logo == "broken":
		opts["load_logo"] = func() -> Variant: return null
	elif logo is Dictionary:
		opts["load_logo"] = func() -> Variant:
			return [Marshalls.base64_to_raw(logo["b64"]), logo["ctype"]]
	if config.get("build") is Dictionary:
		opts["build_tx"] = _build_tx
	_server = ScfgCheckoutServer.new(opts)
	if config.has("build_timeout_s"):
		_server.build_timeout_s = float(config["build_timeout_s"])
	var started: Variant = _server.start()
	if started is ScfgErr:
		return _error("start failed: %s" % (started as ScfgErr).message)
	return _ok({"url": _server.url, "port": _server.port, "secret": _server.secret})


func _load_asset(name: String) -> Variant:
	var assets: Dictionary = _config.get("assets", {})
	if not assets.has(name):
		return null
	return Marshalls.base64_to_raw(assets[name])


func _build_tx(account: Variant) -> Variant:
	_calls.append(["build_tx", [account]])
	var build: Dictionary = _config["build"]
	if build.get("mode") == "raise":
		return ScfgErr.make("RuntimeError", "submit failed")
	var job := ScfgJob.new()
	_jobs.append(job)
	match build.get("mode"):
		"fail":
			job._fail(ScfgErr.make("RuntimeError", "timed out"))
		"ok":
			var delay: float = float(build.get("delay_s", 0))
			if delay > 0:
				_delayed.append([job, build.get("value"), Time.get_ticks_msec() + int(delay * 1000)])
			else:
				job._succeed(build.get("value"))
	return job


func _ok(result: Variant) -> String:
	return JSON.stringify({"ok": result}, "", false)


func _error(why: String) -> String:
	return JSON.stringify({"error": why}, "", false)
