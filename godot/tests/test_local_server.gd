## ScfgCheckoutServer (addons/scfg/local_server.gd): what only the GDScript server has to prove.
## Its HTTP behaviour against local_server.py lives in the shared suite
## (tests/server_security/test_server_security.py, through tests/integration/server_driver.gd).
##
## Here: pump() never waits on a socket (a slow, partial or non-reading client, a build that takes
## frames), the deadlines, and invalid callbacks. The tests drive the server synchronously:
## clients are StreamPeerTCP sockets polled in the same loop, OS.delay_msec(1) lets the kernel
## move bytes between pumps, and a fake clock (`_clock`) stands in for the passing seconds.
##
## "Never waits" is checked by structure, not by timing a pump (a loaded machine makes any time
## budget flaky): every accepted socket is used through a Recorder (the server's `_wrap_peer`
## hook), and `_pump` checks that each pump made a bounded number of socket calls on each
## connection (a loop waiting for bytes polls again and again), never read more than the socket
## said it had, and never called a method that waits (get_data, put_data and the like).
extends RefCounted

const T := preload("res://tests/t.gd")

const ACCOUNT := "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"
const TX := "0xabababababababababababababababababababababababababababababababab"
## The socket calls one pump may make on one connection: read what is there (poll, status,
## available, read), answer (at most WRITE_CHUNKS writes) and then poll, status and available
## once more before it closes or lingers (disconnect).
const MAX_CALLS_PER_PUMP := 8 + ScfgCheckoutServer.WRITE_CHUNKS


## Calls without a reset (by _pump) after which the socket reports an error, so a loop waiting
## on it ends instead of hanging the test.
const HANG_GUARD := 1000000


## What the server uses an accepted StreamPeerTCP through (its `_wrap_peer` test hook): the
## non-blocking methods it may call, passed on and counted, and the blocking ones, recorded as
## a violation and refused.
class Recorder:
	extends RefCounted
	var peer: StreamPeerTCP
	var calls := 0  # since _pump last reset it
	var available := 0  # what get_available_bytes said last
	var violations: Array[String] = []

	func _init(p: StreamPeerTCP) -> void:
		peer = p

	func _count() -> bool:
		calls += 1
		return calls <= HANG_GUARD

	func poll() -> Error:
		return peer.poll() if _count() else ERR_CONNECTION_ERROR

	func get_status() -> StreamPeerTCP.Status:
		return peer.get_status() if _count() else StreamPeerTCP.STATUS_ERROR

	func get_available_bytes() -> int:
		available = peer.get_available_bytes() if _count() else 0
		return available

	func get_partial_data(n: int) -> Array:
		if n > available:
			violations.append("get_partial_data(%d) with %d available" % [n, available])
		return peer.get_partial_data(n) if _count() else [ERR_CONNECTION_ERROR, PackedByteArray()]

	func put_partial_data(data: PackedByteArray) -> Array:
		return peer.put_partial_data(data) if _count() else [ERR_CONNECTION_ERROR, 0]

	func disconnect_from_host() -> void:
		peer.disconnect_from_host()

	func _waits(name: String) -> Error:
		violations.append(name + " (it waits)")
		return ERR_UNAVAILABLE

	func get_data(_n: int) -> Array:
		return [_waits("get_data"), PackedByteArray()]

	func put_data(_data: PackedByteArray) -> Error:
		return _waits("put_data")

	func get_string(_n: int = -1) -> String:
		_waits("get_string")
		return ""

	func get_utf8_string(_n: int = -1) -> String:
		_waits("get_utf8_string")
		return ""

	func get_var(_objects: bool = false) -> Variant:
		return _waits("get_var")

	func put_var(_v: Variant, _objects: bool = false) -> void:
		_waits("put_var")

	func put_string(_v: String) -> void:
		_waits("put_string")

	func put_utf8_string(_v: String) -> void:
		_waits("put_utf8_string")


## A fake clock: `now[0]` seconds.
class Clock:
	extends RefCounted
	var now := 1000.0

	func read() -> float:
		return now


## A callback owner the tests free to make its Callables invalid.
class Owner:
	extends RefCounted
	var calls: Array = []

	func on_tx(tx: Variant, sender: Variant = null) -> void:
		calls.append([tx, sender])

	func status() -> Variant:
		return {"state": "waiting"}

	func asset(_name: String) -> Variant:
		return "x".to_utf8_buffer()

	func logo() -> Variant:
		return ["png".to_utf8_buffer(), "image/png"]

	func build(_account: Variant) -> Variant:
		return ScfgJob.new()


