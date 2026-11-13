## ScfgHttpTransport (http_transport.gd) against a tiny HTTP/1.1 responder that runs inside the
## test, on a TCPServer bound to 127.0.0.1 at a free port (fakes.gd's Responder).
##
## Getting a Node in the tree: the runner (run.gd) is a SceneTree script, so
## `Engine.get_main_loop()` is that SceneTree and its `root` Viewport is inside the tree and
## processed every frame. Each test adds a fresh host Node (the transport's HTTPRequest children
## go under it) and the responder Node (polled in its _process) under `root`, and frees both at
## the end. Nothing else in the suite needs the tree.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const _RPC_ERROR_BODY := F.Responder.RPC_ERROR_BODY


## [host, responder, transport]. A coroutine: the runner starts in SceneTree._initialize, before
## the root enters the tree, so a test that runs before any other test awaited a frame waits for
## one here.
func _setup() -> Array:
	var tree := Engine.get_main_loop() as SceneTree
	var root := tree.root
	if not root.is_inside_tree():
		await tree.process_frame
	var host := Node.new()
	root.add_child(host)
	var responder := F.Responder.new()
	responder.host = host
	root.add_child(responder)
	T.ok(responder.start(), "the responder listens")
	return [host, responder, ScfgHttpTransport.new(host)]


func _teardown(env: Array) -> void:
	var responder: F.Responder = env[1]
	for c in responder.conns:
		(c["peer"] as StreamPeerTCP).disconnect_from_host()
	responder.server.stop()
	responder.queue_free()
	(env[0] as Node).queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame


func _payload() -> Dictionary:
	return {"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber", "params": [{"limit": 100, "v": 0}, "0x10", false]}


func _status_of(v: Variant) -> Variant:
	return (v as ScfgErr).status if v is ScfgErr else "<not an error: %s>" % [v]


func test_200_json_is_the_parsed_body() -> bool:
	var env := await _setup()
	var host: Node = env[0]
	var responder: F.Responder = env[1]
	var tr: ScfgHttpTransport = env[2]
	var got: Variant = await tr.post_json(responder.url("/ok"), _payload(), 5.0)
	T.json_eq(got, {"jsonrpc": "2.0", "id": 1, "result": "0x10"}, "the body")
	T.eq(responder.requests.size(), 1, "one request")
	if responder.requests.size() == 1:
		var req: Dictionary = responder.requests[0]
		T.eq(req["method"], "POST", "POST")
		T.eq(req["path"], "/ok", "path")
		T.eq((req["headers"] as Dictionary).get("content-type"), "application/json", "Content-Type")
		T.eq((req["headers"] as Dictionary).get("user-agent"), "scfg/0.1", "User-Agent (desktop)")
		T.json_eq(JSON.parse_string(req["body"]), _payload(), "the body is the payload")
		var body: String = req["body"]
		T.ok(body.contains("\"id\":1") and body.contains("\"limit\":100")
			and preload("res://tests/test_rpc_client.gd").fractional_number(body) == "", "ints are sent as ints: %s" % body)
		T.eq(req["host_children"], ["HTTPRequest"], "one HTTPRequest child while in flight")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(host.get_child_count(), 0, "the HTTPRequest child is freed afterwards")
	await _teardown(env)
	return T.done()


func test_429_is_a_transport_error_with_its_status() -> bool:
	var env := await _setup()
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json((env[1] as F.Responder).url("/rate"), _payload(), 5.0)
	T.is_err(got, "TransportError", "429")
	T.eq(_status_of(got), 429, "status")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "HTTP 429", "message")
	await _teardown(env)
	return T.done()


func test_4xx_with_a_jsonrpc_error_body_is_the_body() -> bool:
	var env := await _setup()
	var tr: ScfgHttpTransport = env[2]
	var responder: F.Responder = env[1]
	var got: Variant = await tr.post_json(responder.url("/rpc-error"), _payload(), 5.0)
	T.json_eq(got, JSON.parse_string(_RPC_ERROR_BODY), "passed through")
	got = await tr.post_json(responder.url("/created-error"), _payload(), 5.0)
	T.json_eq(got, JSON.parse_string(_RPC_ERROR_BODY), "a 2xx JSON body as it is")
	# an error status whose body has no truthy "error" is an HTTP error
	for row: Array in [["/falsy-error", 500], ["/empty-error", 400], ["/teapot-list", 418]]:
		got = await tr.post_json(responder.url(row[0]), _payload(), 5.0)
		T.is_err(got, "TransportError", row[0])
		T.eq(_status_of(got), row[1], "%s status" % row[0])
	await _teardown(env)
	return T.done()


