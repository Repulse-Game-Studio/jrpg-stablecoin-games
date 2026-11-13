## Shared test doubles for the RPC tests (the twin of tests/unit/fakes.py's transport half).
## Not a test file: the runner only loads test_*.gd.
extends RefCounted


## A settable clock: `clock.fn` is the Callable an ScfgRpcClient takes as `clock`. The Callable
## reads a shared cell, so it keeps working after the FakeClock itself is released.
class FakeClock:
	extends RefCounted
	var _cell := {"now": 0.0}

	func _init(start: float = 1000.0) -> void:
		_cell["now"] = start

	var now: float:
		get:
			return _cell["now"]

	func advance(seconds: float) -> void:
		_cell["now"] += seconds

	var fn: Callable:
		get:
			var cell := _cell
			return func() -> float: return cell["now"]


## Routes each request to a per-URL handler `func(method: String, params: Array) -> Variant`.
## A handler returns the response (a Dictionary such as ok()/err() build, or anything else) or
## an ScfgErr (TransportError to simulate a network or HTTP failure). Every request is recorded
## in `calls` as [url, method, params], and the whole payload and timeout in `payloads` and
## `timeouts`. With `frames` > 0 each request first waits that many process frames, so the
## client really suspends (a real transport is a coroutine).
class FakeTransport:
	extends ScfgCoio.Transport
	var handlers: Dictionary
	var calls: Array = []
	var payloads: Array = []
	var timeouts: Array = []
	var frames := 0

	func _init(handlers_: Dictionary) -> void:
		handlers = handlers_

	func post_json(url: String, payload: Dictionary, timeout: float) -> Variant:
		calls.append([url, payload["method"], payload["params"]])
		payloads.append(payload.duplicate(true))
		timeouts.append(timeout)
		for i in frames:
			await (Engine.get_main_loop() as SceneTree).process_frame
		if not handlers.has(url):
			# Python's FakeTransport: self.handlers[url] is a KeyError, not a TransportError
			return ScfgErr.make("KeyError", ScfgPy.py_repr(url))
		return (handlers[url] as Callable).call(payload["method"], payload["params"])


static func ok(result: Variant) -> Dictionary:
	return {"jsonrpc": "2.0", "id": 1, "result": result}


static func err(code: Variant, message: Variant) -> Dictionary:
	return {"jsonrpc": "2.0", "id": 1, "error": {"code": code, "message": message}}


## A handler for an endpoint that is down (Python's `down`).
static func down() -> Callable:
	return func(_m: String, _p: Array) -> Variant: return ScfgErr.make("TransportError", "connection refused")


## A constant handler returning `response`.
static func always(response: Variant) -> Callable:
	return func(_m: String, _p: Array) -> Variant: return response


## In-memory stand-in for ScfgRpcClient (the same method names), driven by tests: the twin of
## tests/unit/fakes.py FakeChain.
##   - head: the current block number
##   - hashes: {block_number: block_hash}; a block not listed gets "0x" + its number in 64 hex
##   - logs: raw log Dictionaries get_logs returns (filtered by block range and address)
##   - receipts: {tx_hash: receipt}; an unknown hash answers null
##   - down: when true every method returns an RpcUnavailable
##   - log_range: null unless a test sets it (the watcher reads it like Python's getattr)
##   - frames: > 0 makes every method wait that many process frames first, so the caller really
##     suspends (a real client is a coroutine)
## Every method is recorded in `calls` by its Python name.
class FakeChain:
	extends RefCounted
	var head: int
	var hashes := {}
	var logs: Array = []
	var receipts := {}
	var down := false
	var calls: Array[String] = []
	var last_get_logs: Array = []
	var log_range: Variant = null
	var frames := 0
	var chain_obj: Dictionary = {}
	## When set (an Array[String] shared between fakes), every call is also appended as "tag:name".
	var journal: Variant = null
	var tag := ""

	func _init(head_: int = 1000) -> void:
		head = head_

	## The RpcUnavailable of an outage, or null.
	## @fallible
	func _check(name: String) -> Variant:
		calls.append(name)
		if journal != null:
			(journal as Array).append("%s:%s" % [tag, name])
		for i in frames:
			await (Engine.get_main_loop() as SceneTree).process_frame
		if down:
			return ScfgErr.make("RpcUnavailable", "fake outage")
		return null

	func block_hash(n: int) -> Variant:
		return hashes.get(n, "0x" + ("%x" % n).lpad(64, "0"))

	## @fallible
	func block_number() -> Variant:
		var bad: Variant = await _check("block_number")
		if bad is ScfgErr:
			return bad
		return head

	## @fallible
	func get_block_hash(n: int) -> Variant:
		var bad: Variant = await _check("get_block_hash")
		if bad is ScfgErr:
			return bad
		return block_hash(n) if n <= head else null

	## @fallible
	func get_transaction_receipt(tx_hash: Variant) -> Variant:
		var bad: Variant = await _check("get_transaction_receipt")
		if bad is ScfgErr:
			return bad
		return receipts.get(tx_hash)

	## @fallible
	func get_logs(address: Variant, topics: Variant, from_block: int, to_block: int) -> Variant:
		var bad: Variant = await _check("get_logs")
		if bad is ScfgErr:
			return bad
		last_get_logs = [address, topics, from_block, to_block]
		var out := []
		for lg: Dictionary in logs:
			var b: int = (lg["blockNumber"] as String).hex_to_int()
			if from_block <= b and b <= to_block and (lg["address"] as String).to_lower() == str(address).to_lower():
				out.append(lg)
		return out