func _server(opts: Dictionary = {}) -> ScfgCheckoutServer:
	var o := {
		"load_asset": func(name: String) -> Variant: return ("<%s>" % name).to_utf8_buffer(),
		"session_info": {"chain_id": 42161},
		"on_tx": func(_tx: Variant, _sender: Variant = null) -> void: pass,
		"get_status": func() -> Variant: return {"state": "waiting"},
	}
	o.merge(opts, true)
	var srv := ScfgCheckoutServer.new(o)
	var clock := Clock.new()
	srv._clock = clock.read
	srv.set_meta("clock", clock)
	var recorders: Array[Recorder] = []
	srv.set_meta("recorders", recorders)
	srv._wrap_peer = func(peer: StreamPeerTCP) -> Object:
		var r := Recorder.new(peer)
		recorders.append(r)
		return r
	T.eq(srv.start(), null, "the server starts")
	return srv


func _clock(srv: ScfgCheckoutServer) -> Clock:
	return srv.get_meta("clock")


## A connected client (pumping the server until the connection is accepted), or null.
func _connect(srv: ScfgCheckoutServer) -> StreamPeerTCP:
	var c := StreamPeerTCP.new()
	if c.connect_to_host("127.0.0.1", srv.port) != OK:
		return null
	for i in 500:
		c.poll()
		srv.pump()
		if c.get_status() == StreamPeerTCP.STATUS_CONNECTED and srv._conns.size() > 0:
			return c
		OS.delay_msec(1)
	return null


func _send(c: StreamPeerTCP, text: String) -> void:
	c.put_data(text.to_utf8_buffer())


func _head(srv: ScfgCheckoutServer, method: String, sub: String, length: int = -1) -> String:
	var h := "%s /c/%s/%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n" % [method, srv.secret, sub, srv.port]
	if length >= 0:
		h += "Content-Length: %d\r\n" % length
	return h + "\r\n"


## Pumps `frames` times (1 ms apart) and returns the most socket calls one pump made on one
## connection; every waiting call or over-long read so far is a failure.
func _pump(srv: ScfgCheckoutServer, frames: int) -> int:
	var recorders: Array[Recorder] = srv.get_meta("recorders")
	var worst := 0
	for i in frames:
		for r in recorders:
			r.calls = 0
		srv.pump()
		for r in recorders:
			worst = maxi(worst, r.calls)
		OS.delay_msec(1)
	for r in recorders:
		T.eq(r.violations, [] as Array[String], "pump() never waits on a socket")
		r.violations.clear()
	return worst


## Everything the client has received so far, pumping the server meanwhile, until a whole response
## (head + Content-Length body) is there or `frames` pumps passed. {status, headers, body} or {}.
func _response(srv: ScfgCheckoutServer, c: StreamPeerTCP, frames: int = 500) -> Dictionary:
	var buf := PackedByteArray()
	for i in frames:
		srv.pump()
		c.poll()
		var n := c.get_available_bytes()
		if n > 0:
			buf.append_array(c.get_partial_data(n)[1])
		var parsed := _parse(buf)
		if not parsed.is_empty():
			return parsed
		OS.delay_msec(1)
	return {}


static func _parse(buf: PackedByteArray) -> Dictionary:
	var text := buf.get_string_from_ascii()
	var end := text.find("\r\n\r\n")
	if end < 0:
		return {}
	var lines := text.substr(0, end).split("\r\n")
	var headers := {}
	for i in range(1, lines.size()):
		var colon := lines[i].find(":")
		headers[lines[i].substr(0, colon).to_lower()] = lines[i].substr(colon + 1).strip_edges()
	var length := int(headers.get("content-length", "0"))
	if buf.size() < end + 4 + length:
		return {}
	return {"status": int(lines[0].split(" ")[1]), "headers": headers,
		"body": buf.slice(end + 4, end + 4 + length).get_string_from_utf8()}


## True once the server has closed the client's connection (pumping meanwhile).
func _closed(srv: ScfgCheckoutServer, c: StreamPeerTCP, frames: int = 500) -> bool:
	for i in frames:
		srv.pump()
		c.poll()
		if c.get_status() != StreamPeerTCP.STATUS_CONNECTED:
			return true
		var n := c.get_available_bytes()
		if n > 0:
			c.get_partial_data(n)
		OS.delay_msec(1)
	return false


