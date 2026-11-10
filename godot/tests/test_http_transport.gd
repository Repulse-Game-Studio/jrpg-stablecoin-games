## ScfgHttpTransport (http_transport.gd) against a tiny HTTP/1.1 responder that runs inside the
## test, on a TCPServer bound to 127.0.0.1 at a free port.
##
## Getting a Node in the tree: the runner (run.gd) is a SceneTree script, so
## `Engine.get_main_loop()` is that SceneTree and its `root` Viewport is inside the tree and
## processed every frame. Each test adds a fresh host Node (the transport's HTTPRequest children
## go under it) and the responder Node (polled in its _process) under `root`, and frees both at
## the end. Nothing else in the suite needs the tree.
extends RefCounted

const T := preload("res://tests/t.gd")

const _OK_BODY := "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"0x10\"}"
const _RPC_ERROR_BODY := "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,\"message\":\"block range too large\"}}"


## Answers each request by its path; `/slow` never answers. Records every request.
class Responder:
	extends Node
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
			"/ok":
				return [200, _OK_BODY.to_utf8_buffer()]
			"/rate":
				return [429, "Too Many Requests".to_utf8_buffer()]
			"/rpc-error":
				return [400, _RPC_ERROR_BODY.to_utf8_buffer()]
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
				return [201, _RPC_ERROR_BODY.to_utf8_buffer()]
		return []


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
	var responder := Responder.new()
	responder.host = host
	root.add_child(responder)
	T.ok(responder.start(), "the responder listens")
	return [host, responder, ScfgHttpTransport.new(host)]


func _teardown(env: Array) -> void:
	var responder: Responder = env[1]
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
	var responder: Responder = env[1]
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
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json((env[1] as Responder).url("/rate"), _payload(), 5.0)
	T.is_err(got, "TransportError", "429")
	T.eq(_status_of(got), 429, "status")
	T.eq((got as ScfgErr).message if got is ScfgErr else "", "HTTP 429", "message")
	await _teardown(env)
	return T.done()


func test_4xx_with_a_jsonrpc_error_body_is_the_body() -> bool:
	var env := await _setup()
	var tr: ScfgHttpTransport = env[2]
	var responder: Responder = env[1]
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
	var responder: Responder = env[1]
	for path: String in ["/not-json", "/bad-utf8", "/bom", "/nul"]:
		var got: Variant = await tr.post_json(responder.url(path), _payload(), 5.0)
		T.is_err(got, "TransportError", path)
		T.eq((got as ScfgErr).message if got is ScfgErr else "", "invalid JSON response", "%s message" % path)
		T.eq(_status_of(got), null, "%s has no status" % path)
	T.eq(await tr.post_json(responder.url("/null"), _payload(), 5.0), null, "a JSON null body is null")
	await _teardown(env)
	return T.done()


func test_timeout_is_a_transport_error() -> bool:
	var env := await _setup()
	var host: Node = env[0]
	var start := Time.get_ticks_msec()
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json((env[1] as Responder).url("/slow"), _payload(), 0.5)
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
	var responder: Responder = env[1]
	var url := responder.url("/ok")
	responder.server.stop()
	var got: Variant = await (env[2] as ScfgHttpTransport).post_json(url, _payload(), 5.0)
	T.is_err(got, "TransportError", "refused")
	T.eq(_status_of(got), null, "no status")
	await _teardown(env)
	return T.done()


func test_host_outside_the_tree_is_a_transport_error() -> bool:
	var orphan := Node.new()
	var got: Variant = await ScfgHttpTransport.new(orphan).post_json("http://127.0.0.1:9/", {}, 1.0)
	T.is_err(got, "TransportError", "no tree")
	orphan.free()
	return T.done()


## The whole path: ScfgRpcClient over the real transport, with failover and a JSON-RPC error.
func test_rpc_client_over_http() -> bool:
	var env := await _setup()
	var responder: Responder = env[1]
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