## The raw eth_getLogs entry of an ERC-20 Transfer (fakes.py transfer_log). `value` is an int or
## an ScfgBigInt; `opts`: block and tx_hash (required), log_index (0), block_hash.
static func transfer_log(token: String, sender: String, recipient: String, value: Variant, opts: Dictionary) -> Dictionary:
	var block: int = opts["block"]
	var hex: String = (value as ScfgBigInt).to_hex() if value is ScfgBigInt else "%x" % value
	return {
		"address": token.to_lower(),
		"topics": [ScfgEvm.TRANSFER_TOPIC, ScfgEvm.address_topic(sender), ScfgEvm.address_topic(recipient)],
		"data": "0x" + hex.lpad(64, "0"),
		"transactionHash": opts["tx_hash"],
		"logIndex": "0x%x" % opts.get("log_index", 0),
		"blockNumber": "0x%x" % block,
		"blockHash": opts.get("block_hash", "0x" + ("%x" % block).lpad(64, "0")),
	}


## An entropy (Python's `secrets`) that records each draw as "fn(n)" and answers fixed values:
## token_hex(n) is "ab" * n, randbelow(n) is `nonce`, token_bytes(n) is n zero bytes.
class RecordingEntropy:
	extends RefCounted
	var draws: Array[String] = []
	var nonce := 42

	func token_hex(n: int) -> Variant:
		draws.append("token_hex(%d)" % n)
		return "ab".repeat(n)

	func randbelow(n: int) -> Variant:
		draws.append("randbelow(%d)" % n)
		return nonce

	func token_bytes(n: int) -> Variant:
		draws.append("token_bytes(%d)" % n)
		var b := PackedByteArray()
		b.resize(n)
		return b


# --- Solana (tests/unit/fakes.py solana_tx, FakeSolanaChain; test_solana_build.py BuildRpc) ----


static var _sigs := {}


## A distinct 64-byte signature per n: Python's `b58encode(n.to_bytes(64, "big"))` (cached).
static func sig(n: int) -> String:
	if not _sigs.has(n):
		var b := PackedByteArray()
		b.resize(64)
		b.fill(0)
		var v := n
		var i := 63
		while v > 0 and i >= 0:
			b[i] = v & 0xff
			v >>= 8
			i -= 1
		_sigs[n] = ScfgSolanaBase58.b58encode(b)
	return _sigs[n]


## A getTransaction(json) result where `payer` sends `amount` of `mint` to `owner`. accountIndex
## 1 = the payer's token account, 2 = the owner's. `opts`: mint, payer, owner, amount, slot
## (required), err (null), payer_start (10**9).
static func solana_tx(s: String, opts: Dictionary) -> Dictionary:
	return FakeSolanaChain.make_tx(s, opts)