func test_url_and_secret() -> bool:
	var srv := _server()
	T.ok(srv.running, "running")
	T.ok(srv.port > 0, "a port")
	T.eq(srv.url, "http://127.0.0.1:%d/c/%s/" % [srv.port, srv.secret], "url")
	T.eq(srv.secret.length(), 22, "16 random bytes, base64url without padding")
	T.ok(RegEx.create_from_string("^[A-Za-z0-9_-]{22}$").search(srv.secret) != null, "url-safe: %s" % srv.secret)
	T.ok(ScfgCheckoutServer.new({}).secret != srv.secret, "a new secret per server")
	srv.stop()
	return T.done()


func test_start_is_idempotent_and_a_restart_keeps_the_secret() -> bool:
	var srv := _server()
	var port := srv.port
	var secret := srv.secret
	T.eq(srv.start(), null, "a second start")
	T.eq(srv.port, port, "same port")
	srv.stop()
	T.ok(not srv.running, "stopped")
	srv.stop()  # twice is fine
	T.eq(srv.start(), null, "restarts")
	T.eq(srv.secret, secret, "same secret, as Python's (a new port)")
	srv.stop()
	return T.done()


func test_start_failure_is_an_err() -> bool:
	var srv := ScfgCheckoutServer.new({"host": "192.0.2.1"})  # TEST-NET-1: not on this machine
	T.is_err(srv.start(), "RuntimeError", "cannot bind")
	T.ok(not srv.running, "not running")
	srv.pump()  # harmless
	return T.done()


func test_slow_client_does_not_block_the_frame() -> bool:
	var srv := _server()
	var c := _connect(srv)
	T.ok(c != null, "connected")
	if c == null:
		return T.done()
	_send(c, "GET /c/%s/ HTTP/1.1\r\nHost: 127.0" % srv.secret)  # and never the rest
	var worst := _pump(srv, 50)
	T.ok(worst <= MAX_CALLS_PER_PUMP, "pump() returns at once with a partial request (at most %d socket calls)" % worst)
	T.eq(srv._conns.size(), 1, "the connection is held before the deadline")
	# another client is served meanwhile
	var other := _connect(srv)
	_send(other, _head(srv, "GET", "status"))
	var got := _response(srv, other)
	T.eq(got.get("status"), 200, "a second client is answered while the first stalls")
	other.disconnect_from_host()
	_pump(srv, 20)
	_clock(srv).now += ScfgCheckoutServer.DEADLINE_S - 0.5
	srv.pump()
	T.eq(srv._conns.size(), 1, "the slow client is still held at 9.5 s")
	_clock(srv).now += 1.0
	worst = _pump(srv, 3)
	T.ok(worst <= MAX_CALLS_PER_PUMP, "dropping is immediate too (%d socket calls)" % worst)
	T.ok(_closed(srv, c), "the slow client is dropped after the deadline")
	srv.stop()
	return T.done()


func test_a_partial_body_is_dropped_after_the_deadline() -> bool:
	var srv := _server()
	var c := _connect(srv)
	_send(c, _head(srv, "POST", "tx", 100) + "{\"tx")
	T.ok(_pump(srv, 30) <= MAX_CALLS_PER_PUMP, "pump() does not wait for the body")
	T.eq(srv._conns.size(), 1, "held while the body may still come")
	_clock(srv).now += ScfgCheckoutServer.DEADLINE_S + 0.1
	T.ok(_closed(srv, c), "dropped after the deadline")
	srv.stop()
	return T.done()


func test_byte_at_a_time_client_is_served() -> bool:
	var srv := _server()
	var c := _connect(srv)
	var req := _head(srv, "GET", "session.json")
	for i in req.length():
		_send(c, req[i])
		srv.pump()
	var got := _response(srv, c)
	T.eq(got.get("status"), 200, "served")
	T.json_eq(JSON.parse_string(got.get("body", "null")), {"chain_id": 42161}, "session info")
	srv.stop()
	return T.done()