func test_non_json_body_is_invalid_json_response() -> bool:
	var env := await _setup()
	var tr: ScfgHttpTransport = env[2]
	var responder: F.Responder = env[1]
	for path: String in ["/not-json", "/bad-utf8", "/bom", "/nul"]:
		var got: Variant = await tr.post_json(responder.url(path), _payload(), 5.0)
		T.is_err(got, "TransportError", path)
		T.eq((got as ScfgErr).message if got is ScfgErr else "", "invalid JSON response", "%s message" % path)
		T.eq(_status_of(got), null, "%s has no status" % path)
	T.eq(await tr.post_json(responder.url("/null"), _payload(), 5.0), null, "a JSON null body is null")
	await _teardown(env)
	return T.done()


# tests/unit/test_urllib_transport.py::test_an_overlong_json_integer_is_invalid_json
func test_an_overlong_json_integer_is_invalid_json() -> bool:
	var env := await _setup()
	var tr: ScfgHttpTransport = env[2]
	var responder: F.Responder = env[1]
	var got: Variant = await tr.post_json(responder.url("/long-int"), _payload(), 5.0)
	T.is_err(got, "TransportError", "/long-int")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "invalid JSON response", "/long-int message")
	got = await tr.post_json(responder.url("/long-int-error"), _payload(), 5.0)
	T.is_err(got, "TransportError", "/long-int-error")
	T.eq(_status_of(got), 400, "/long-int-error status")
	await _teardown(env)
	return T.done()


## Godot only (urllib reads any body): a body over `body_size_limit` is dropped, a TransportError
## without a status, so ScfgRpcClient cools the endpoint down and tries the next one.
func test_a_response_over_the_size_limit_is_a_transport_error() -> bool:
	var env := await _setup()
	var host: Node = env[0]
	var tr: ScfgHttpTransport = env[2]
	var responder: F.Responder = env[1]
	T.eq(tr.body_size_limit, 16 * 1024 * 1024, "16 MiB by default")
	T.eq(tr.body_size_limit, ScfgHttpTransport.MAX_BODY_BYTES, "MAX_BODY_BYTES")
	var size := F.Responder.OK_BODY.to_utf8_buffer().size()
	tr.body_size_limit = size - 1
	var got: Variant = await tr.post_json(responder.url("/ok"), _payload(), 5.0)
	T.is_err(got, "TransportError", "one byte over")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "response too large", "its message")
	T.eq(_status_of(got), null, "no status")
	# without a Content-Length the body is counted as it arrives
	got = await tr.post_json(responder.url("/ok-chunked"), _payload(), 5.0)
	T.is_err(got, "TransportError", "one byte over, no Content-Length")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "response too large", "its message (no Content-Length)")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(host.get_child_count(), 0, "the HTTPRequest is freed")
	tr.body_size_limit = size
	T.json_eq(await tr.post_json(responder.url("/ok"), _payload(), 5.0),
		{"jsonrpc": "2.0", "id": 1, "result": "0x10"}, "a body of exactly the limit is read")
	T.json_eq(await tr.post_json(responder.url("/ok-chunked"), _payload(), 5.0),
		{"jsonrpc": "2.0", "id": 1, "result": "0x10"}, "also without a Content-Length")
	await _teardown(env)
	return T.done()


## Waits until a frame begins whose time since the last one is all its own. HTTPRequest's timeout
## counts the process delta of the frames it runs in, and the first frame after a long one (a slow
## test before this one, a busy machine) carries all of that frame's time: a timeout measured from
## it ends early. The second frame from now has a fresh delta.
static func _fresh_frame() -> void:
	for i in 2:
		await (Engine.get_main_loop() as SceneTree).process_frame


func test_timeout_is_a_transport_error() -> bool:
	var env := await _setup()
	var host: Node = env[0]
	await _fresh_frame()
	var start := Time.get_ticks_msec()
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json((env[1] as F.Responder).url("/slow"), _payload(), 0.5)
	var took := (Time.get_ticks_msec() - start) / 1000.0
	T.is_err(got, "TransportError", "timed out")
	T.eq(_status_of(got), null, "no status")
	T.ok(took >= 0.4 and took < 5.0, "honours the 0.5 s timeout (%.2f s)" % took)
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(host.get_child_count(), 0, "freed after a timeout too")
	await _teardown(env)
	return T.done()


func test_connection_refused_is_a_transport_error() -> bool:
	var env := await _setup()
	var responder: F.Responder = env[1]
	var url := responder.url("/ok")
	responder.server.stop()
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json(url, _payload(), 5.0)
	T.is_err(got, "TransportError", "refused")
	T.eq(_status_of(got), null, "no status")
	await _teardown(env)
	return T.done()