## In-memory stand-in for ScfgSolanaRpc (the same method names), driven by tests: the twin of
## tests/unit/fakes.py FakeSolanaChain.
##   - txs: {signature: getTransaction result}
##   - addresses: {address: [signature info Dictionaries, newest first]}
##   - statuses: {signature: getSignatureStatuses entry or null}
##   - accounts: {address: true} of the existing accounts (account_exists)
##   - first_available: the slot from which get_transaction_any's null is authoritative
##   - down: when true every method returns an RpcUnavailable
##   - frames: > 0 makes every method wait that many process frames first
##   - patched: {Python method name: Callable} replacing that method (Python's monkeypatch); the
##     Callable takes the method's arguments, may return an ScfgErr, and is not recorded in calls
##     (the real one is `real_<name>`)
## Every method is recorded in `calls` by its Python name.
class FakeSolanaChain:
	extends RefCounted
	var slot: int
	var txs := {}
	var addresses := {}
	var statuses := {}
	var accounts := {}
	var first_available := 0
	var down := false
	var frames := 0
	var calls: Array[String] = []
	var patched := {}

	func _init(slot_: int = 1000) -> void:
		slot = slot_

	## fakes.py solana_tx (see the outer `solana_tx`).
	static func make_tx(s: String, opts: Dictionary) -> Dictionary:
		var mint: String = opts["mint"]
		var payer: String = opts["payer"]
		var owner: String = opts["owner"]
		var amount: int = opts["amount"]
		var start: int = opts.get("payer_start", 1_000_000_000)
		var row := func(index: int, who: String, value: int) -> Dictionary:
			return {"accountIndex": index, "mint": mint, "owner": who, "uiTokenAmount": {"amount": str(value), "decimals": 6}}
		return {
			"slot": opts["slot"],
			"transaction": {"signatures": [s], "message": {"accountKeys": [payer, "PAYERATA", "OWNERATA", mint]}},
			"meta": {"err": opts.get("err"),
				"preTokenBalances": [row.call(1, payer, start), row.call(2, owner, 0)],
				"postTokenBalances": [row.call(1, payer, start - amount), row.call(2, owner, amount)]},
		}

	## The RpcUnavailable of an outage, or null.
	## @fallible
	func _check(name: String) -> Variant:
		calls.append(name)
		for i in frames:
			await (Engine.get_main_loop() as SceneTree).process_frame
		if down:
			return ScfgErr.make("RpcUnavailable", "fake outage")
		return null

	## `opts`: mint, payer, owner, amount (required), slot (this chain's), err (null), addresses
	## ([]: each gets the signature as its newest), status ("confirmed").
	func pay(s: String, opts: Dictionary) -> Dictionary:
		var o := opts.duplicate()
		if o.get("slot") == null:
			o["slot"] = slot
		var err: Variant = o.get("err")
		var tx := make_tx(s, o)
		txs[s] = tx
		for a: String in o.get("addresses", []):
			if not addresses.has(a):
				addresses[a] = []
			(addresses[a] as Array).insert(0, {"signature": s, "slot": tx["slot"], "err": err})
		var status: String = o.get("status", "confirmed")
		statuses[s] = {"slot": tx["slot"], "err": err, "confirmationStatus": status,
			"confirmations": null if status == "finalized" else 1}
		return tx

	func finalize(s: String) -> void:
		var st: Dictionary = (statuses[s] as Dictionary).duplicate()
		st["confirmationStatus"] = "finalized"
		st["confirmations"] = null
		statuses[s] = st

	## @fallible
	func get_slot(commitment: String = "confirmed") -> Variant:
		if patched.has("get_slot"):
			return await (patched["get_slot"] as Callable).call(commitment)
		var bad: Variant = await _check("get_slot")
		if bad is ScfgErr:
			return bad
		return slot

	## `opts`: until, before, limit (100), commitment.
	## @fallible
	func get_signatures_for_address(address: Variant, opts: Dictionary = {}) -> Variant:
		if patched.has("get_signatures_for_address"):
			return await (patched["get_signatures_for_address"] as Callable).call(address, opts)
		return await real_get_signatures_for_address(address, opts)

	## @fallible
	func real_get_signatures_for_address(address: Variant, opts: Dictionary = {}) -> Variant:
		var bad: Variant = await _check("get_signatures_for_address")
		if bad is ScfgErr:
			return bad
		var sigs: Array = (addresses.get(address, []) as Array).slice(0)
		var until: Variant = opts.get("until")
		var before: Variant = opts.get("before")
		var at := _index_of(sigs, until)
		if at >= 0:
			sigs = sigs.slice(0, at)
		at = _index_of(sigs, before)
		if at >= 0:
			sigs = sigs.slice(at + 1)
		var out := []
		for s: Dictionary in sigs.slice(0, opts.get("limit", 100)):
			out.append(s.duplicate())
		return out

	static func _index_of(sigs: Array, name: Variant) -> int:
		if name == null:
			return -1
		for i in sigs.size():
			if sigs[i]["signature"] == name:
				return i
		return -1

	## @fallible
	func get_transaction(s: Variant, commitment: String = "confirmed") -> Variant:
		if patched.has("get_transaction"):
			return await (patched["get_transaction"] as Callable).call(s, commitment)
		var bad: Variant = await _check("get_transaction")
		if bad is ScfgErr:
			return bad
		return txs.get(s)

	## [tx or null, authoritative]. `opts`: min_slot, commitment.
	## @fallible
	func get_transaction_any(s: Variant, opts: Dictionary = {}) -> Variant:
		if patched.has("get_transaction_any"):
			return await (patched["get_transaction_any"] as Callable).call(s, opts)
		var bad: Variant = await _check("get_transaction_any")
		if bad is ScfgErr:
			return bad
		if txs.get(s) != null:
			return [txs[s], false]
		var min_slot: Variant = opts.get("min_slot")
		return [null, min_slot != null and first_available <= min_slot]

	## @fallible
	func get_signature_statuses(sigs: Variant) -> Variant:
		if patched.has("get_signature_statuses"):
			return await (patched["get_signature_statuses"] as Callable).call(sigs)
		var bad: Variant = await _check("get_signature_statuses")
		if bad is ScfgErr:
			return bad
		var out := []
		for s: Variant in sigs:
			out.append(statuses.get(s))
		return out

	## @fallible
	func account_exists(address: Variant) -> Variant:
		if patched.has("account_exists"):
			return await (patched["account_exists"] as Callable).call(address)
		var bad: Variant = await _check("account_exists")
		if bad is ScfgErr:
			return bad
		return accounts.has(address)