func test_a_client_that_never_reads_does_not_block_the_frame() -> bool:
	var big := PackedByteArray()
	big.resize(32 << 20)  # far more than the socket buffers hold
	var srv := _server({"load_asset": func(_n: String) -> Variant: return big})
	var c := _connect(srv)
	_send(c, _head(srv, "GET", "app.js"))
	# the pump that answers copies the 32 MB into the answer (work, not waiting); time the rest
	for i in 200:
		srv.pump()
		if srv._conns.size() == 1 and srv._conns[0].state == ScfgCheckoutServer._State.WRITING:
			break
		OS.delay_msec(1)
	T.eq(srv._conns[0].state if srv._conns.size() == 1 else null, ScfgCheckoutServer._State.WRITING, "answering")
	var worst := _pump(srv, 40)
	T.ok(worst <= MAX_CALLS_PER_PUMP, "pump() writes what the socket takes and returns (at most %d socket calls)" % worst)
	T.eq(srv._conns.size(), 1, "still writing")
	_clock(srv).now += ScfgCheckoutServer.DEADLINE_S + 0.1
	srv.pump()
	T.eq(srv._conns.size(), 0, "dropped after the deadline")
	c.disconnect_from_host()
	srv.stop()
	return T.done()


func test_build_waits_for_the_job_without_blocking() -> bool:
	var job := ScfgJob.new()
	var asked: Array = []
	var srv := _server({"build_tx": func(account: Variant) -> Variant:
		asked.append(account)
		return job})
	var c := _connect(srv)
	var body := JSON.stringify({"account": ACCOUNT})
	_send(c, _head(srv, "POST", "build", body.length()) + body)
	var worst := _pump(srv, 30)
	T.ok(worst <= MAX_CALLS_PER_PUMP, "pump() returns at once while the job runs (at most %d socket calls)" % worst)
	T.eq(asked, [ACCOUNT], "build_tx got the normalised account once")
	c.poll()
	T.eq(c.get_available_bytes(), 0, "no answer before the job finishes")
	# held past the 10 s request deadline: the build has its own
	_clock(srv).now += ScfgCheckoutServer.DEADLINE_S + 1.0
	srv.pump()
	T.eq(srv._conns.size(), 1, "the build connection is still held")
	job._succeed({"transaction": "AA==", "sol_balance": "2"})
	var got := _response(srv, c)
	T.eq(got.get("status"), 200, "answered when the job finished")
	T.json_eq(JSON.parse_string(got.get("body", "null")), {"transaction": "AA==", "sol_balance": "2"}, "the job's result")
	T.eq((got.get("headers", {}) as Dictionary).get("content-type"), "application/json", "JSON")
	srv.stop()
	return T.done()


func test_build_timeout_answers_503_and_cancels_the_job() -> bool:
	var job := ScfgJob.new()
	var srv := _server({"build_tx": func(_a: Variant) -> Variant: return job})
	var c := _connect(srv)
	var body := JSON.stringify({"account": ACCOUNT})
	_send(c, _head(srv, "POST", "build", body.length()) + body)
	_pump(srv, 10)
	_clock(srv).now += ScfgPageMessages.BUILD_TIMEOUT_S - 0.5
	srv.pump()
	T.ok(not job.cancelled(), "not before BUILD_TIMEOUT_S")
	_clock(srv).now += 1.0
	var got := _response(srv, c)
	T.eq(got.get("status"), 503, "503 after BUILD_TIMEOUT_S")
	T.json_eq(JSON.parse_string(got.get("body", "null")), {"error": "unavailable"}, "body")
	T.ok(job.cancelled(), "the job is cancelled")
	srv.stop()
	return T.done()


func test_failed_or_odd_build_jobs_are_503() -> bool:
	var failed := ScfgJob.new()
	failed._fail(ScfgErr.make("RuntimeError", "rpc down"))
	var running := ScfgJob.new()
	running._start()
	running._succeed("not a dict")
	for row: Array in [["failed job", func(_a: Variant) -> Variant: return failed],
			["a non-dict result", func(_a: Variant) -> Variant: return running],
			["not a job", func(_a: Variant) -> Variant: return {"transaction": "AA=="}],
			["an error", func(_a: Variant) -> Variant: return ScfgErr.make("RuntimeError", "busy")],
			["null", func(_a: Variant) -> Variant: return null]]:
		var srv := _server({"build_tx": row[1]})
		var c := _connect(srv)
		var body := JSON.stringify({"account": ACCOUNT})
		_send(c, _head(srv, "POST", "build", body.length()) + body)
		var got := _response(srv, c)
		T.eq(got.get("status"), 503, row[0])
		srv.stop()
	return T.done()


