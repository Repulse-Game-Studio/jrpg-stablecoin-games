## JSON-lines driver: runs the GDScript Engine in a headless Godot for the Python integration
## tests (tests/integration/gd_driver.py), so the same Anvil / solana-test-validator fixtures can
## exercise the Python, TypeScript and GDScript cores.
##
##   godot --headless --path godot --script res://tests/integration/engine_driver.gd
##
## Protocol: the TypeScript driver's (rpgmaker/test/integration/engine_driver.ts). One JSON
## command per stdin line, one reply per command, in order:
##   {"ok": <result>}  or  {"error": {"type", "code", "message"}}
## Commands:
##   init     {override, config, custom_chains?, items}  patch ScfgChains.MAINNET/TESTNET[key],
##            build the Engine on an ScfgHttpTransport
##            override = {network: "mainnet"|"testnet", key, rpc: [url], confirmations?, tokens?: {name: address}}
##   start_session {kind, chain, item?, amount?, token?, title?}   report_hint {sid, tx}
##   tick   reverify   restore_lookup {tx}   restore_apply {candidates, choices}
##   build_solana_payment {sid, account}   snapshot
## Test-only helpers for the receipt tampering the Python tests do on `engine.receipts`:
##   set_receipt {key, receipt}   clear_receipts
## Results are encoded exactly like the Tier 2 `returns` / `snapshot` (tests/tier2.gd).
## An error's `type` is the ScfgErr kind (DriverError for the driver's own refusals), its `code`
## the ConfigError/RestoreError string or RpcError int, else null.
##
## Godot specifics:
## - Godot writes its own banner to stdout, so each reply is one line starting with REPLY_PREFIX;
##   gd_driver.py ignores every other stdout line. Diagnostics go to stderr.
## - Commands are read with OS.read_string_from_stdin, which blocks the main loop between commands
##   (nothing is in flight then) and lets it run while a command awaits HTTP. Godot 4.3 returns a
##   line in pieces of at most 1023 bytes, newline kept; 4.4+ returns at most buffer_size - 1
##   bytes with the newline stripped, so there one read of STDIN_BUFFER takes a whole line (the
##   Python side keeps lines below it). An empty read is the end of stdin (a command line is never
##   empty), which quits.
## - Numbers: a command is read as Python's json.load reads it (ScfgPy.from_json: integral numbers
##   are ints). restore_apply's `choices` keys arrive as strings and become ints (ScfgRestore keys
##   choices by candidate index).
## - The clock is whole seconds: the Tier 2 encoders refuse a fractional timestamp.
extends SceneTree

const Tier2 := preload("res://tests/tier2.gd")

const REPLY_PREFIX := "SCFG-REPLY "
## Godot 4.4+ stdin read size (bytes, including fgets' terminator): the longest command line + 1.
const STDIN_BUFFER := 1 << 20
const _VOID_OPS: Array[String] = ["init", "report_hint", "set_receipt", "clear_receipts"]

var _host: Node
var _engine: ScfgEngine = null


func _initialize() -> void:
	_serve()  # a coroutine: the main loop keeps turning while a command awaits


func _serve() -> void:
	await process_frame  # the root enters the tree only after _initialize
	_host = Node.new()
	_host.name = "ScfgDriverHost"
	root.add_child(_host)
	while true:
		var line := _read_line()
		if line == "":
			break
		var reply: Variant = await _reply_for(line)
		if not reply is String:  # _reply_for itself aborted on a script error
			reply = _error_json("DriverError", null, "the driver aborted on this command (see stderr)")
		print(REPLY_PREFIX + reply)
	quit(0)


## One command line without its newline, or "" at the end of stdin.
func _read_line() -> String:
	if Engine.get_version_info()["hex"] >= 0x040400:
		# (a dynamic call: 4.3's read_string_from_stdin takes no argument)
		return str(OS.call("read_string_from_stdin", STDIN_BUFFER)).strip_edges()
	var buf := ""
	while not buf.ends_with("\n"):
		var piece := OS.read_string_from_stdin()
		if piece == "":
			break  # end of stdin (a partial last line is still answered)
		buf += piece
	return buf.strip_edges()


func _reply_for(line: String) -> String:
	var json := JSON.new()  # (not JSON.parse_string, which push_errors on bad input)
	var parsed: Variant = json.data if json.parse(line) == OK else null
	if not parsed is Dictionary:
		return _error_json("DriverError", null, "not a JSON object: %s" % line.left(200))
	var cmd: Dictionary = ScfgPy.from_json(parsed)
	if not cmd.get("op") is String:
		return _error_json("DriverError", null, "a command needs a string op")
	var op: String = cmd["op"]
	var result: Variant = await _run(op, cmd)
	if result is ScfgErr:
		var e: ScfgErr = result
		var code: Variant = e.code if (e.code is String or e.code is int) else null
		return _error_json(e.kind, code, e.message)
	if result == null and not op in _VOID_OPS:
		return _error_json("DriverError", null,
			"%s returned null (aborted by a script error, or a coroutine not awaited?)" % op)
	return JSON.stringify({"ok": result}, "", false)  # keys in the Engine's order, as Python's