## test_solana_build.py BuildRpc: a FakeSolanaChain that also answers build_payment's calls (each
## recorded in `calls`). `token_account` holds 7_000_000 units, any other account 0; the SOL
## balance is 2_000_000; the priority fee is `fee`.
class BuildRpc:
	extends FakeSolanaChain
	const BLOCKHASH := "4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn"
	var fee: Variant = 5_000
	var token_account := ""

	## @fallible
	func get_token_balance(account: Variant) -> Variant:
		calls.append("get_token_balance")
		return 7_000_000 if account == token_account else 0

	## @fallible
	func get_balance(_address: Variant) -> Variant:
		calls.append("get_balance")
		return 2_000_000

	## @fallible
	func get_latest_blockhash() -> Variant:
		calls.append("get_latest_blockhash")
		return BLOCKHASH

	## @fallible
	func get_priority_fee(_accounts: Variant) -> Variant:
		calls.append("get_priority_fee")
		return fee


## Worker.submit stand-in (fakes.py immediate_submitter): runs the command at once and returns a
## done ScfgJob. `ImmediateSubmitter.new(engine).submit` is the controller's `submit`. The command
## must not suspend (use fakes with frames = 0): a command that does fails its job, loudly.
class ImmediateSubmitter:
	extends RefCounted
	var engine: Variant

	func _init(engine_: Variant) -> void:
		engine = engine_

	## The controller's `submit`. A lambda, so the Callable keeps this object alive (a bound method
	## Callable only names its object).
	var submit: Callable:
		get:
			return func(fn: Callable) -> ScfgJob: return _submit(fn)

	func _submit(fn: Callable) -> ScfgJob:
		var job := ScfgJob.new()
		job._start()
		var value: Variant = fn.call(engine)
		if value is Object and (value as Object).get_class() == "GDScriptFunctionState":
			job._fail(ScfgErr.make("RuntimeError", "the command suspended; ImmediateSubmitter cannot wait for it"))
		elif value is ScfgErr:
			job._fail(value)
		else:
			job._succeed(value)
		return job


## test_checkout_abandoned_start.py DeferredSubmitter: commands wait in `queue` ([fn, job] pairs)
## until `run()` is awaited.
class DeferredSubmitter:
	extends RefCounted
	var engine: Variant
	var queue: Array = []

	func _init(engine_: Variant) -> void:
		engine = engine_

	var submit: Callable:
		get:
			return func(fn: Callable) -> ScfgJob: return _submit(fn)

	func _submit(fn: Callable) -> ScfgJob:
		var job := ScfgJob.new()
		queue.append([fn, job])
		return job

	func run() -> void:
		while not queue.is_empty():
			var entry: Array = queue.pop_front()
			var job: ScfgJob = entry[1]
			if not job._start():
				continue
			var value: Variant = await (entry[0] as Callable).call(engine)
			if value is ScfgErr:
				job._fail(value)
			else:
				job._succeed(value)