func test_stop_cancels_a_pending_build() -> bool:
	var job := ScfgJob.new()
	var srv := _server({"build_tx": func(_a: Variant) -> Variant: return job})
	var c := _connect(srv)
	var body := JSON.stringify({"account": ACCOUNT})
	_send(c, _head(srv, "POST", "build", body.length()) + body)
	_pump(srv, 10)
	srv.stop()
	T.ok(job.cancelled(), "a page that is gone gets no build")
	T.eq(srv._conns.size(), 0, "connections closed")
	return T.done()


func test_invalid_callbacks_answer_without_a_script_error() -> bool:
	var owner := Owner.new()
	var opts := {"on_tx": owner.on_tx, "get_status": owner.status, "load_asset": owner.asset,
		"load_logo": owner.logo, "build_tx": owner.build}
	owner = null  # frees it: every Callable above is now invalid
	T.ok(not (opts["on_tx"] as Callable).is_valid(), "the callables are invalid")
	var srv := _server(opts)
	var tx_body := JSON.stringify({"tx_hash": TX})
	var build_body := JSON.stringify({"account": ACCOUNT})
	for row: Array in [[_head(srv, "POST", "tx", tx_body.length()) + tx_body, 503, "on_tx"],
			[_head(srv, "GET", "status"), 503, "get_status"],
			[_head(srv, "GET", "app.js"), 500, "load_asset"],
			[_head(srv, "GET", "logo"), 404, "load_logo"],
			[_head(srv, "POST", "build", build_body.length()) + build_body, 503, "build_tx"]]:
		var c := _connect(srv)
		_send(c, row[0])
		var got := _response(srv, c)
		T.eq(got.get("status"), row[1], "an invalid %s" % row[2])
	srv.stop()
	var plain := _server({"on_tx": Callable(), "get_status": null, "load_asset": 5})
	for row: Array in [[_head(plain, "POST", "tx", tx_body.length()) + tx_body, 503],
			[_head(plain, "GET", "status"), 503], [_head(plain, "GET", ""), 500]]:
		var c := _connect(plain)
		_send(c, row[0])
		T.eq(_response(plain, c).get("status"), row[1], "a missing callback: %s" % row[0].get_slice(" ", 1))
	plain.stop()
	return T.done()


func test_logo_must_be_a_raster_image() -> bool:
	var types: Array = ScfgCheckoutServer.RASTER_TYPES
	T.eq(types.get_typed_builtin(), TYPE_STRING, "RASTER_TYPES is an Array[String]")
	T.eq(types, ["image/png", "image/jpeg", "image/jpeg", "image/webp"], "the logo types ScfgConfig accepts")
	for row: Array in [["image/png", 200], ["image/jpeg", 200], ["image/webp", 200],
			["image/svg+xml", 404], ["text/html", 404], ["image/png\r\nX-Evil: 1", 404]]:
		var ctype: String = row[0]
		var srv := _server({"load_logo": func() -> Variant: return ["<svg/>".to_utf8_buffer(), ctype]})
		var c := _connect(srv)
		_send(c, _head(srv, "GET", "logo"))
		var got := _response(srv, c)
		T.eq(got.get("status"), row[1], ctype.c_escape())
		T.ok(not (got.get("headers", {}) as Dictionary).has("x-evil"), "no header injection")
		srv.stop()
	for bad: Variant in [null, [], ["x".to_utf8_buffer()], ["x", "image/png"], [PackedByteArray(), 5]]:
		var srv := _server({"load_logo": func() -> Variant: return bad})
		var c := _connect(srv)
		_send(c, _head(srv, "GET", "logo"))
		T.eq(_response(srv, c).get("status"), 404, "load_logo returned %s" % [bad])
		srv.stop()
	return T.done()