func test_host_outside_the_tree_is_host_gone() -> bool:
	# not an endpoint failure (no cooldown), and no add_child on a host outside the tree
	var orphan := Node.new()
	var got: Variant = await ScfgHttpTransport.new(orphan).post_json("http://127.0.0.1:9/", {}, 1.0)
	T.is_err(got, "HostGone", "no tree")
	T.ok(not (got is ScfgErr and (got as ScfgErr).is_a("TransportError")), "not a TransportError")
	T.eq(orphan.get_child_count(), 0, "nothing was added to it")
	orphan.free()
	var none: Variant = await ScfgHttpTransport.new(null).post_json("http://127.0.0.1:9/", {}, 1.0)
	T.is_err(none, "HostGone", "no host at all")
	return T.done()


## Starts `client.block_number()` without waiting for it; `box["got"]` holds its result once
## `box["done"]` is true.
func _start_block_number(client: ScfgRpcClient, box: Dictionary) -> void:
	var run := func() -> void:
		box["got"] = await client.block_number()
		box["done"] = true
	run.call()


func _wait_for(responder: F.Responder, box: Dictionary, what: String) -> void:
	var tree := Engine.get_main_loop() as SceneTree
	for i in 300:
		if (what == "request" and responder.requests.size() > 0) or (what == "done" and box.get("done", false)):
			return
		await tree.process_frame


func test_host_freed_mid_request_is_host_gone_not_a_cooldown() -> bool:
	# Resuming inside the exit-tree notification would fail over with add_child on the dying
	# host (engine errors) and put the healthy endpoints in cooldown.
	var env := await _setup()
	var host: Node = env[0]
	var responder: F.Responder = env[1]
	var client := ScfgRpcClient.new([responder.url("/slow"), responder.url("/ok")], {"transport": env[2]})
	var box := {}
	_start_block_number(client, box)
	await _wait_for(responder, box, "request")
	T.eq(responder.requests.size(), 1, "the request is in flight")
	responder.host = null
	host.free()
	T.eq(box.get("done", false), false, "the caller does not resume inside the exit-tree notification")
	await _wait_for(responder, box, "done")
	T.eq(box.get("done"), true, "the call finished")
	var got: Variant = box.get("got")
	T.is_err(got, "HostGone", "the host is gone")
	T.ok(not (got is ScfgErr and (got as ScfgErr).is_a("TransportError")), "not a TransportError")
	T.eq(client.live_urls(), [responder.url("/slow"), responder.url("/ok")], "no endpoint cooled down")
	T.eq(responder.requests.size(), 1, "no failover request was started")
	await _teardown_without_host(responder)
	return T.done()


func test_host_removed_mid_request_is_host_gone() -> bool:
	var env := await _setup()
	var host: Node = env[0]
	var responder: F.Responder = env[1]
	var box := {}
	var client := ScfgRpcClient.new([responder.url("/slow"), responder.url("/ok")], {"transport": env[2]})
	_start_block_number(client, box)
	await _wait_for(responder, box, "request")
	responder.host = null
	host.get_parent().remove_child(host)
	await _wait_for(responder, box, "done")
	T.is_err(box.get("got"), "HostGone", "the host left the tree")
	T.eq(client.live_urls(), [responder.url("/slow"), responder.url("/ok")], "no endpoint cooled down")
	T.eq(responder.requests.size(), 1, "no failover request was started")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(host.get_child_count(), 0, "the abandoned HTTPRequest is freed")
	host.free()
	await _teardown_without_host(responder)
	return T.done()


func test_close_ends_requests_in_flight_with_host_gone() -> bool:
	# At a real quit the tree is torn down with no further frame, so the deferred tree_exiting
	# never resumes a waiting caller and its coroutines leak ("ObjectDB instances leaked at
	# exit"). close() ends them at once.
	var env := await _setup()
	var host: Node = env[0]
	var responder: F.Responder = env[1]
	var transport: ScfgHttpTransport = env[2]
	var client := ScfgRpcClient.new([responder.url("/slow"), responder.url("/ok")], {"transport": transport})
	var box := {}
	_start_block_number(client, box)
	await _wait_for(responder, box, "request")
	T.eq(responder.requests.size(), 1, "the request is in flight")
	transport.close()
	T.eq(box.get("done", false), true, "the caller resumed inside close()")
	var got: Variant = box.get("got")
	T.is_err(got, "HostGone", "the request ended")
	T.ok(not (got is ScfgErr and (got as ScfgErr).is_a("TransportError")), "not a TransportError")
	T.eq(client.live_urls(), [responder.url("/slow"), responder.url("/ok")], "no endpoint cooled down")
	var later: Variant = await transport.post_json(responder.url("/ok"), _payload(), 5.0)
	T.is_err(later, "HostGone", "a closed transport sends nothing more")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(responder.requests.size(), 1, "no failover or later request reached the network")
	T.eq(host.get_child_count(), 0, "the abandoned HTTPRequest is freed")
	await _teardown(env)
	return T.done()