## The localhost page / web overlay a server_factory returns. Every field is public; `pump` and
## `reopen` exist only on PageWithPump, because the controller looks for them by name.
class FakeServer:
	extends RefCounted
	var kw: Dictionary
	var session_info: Dictionary
	var on_tx: Callable
	var get_status: Callable
	var build_tx: Variant
	var started := false
	var stopped := false
	var running := false
	var url := "http://127.0.0.1:1234/c/secret/"

	func _init(kw_: Dictionary) -> void:
		kw = kw_
		session_info = kw_["session_info"]
		on_tx = kw_["on_tx"]
		get_status = kw_["get_status"]
		build_tx = kw_.get("build_tx")

	func start() -> Variant:
		started = true
		running = true
		return url

	func stop() -> void:
		stopped = true
		running = false


class PageWithPump:
	extends FakeServer
	var pumps := 0
	var reopens := 0

	func pump() -> void:
		pumps += 1

	func reopen() -> void:
		reopens += 1


## The page's JavaScript (ScfgWebPage's `js`): records every script it is given in `scripts`.
## `answers` maps the start of a script (such as "scfgBridge.take(") to the values returned for
## the scripts that start with it, oldest first; any other script, and one whose answers have run
## out, returns null, as JavaScriptBridge.eval does for a script with no value.
##
## ScfgWebMirror's two scripts (READ_JS / WRITE_JS called on arguments) are kept apart, in
## `storage_scripts`, and run on a fake localStorage: `local_storage` (key -> text; null when the
## page has none). Their arguments are read as a JSON array, so a script whose arguments are not
## JSON literals gets no answer. `storage_throws`: the error name every localStorage call throws
## ("" for none: "SecurityError" is blocked site data); `write_throws`: only setItem throws
## ("QuotaExceededError" is a full storage).
class FakeJs:
	extends RefCounted
	var scripts: Array[String] = []
	var answers: Dictionary = {}
	var local_storage: Variant = {}
	var storage_throws := ""
	var write_throws := ""
	var storage_scripts: Array[String] = []

	func eval(code: String) -> Variant:
		for op: String in [ScfgWebMirror.READ_JS, ScfgWebMirror.WRITE_JS]:
			if code.begins_with(op + "(") and code.ends_with(")"):
				storage_scripts.append(code)
				return _storage(op == ScfgWebMirror.WRITE_JS, code.substr(op.length() + 1, code.length() - op.length() - 2))
		scripts.append(code)
		for prefix: String in answers:
			if code.begins_with(prefix) and not (answers[prefix] as Array).is_empty():
				return (answers[prefix] as Array).pop_front()
		return null

	func _storage(write: bool, arguments: String) -> Variant:
		var json := JSON.new()
		if json.parse("[" + arguments + "]") != OK or not json.data is Array:
			return null  # (the page would throw a SyntaxError)
		var args: Array = json.data
		if args.size() != (2 if write else 1) or not args.all(func(a: Variant) -> bool: return a is String):
			return null
		if local_storage == null:
			return JSON.stringify({"error": "no localStorage"})
		if storage_throws != "":
			return JSON.stringify({"error": storage_throws + ": the operation is insecure."})
		if write:
			if write_throws != "":
				return JSON.stringify({"error": write_throws + ": the quota has been exceeded."})
			(local_storage as Dictionary)[args[0]] = args[1]
			return "{}"
		return JSON.stringify({"value": (local_storage as Dictionary).get(args[0])})

	## The scripts that start with `prefix`.
	func scripts_for(prefix: String) -> Array[String]:
		var out: Array[String] = []
		for s in scripts:
			if s.begins_with(prefix):
				out.append(s)
		return out