func test_oversized_head_is_431() -> bool:
	var srv := _server()
	var c := _connect(srv)
	# lines each under MAX_LINE (as http.client reads them) that add up to more than MAX_HEAD
	var pad := "X-Pad: %s\r\n" % "a".repeat(ScfgCheckoutServer.MAX_LINE - 10)
	var lines := ScfgCheckoutServer.MAX_HEAD / pad.length() + 1
	_send(c, "GET /c/%s/ HTTP/1.1\r\n" % srv.secret + pad.repeat(lines))
	T.eq(_response(srv, c).get("status"), 431, "a head over MAX_HEAD")
	c = _connect(srv)
	_send(c, "GET /c/%s/ HTTP/1.1\r\nX-Pad: %s" % [srv.secret, "a".repeat(ScfgCheckoutServer.MAX_LINE)])
	T.eq(_response(srv, c).get("status"), 431, "a header line over MAX_LINE, before its end arrives")
	# http.client counts the blank line that ends the head as one of its 100: 99 headers at most
	var host := "Host: 127.0.0.1:%d\r\n" % srv.port
	var most := "GET /c/%s/ HTTP/1.1\r\n" % srv.secret + host
	for i in 98:
		most += "X-%d: 1\r\n" % i
	c = _connect(srv)
	_send(c, most + "\r\n")
	T.eq(_response(srv, c).get("status"), 200, "99 header lines are read")
	c = _connect(srv)
	_send(c, most + "X-98: 1\r\n\r\n")
	T.eq(_response(srv, c).get("status"), 431, "100 header lines are too many (http.client's limit)")
	srv.stop()
	return T.done()


func test_on_tx_gets_the_normalised_hint() -> bool:
	var got_calls: Array = []
	var srv := _server({"on_tx": func(tx: Variant, sender: Variant = null) -> void: got_calls.append([tx, sender])})
	var body := JSON.stringify({"tx_hash": TX.to_upper().replace("0X", "0x"),
		"from": "0x52908400098527886e0f7030069857d2e4169ee7"})
	var c := _connect(srv)
	_send(c, _head(srv, "POST", "tx", body.length()) + body)
	var got := _response(srv, c)
	T.eq(got.get("status"), 202, "202")
	T.json_eq(JSON.parse_string(got.get("body", "null")), {"ok": true}, "body")
	T.eq(got_calls, [[TX, "0x52908400098527886E0F7030069857D2E4169EE7"]], "normalised")
	srv.stop()
	return T.done()


func test_a_callback_that_stops_the_server_is_safe() -> bool:
	var holder := [null]
	var srv := _server({"on_tx": func(_tx: Variant, _sender: Variant = null) -> void: (holder[0] as ScfgCheckoutServer).stop()})
	holder[0] = srv
	var body := JSON.stringify({"tx_hash": TX})
	var c := _connect(srv)
	_send(c, _head(srv, "POST", "tx", body.length()) + body)
	_pump(srv, 5)
	T.ok(not srv.running, "stopped from inside pump()")
	T.eq(srv._conns.size(), 0, "no connection left")
	holder[0] = null
	return T.done()


func test_connections_are_capped() -> bool:
	var srv := _server()
	var clients: Array[StreamPeerTCP] = []
	for i in ScfgCheckoutServer.MAX_CONNECTIONS + 4:
		var c := StreamPeerTCP.new()
		c.connect_to_host("127.0.0.1", srv.port)
		clients.append(c)
	# TCPServer's listen backlog is small (8): the kernel completes the other handshakes as the
	# queue frees, on the client's retransmitted first byte
	var t0 := Time.get_ticks_msec()
	while srv._conns.size() < ScfgCheckoutServer.MAX_CONNECTIONS and Time.get_ticks_msec() - t0 < 8000:
		_poll_and_send_a_byte(clients)
		srv.pump()
		OS.delay_msec(2)
	_pump(srv, 50)
	T.eq(srv._conns.size(), ScfgCheckoutServer.MAX_CONNECTIONS, "at most MAX_CONNECTIONS are taken")
	_clock(srv).now += ScfgCheckoutServer.DEADLINE_S + 0.1
	srv.pump()  # the idle ones are dropped...
	t0 = Time.get_ticks_msec()
	while srv._conns.is_empty() and Time.get_ticks_msec() - t0 < 8000:
		_poll_and_send_a_byte(clients)
		srv.pump()
		OS.delay_msec(2)
	var taken := srv._conns.size()
	T.ok(taken > 0 and taken <= 4, "...and the waiting ones taken next (%d)" % taken)
	for c in clients:
		c.disconnect_from_host()
	srv.stop()
	return T.done()


static func _poll_and_send_a_byte(clients: Array[StreamPeerTCP]) -> void:
	for c in clients:
		c.poll()
		if c.get_status() == StreamPeerTCP.STATUS_CONNECTED and not c.has_meta("sent"):
			c.set_meta("sent", true)
			c.put_data("G".to_ascii_buffer())