func _error_json(type: String, code: Variant, message: String) -> String:
	return JSON.stringify({"error": {"type": type, "code": code, "message": message}}, "", false)


## The driver's own refusal (a bad command). Not a core kind, so it is built without
## ScfgErr.make, which reports kinds outside ScfgErr.BASES; it is only ever serialized.
static func _driver_error(message: String) -> ScfgErr:
	var e := ScfgErr.new()
	e.kind = "DriverError"
	e.message = message
	return e


## The op's encoded result, or an ScfgErr.
## @fallible
func _run(op: String, cmd: Dictionary) -> Variant:
	if op == "init":
		return _init_engine(cmd)
	if _engine == null:
		return _driver_error("init first")
	var e := _engine
	match op:
		"start_session":
			var opts := cmd.duplicate()
			for k: String in ["op", "kind", "chain"]:
				opts.erase(k)
			return _encoded(op, await e.start_session(cmd.get("kind"), cmd.get("chain"), opts))
		"report_hint":
			return _encoded(op, e.report_hint(cmd.get("sid"), cmd.get("tx")))
		"tick":
			return _encoded(op, await e.tick())
		"reverify":
			return _encoded(op, await e.reverify())
		"restore_lookup":
			return _encoded(op, await e.restore_lookup(cmd.get("tx")))
		"restore_apply":
			return _restore_apply(e, cmd)
		"build_solana_payment":
			return _encoded(op, await e.build_solana_payment(cmd.get("sid"), cmd.get("account")))
		"snapshot":
			return Tier2.snapshot_json(e)
		"set_receipt":
			e.receipts[cmd.get("key")] = cmd.get("receipt")
			return null
		"clear_receipts":
			e.receipts.clear()
			return null
	return _driver_error("unknown op %s" % JSON.stringify(op))


## A void op's null stays null; any other result is Tier 2-encoded (an ScfgErr passes through).
## @fallible
static func _encoded(op: String, result: Variant) -> Variant:
	if result is ScfgErr or result == null:
		return result
	return Tier2.encode_returns(op, result)


## @fallible
func _init_engine(cmd: Dictionary) -> Variant:
	if cmd.get("override") != null:
		var bad: Variant = _apply_override(cmd["override"])
		if bad is ScfgErr:
			return bad
	var setup := {"config": cmd.get("config"), "custom_chains": cmd.get("custom_chains", []),
		"items": cmd.get("items", [])}
	if not (setup["config"] is Dictionary and setup["custom_chains"] is Array and setup["items"] is Array):
		return _driver_error("init needs a config object and custom_chains/items lists")
	var config: Variant = Tier2.build_config_from(setup)
	if config is ScfgErr:
		return config
	var engine: Variant = ScfgEngine.create(config, {
		"transport": ScfgHttpTransport.new(_host),
		"clock": func() -> int: return int(Time.get_unix_time_from_system()),
		"on_error": func(err: Variant) -> void: printerr("engine error: %s" % str(err)),
	})
	if engine is ScfgErr:
		return engine
	_engine = engine
	return null


## Replace registry[key] with a copy carrying the override's rpc list, confirmations and tokens,
## as the Python fixtures' monkeypatch does.
## @fallible
static func _apply_override(o: Variant) -> Variant:
	if not (o is Dictionary and o.get("rpc") is Array):
		return _driver_error("override needs an object with an rpc list")
	var registry: Dictionary = ScfgChains.TESTNET if o.get("network") == "testnet" else ScfgChains.MAINNET
	var key: Variant = o.get("key")
	if not registry.has(key):
		return _driver_error("unknown chain %s" % JSON.stringify(key))
	var next: Dictionary = (registry[key] as Dictionary).duplicate()
	next["rpc"] = (o["rpc"] as Array).duplicate()
	if o.get("confirmations") != null:
		next["confirmations"] = o["confirmations"]
	if o.get("tokens") != null:
		if not o["tokens"] is Dictionary:
			return _driver_error("override tokens must be an object")
		var tokens := {}
		for name: Variant in o["tokens"]:
			var token: Variant = ScfgChains.make_token(o["tokens"][name])
			if token is ScfgErr:
				return token
			tokens[name] = token
		next["tokens"] = tokens
	registry[key] = next
	return null


## restore_apply {candidates, choices}: candidates as restore_lookup returned them, choices keyed
## by candidate index as a string (JSON), turned into the ints ScfgRestore expects.
## @fallible
func _restore_apply(e: ScfgEngine, cmd: Dictionary) -> Variant:
	if not cmd.get("candidates") is Array:
		return _driver_error("restore_apply needs a candidates list")
	var candidates := []
	for d: Variant in cmd["candidates"]:
		var c: Variant = Tier2.candidate_from_json(d)
		if c is ScfgErr:
			return c
		candidates.append(c)
	var choices: Variant = cmd.get("choices")
	if choices != null:
		if not choices is Dictionary:
			return _driver_error("restore_apply choices must be an object or null")
		var keyed := {}
		for k: Variant in choices:
			var i: Variant = ScfgPy.py_int(k)
			if i is ScfgErr:
				return i
			keyed[i] = choices[k]
		choices = keyed
	return _encoded("restore_apply", e.restore_apply(candidates, choices))