## A tiny HTTP/1.1 server on 127.0.0.1 at a free port, polled in its _process (add it under the
## tree root, then start()): it answers each request by its path; `/slow` never answers. Records
## every request. Used by test_http_transport.gd and test_scfg_api.gd.
class Responder:
	extends Node
	const OK_BODY := "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"0x10\"}"
	const RPC_ERROR_BODY := "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,\"message\":\"block range too large\"}}"
	var server := TCPServer.new()
	var port := 0
	var conns: Array[Dictionary] = []
	var requests: Array[Dictionary] = []
	var host: Node  # the transport's host, to count its children while a request is in flight

	func start() -> bool:
		if server.listen(0, "127.0.0.1") != OK:
			return false
		port = server.get_local_port()
		return true

	func url(path: String) -> String:
		return "http://127.0.0.1:%d%s" % [port, path]

	func _process(_delta: float) -> void:
		while server.is_connection_available():
			conns.append({"peer": server.take_connection(), "buf": PackedByteArray(), "answered": -1})
		for c in conns:
			var peer: StreamPeerTCP = c["peer"]
			peer.poll()
			if c["answered"] == -2:
				continue  # /slow
			if c["answered"] >= 0:
				c["answered"] += 1
				if c["answered"] == 10:
					peer.disconnect_from_host()
				continue
			if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
				continue
			var n := peer.get_available_bytes()
			if n > 0:
				var got: Array = peer.get_data(n)
				if got[0] == OK:
					c["buf"] = (c["buf"] as PackedByteArray) + (got[1] as PackedByteArray)  # a value type: reassign
			var req := _parse(c["buf"])
			if req.is_empty():
				continue
			req["host_children"] = host.get_children().map(func(ch: Node) -> String: return ch.get_class()) if host else []
			requests.append(req)
			var answer := _answer(req["path"])
			if answer.is_empty():
				c["answered"] = -2  # /slow: never answers
				continue
			var body: PackedByteArray = answer[1]
			var head := "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % [
				answer[0], body.size()]
			if req["path"] == "/ok-chunked":  # no Content-Length: the body arrives in two chunks
				head = "HTTP/1.1 200 X\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
				var cut := body.size() / 2
				body = ("%x\r\n" % cut).to_ascii_buffer() + body.slice(0, cut) + ("\r\n%x\r\n" % (body.size() - cut)).to_ascii_buffer() \
					+ body.slice(cut) + "\r\n0\r\n\r\n".to_ascii_buffer()
			peer.put_data(head.to_ascii_buffer() + body)
			c["answered"] = 0

	## {method, path, headers (lowercase names), body} once the whole request arrived, else {}.
	static func _parse(buf: PackedByteArray) -> Dictionary:
		var text := buf.get_string_from_ascii()
		var end := text.find("\r\n\r\n")
		if end < 0:
			return {}
		var lines := text.substr(0, end).split("\r\n")
		var first := lines[0].split(" ")
		var headers := {}
		for i in range(1, lines.size()):
			var colon := lines[i].find(":")
			headers[lines[i].substr(0, colon).to_lower()] = lines[i].substr(colon + 1).strip_edges()
		var length := int(headers.get("content-length", "0"))
		if buf.size() < end + 4 + length:
			return {}
		return {"method": first[0], "path": first[1], "headers": headers,
			"body": buf.slice(end + 4, end + 4 + length).get_string_from_utf8()}

	static func _answer(path: String) -> Array:
		match path:
			"/ok", "/ok-chunked":
				return [200, OK_BODY.to_utf8_buffer()]
			"/rate":
				return [429, "Too Many Requests".to_utf8_buffer()]
			"/rpc-error":
				return [400, RPC_ERROR_BODY.to_utf8_buffer()]
			"/falsy-error":
				return [500, "{\"error\": null, \"id\": 1}".to_utf8_buffer()]
			"/empty-error":
				return [400, "{\"error\": {}}".to_utf8_buffer()]
			"/teapot-list":
				return [418, "[1]".to_utf8_buffer()]
			"/not-json":
				return [200, "<html>oops</html>".to_utf8_buffer()]
			"/bad-utf8":
				return [200, PackedByteArray([0x7b, 0xff, 0x7d])]
			"/bom":
				return [200, PackedByteArray([0xef, 0xbb, 0xbf]) + "{}".to_utf8_buffer()]
			"/nul":
				return [200, "{}".to_utf8_buffer() + PackedByteArray([0])]
			"/null":
				return [200, "null".to_utf8_buffer()]
			"/created-error":
				return [201, RPC_ERROR_BODY.to_utf8_buffer()]
			"/long-int":
				return [200, ("{\"result\": " + "9".repeat(4301) + "}").to_utf8_buffer()]
			"/long-int-error":
				return [400, ("{\"error\": {\"code\": " + "9".repeat(4301) + ", \"message\": \"x\"}}").to_utf8_buffer()]
		return []