func _teardown_without_host(responder: F.Responder) -> void:
	for c in responder.conns:
		(c["peer"] as StreamPeerTCP).disconnect_from_host()
	responder.server.stop()
	responder.queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame


## The whole path: ScfgRpcClient over the real transport, with failover and a JSON-RPC error.
func test_rpc_client_over_http() -> bool:
	var env := await _setup()
	var responder: F.Responder = env[1]
	var c := ScfgRpcClient.new([responder.url("/rate"), responder.url("/ok")], {"transport": env[2]})
	T.eq(await c.block_number(), 16, "failover from the 429 endpoint")
	T.eq(c.live_urls(), [responder.url("/ok")], "the 429 endpoint cools down")
	var e := ScfgRpcClient.new([responder.url("/rpc-error")], {"transport": env[2]})
	var got: Variant = await e.call_rpc("eth_getLogs", [])
	T.is_err(got, "RpcError", "the 400 body's error")
	if got is ScfgErr:
		T.eq((got as ScfgErr).code, -32600, "an int code")
		T.eq((got as ScfgErr).data, {"url": responder.url("/rpc-error")}, "url")
	await _teardown(env)
	return T.done()


# tests/unit/test_coio.py::test_an_overlong_json_integer_is_invalid_json: the integer literals of an
# RPC body as Python's json reads them with ints.int_from (strings, fractions, exponents aside).
# Bodies that Godot's JSON would warn about ("Exponent too high") are checked with the scanner.
func test_parse_rpc_body_refuses_an_overlong_integer() -> bool:
	var long := "9".repeat(4301)
	for text: String in ["{\"result\": %s}" % long, "[1, -%s]" % long, long, "{\"a\": \"x\\\\\", \"b\": %s}" % long]:
		T.eq(ScfgCoio.parse_rpc_body(text.to_utf8_buffer()), [false, null], text.substr(0, 30))
		T.ok(ScfgCoio._has_overlong_int(text.to_utf8_buffer()), "scanner: %s" % text.substr(0, 30))
	T.ok(not ScfgCoio._has_overlong_int(("{\"r\": %s}" % "9".repeat(4300)).to_utf8_buffer()), "4300 digits are read")
	var body := "{\"s\": \"%s\", \"t\": \"\\\"%s\", \"f\": 1.%s, \"e\": 1e-%s, \"g\": 1E+%s}" % [long, long, long, long, long]
	T.ok(not ScfgCoio._has_overlong_int(body.to_utf8_buffer()), "strings, fractions and exponents are not integers")
	var strings := "{\"s\": \"%s\", \"t\": \"\\\"%s\"}" % [long, long]
	var parsed: Array = ScfgCoio.parse_rpc_body(strings.to_utf8_buffer())
	T.eq(parsed[0], true, "digits in strings are text")
	if parsed[0]:
		T.eq((parsed[1] as Dictionary)["t"], "\"" + long, "the string after an escaped quote")
	T.eq(ScfgCoio.parse_rpc_body("not json".to_utf8_buffer()), [false, null], "not JSON")
	T.eq(ScfgCoio.parse_rpc_body("[1, 2.5]".to_utf8_buffer()), [true, [1.0, 2.5]], "short bodies as parse_body")
	T.eq(ScfgCoio.parse_rpc_body(PackedByteArray([0x5b, 0xff]) + long.to_utf8_buffer() + PackedByteArray([0x5d])),
		[false, null], "not UTF-8, without an engine error")
	var start := Time.get_ticks_msec()
	var big := "[" + ", ".join(PackedStringArray(Array(range(20000)).map(func(i: int) -> String: return str(i)))) + ", " + long + "]"
	T.eq(ScfgCoio.parse_rpc_body(big.to_utf8_buffer()), [false, null], "a long body")
	T.ok(Time.get_ticks_msec() - start < 3000, "scanned in linear time")
	return T.done()


## A body of many digit runs just under the limit is prefiltered in linear time: the regex tries a
## match only where a run starts (an unanchored `[0-9]{4301}` retried at every digit of every run,
## about 2.4 s for this 860 KB body). Strings, so Godot's JSON parses it without a warning.
func test_parse_rpc_body_is_linear_on_many_long_digit_runs() -> bool:
	var run := "\"" + "9".repeat(4300) + "\""
	var runs := PackedStringArray()
	for i in 200:
		runs.append(run)
	var raw := ("[" + ",".join(runs) + "]").to_utf8_buffer()
	var start := Time.get_ticks_usec()
	var parsed: Array = ScfgCoio.parse_rpc_body(raw)
	var took := (Time.get_ticks_usec() - start) / 1000.0
	T.eq(parsed[0], true, "200 strings of 4300 digits are JSON")
	T.ok(took < 500.0, "prefiltered in linear time (%.0f ms)" % took)
	return T.done()
