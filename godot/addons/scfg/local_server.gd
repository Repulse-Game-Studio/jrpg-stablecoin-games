## The localhost checkout server for desktop browser wallets: the GDScript twin of
## scfg_core/local_server.py (CheckoutServer), route by route.
##
## Browser extensions (MetaMask, Rabby, Phantom...) only talk to web pages, never to a game. So
## the game serves the bundled checkout page on 127.0.0.1 and opens the browser on it. The page
## connects the wallet, sends the transfer and posts the tx hash back here.
##
## `ScfgCheckoutServer.new(opts)` takes local_server.CheckoutServer's keyword arguments as keys:
##   - load_asset(name) -> PackedByteArray: one of ASSETS (anything else it returns is a 500);
##   - session_info: the value served as session.json;
##   - on_tx(tx_hash, sender): the page's hint, already normalised (ScfgPageMessages);
##   - get_status() -> Dictionary: served as status;
##   - load_logo() -> [PackedByteArray, content type] (optional; no route without it);
##   - build_tx(account) -> ScfgJob (optional, Solana; no route without it);
##   - host ("127.0.0.1").
## Every Callable is synchronous and checked with is_valid() before each call: one whose object
## was freed (a released checkout controller) answers 503 (500 for an asset, 404 for the logo),
## never a script error.
##
## The game calls `pump()` every frame. It accepts connections, reads what each socket has,
## answers and closes, and never waits on a socket: reads take only the bytes available, writes
## use put_partial_data and carry the rest to the next frame. `start()` binds a random free port
## and does not suspend; it returns null or an ScfgErr (RuntimeError) when it cannot listen.
## `stop()` closes everything and cancels a build a page is still waiting for.
##
## Security (as local_server.py):
##   - bound to 127.0.0.1 on a random port;
##   - every URL holds a 128-bit secret, /c/<secret>/..., otherwise 404;
##   - the Host header must be exactly 127.0.0.1:<port> (defeats DNS rebinding), otherwise 403;
##   - no CORS headers; a request body is at most MAX_BODY bytes (413), with a valid
##     Content-Length (400);
##   - every answer has Cache-Control: no-store, Referrer-Policy: no-referrer and
##     X-Content-Type-Options: nosniff;
##   - /build returns the *unsigned* transaction only for a valid account, and gives up after
##     BUILD_TIMEOUT_S (503, the job cancelled);
##   - a posted tx hash is only a hint: the watcher verifies it on-chain;
##   - the logo is served only as a raster image (png, jpeg, webp), never SVG.
## Godot only: a connection has DEADLINE_S to deliver its request (and to take its answer), at
## most MAX_CONNECTIONS are open at once, and a request head is at most MAX_HEAD bytes in all
## (431; Python caps only each line, at MAX_LINE, and the number of header lines).
##
## HTTP, as Python's BaseHTTPRequestHandler reads it: GET and POST only, any other method
## (HEAD included, which gets no body) is 501; a request line that is empty is closed without an
## answer, a bad version is 400 and HTTP/2+ is 505; lines may end in LF or CRLF, and a bare CR
## inside a header line ends that header (_headers); header names are case-insensitive and the
## first of a repeated header counts; a path starting with `//` is reduced to one `/`; the query
## string is ignored; Content-Length is read as Python's int().
## Every answer is HTTP/1.1 with `Connection: close`. Deviations: a two-word (HTTP/0.9) request
## line is a 400 (http.server answers it with a bare body), an answer is sent with its status
## line where http.server sends a bare error page (a bad request line), and the 501's body is
## plain text.
class_name ScfgCheckoutServer
extends RefCounted

const MAX_BODY := 4096
const ASSETS: Array[String] = ["index.html", "app.js", "evm.js", "solana.js"]
## Seconds a connection has to send its whole request, and again to take its answer.
const DEADLINE_S := 10.0
## One line of the head, in bytes with its line end: http.server reads the request line with
## readline(65537) and http.client each header line with readline(_MAXLINE + 1), so a longer
## request line is a 414 and a longer header line a 431 ("Line too long"). A browser may send
## 127.0.0.1 a Cookie header this large.
const MAX_LINE := 65536
## The request line plus headers in all, in bytes; more is a 431 (Godot only: Python has no total
## cap, only MAX_LINE per line and MAX_HEADERS lines).
const MAX_HEAD := 262144
## http.client's _MAXHEADERS. It counts the blank line that ends the head as one of them, so a
## request has at most MAX_HEADERS - 1 header lines; one more is a 431.
const MAX_HEADERS := 100
const MAX_CONNECTIONS := 32
## After an answer given before the whole request was read (a body never read, a head cut
## short): how long the rest is drained before closing, and how much of it. Closing on unread
## input resets the connection, and the client could lose the answer. An answer to a complete
## request closes as soon as it is written.
const LINGER_S := 1.0
const LINGER_MAX_BYTES := 65536
## put_partial_data takes a copy of what it is offered: offer the answer in pieces.
const WRITE_CHUNK := 65536
const WRITE_CHUNKS := 16
## The content types the logo is served as: the ones ScfgConfig accepts for a logo file.
static var RASTER_TYPES: Array[String] = Array(ScfgConfig.LOGO_TYPES.values(), TYPE_STRING, &"", null)

const _CTYPES := {
	"index.html": "text/html; charset=utf-8",
	"app.js": "application/javascript; charset=utf-8",
	"evm.js": "application/javascript; charset=utf-8",
	"solana.js": "application/javascript; charset=utf-8",
}
const _REASONS := {
	200: "OK", 202: "Accepted", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
	413: "Request Entity Too Large", 414: "Request-URI Too Long", 431: "Request Header Fields Too Large",
	500: "Internal Server Error", 501: "Not Implemented", 503: "Service Unavailable",
	505: "HTTP Version Not Supported",
}

enum _State { HEAD, BODY, BUILDING, WRITING, LINGER, CLOSED }

var load_asset: Variant
var session_info: Variant
var on_tx: Variant
var get_status: Variant
var load_logo: Variant
var build_tx: Variant
var host: String
## 16 random bytes, base64url without padding: Python's secrets.token_urlsafe(16).
var secret: String
## How long /build waits for its job. Python's module constant BUILD_TIMEOUT_S; tests lower it.
var build_timeout_s: float = ScfgPageMessages.BUILD_TIMEOUT_S

## The port bound by the last start() (0 before).
var port: int:
	get:
		return _port
var url: String:
	get:
		return "http://%s:%d/c/%s/" % [host, _port, secret]
var running: bool:
	get:
		return _tcp != null

## Seconds, monotonic. A test hook (tests/test_local_server.gd).
var _clock: Callable = _ticks
## A test hook (tests/test_local_server.gd): `func(peer: StreamPeerTCP) -> Object`, what each
## accepted connection is used through (a recorder of the calls pump() makes), or null.
var _wrap_peer: Variant = null
var _tcp: TCPServer = null
var _port := 0
var _conns: Array = []  # of _Conn


class _Conn:
	extends RefCounted
	var peer: Object  # a StreamPeerTCP, or what the _wrap_peer test hook made of one
	var state := _State.HEAD
	var deadline := 0.0
	var buf := PackedByteArray()
	var line_start := 0  # where the line being read starts in buf
	var lines: Array[String] = []  # the request line, then the header lines (Latin-1)
	var method := ""
	var path := ""
	var sub := ""  # the path after /c/<secret>/
	var need := 0  # body bytes to read
	var job: ScfgJob = null
	var out := PackedByteArray()
	var sent := 0
	var drained := 0
	var complete := false  # the whole request was read (nothing more is expected from the client)


func _init(opts: Dictionary = {}) -> void:
	load_asset = opts.get("load_asset")
	session_info = opts.get("session_info", {})
	on_tx = opts.get("on_tx")
	get_status = opts.get("get_status")
	load_logo = opts.get("load_logo")
	build_tx = opts.get("build_tx")
	host = str(opts.get("host", "127.0.0.1"))
	secret = Marshalls.raw_to_base64(Crypto.new().generate_random_bytes(16)) \
		.replace("+", "-").replace("/", "_").replace("=", "")


static func _ticks() -> float:
	return Time.get_ticks_usec() / 1000000.0


## Binds 127.0.0.1 (`host`) on a random free port; a second start() while running does nothing.
## @fallible
func start() -> Variant:
	if _tcp != null:
		return null
	var tcp := TCPServer.new()
	var err := tcp.listen(0, host)
	if err != OK:
		return ScfgErr.make("RuntimeError", "cannot listen on %s: %s" % [host, error_string(err)])
	_tcp = tcp
	_port = tcp.get_local_port()
	return null


func stop() -> void:
	if _tcp == null:
		return
	_tcp.stop()
	_tcp = null
	var conns := _conns
	_conns = []
	for c: _Conn in conns:
		if c.job != null and not c.job.done():
			c.job.cancel()  # don't build it later for a page that is gone
		_close(c)


## Accept, read, answer and close, without waiting on any socket. Call it every frame.
func pump() -> void:
	if _tcp == null:
		return
	while _conns.size() < MAX_CONNECTIONS and _tcp.is_connection_available():
		var peer := _tcp.take_connection()
		if peer == null:
			break
		var c := _Conn.new()
		c.peer = peer if _wrap_peer == null else (_wrap_peer as Callable).call(peer)
		c.deadline = _now() + DEADLINE_S
		_conns.append(c)
	for c: _Conn in _conns.duplicate():
		if _tcp == null:
			return  # a callback stopped the server: stop() closed everything
		_step(c)
	_conns = _conns.filter(func(c: _Conn) -> bool: return c.state != _State.CLOSED)


func _now() -> float:
	return _clock.call()


func _step(c: _Conn) -> void:
	match c.state:
		_State.HEAD, _State.BODY:
			if _now() >= c.deadline:
				_close(c)
				return
			c.peer.poll()
			if c.peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
				_close(c)
				return
			if not _read(c):
				return
			if c.state == _State.HEAD:
				_read_head(c)
			if c.state == _State.BODY and c.buf.size() >= c.need:
				_on_body(c, c.buf.slice(0, c.need))
		_State.BUILDING:
			_check_build(c)
		_State.WRITING:
			_write(c)
		_State.LINGER:
			_linger(c)


## Takes the bytes the socket has, up to what this state may hold. False if the connection broke.
func _read(c: _Conn) -> bool:
	var room := (MAX_HEAD + 1 - c.buf.size()) if c.state == _State.HEAD else (c.need - c.buf.size())
	var n := mini(c.peer.get_available_bytes(), room)
	if n <= 0:
		return true
	var got: Array = c.peer.get_partial_data(n)
	if got[0] != OK:
		_close(c)
		return false
	c.buf.append_array(got[1])
	return true


## Splits off the complete lines received so far: the request line, then headers up to the blank
## line. Answers a bad request line at once, as http.server does before reading the headers.
func _read_head(c: _Conn) -> void:
	while c.state == _State.HEAD:
		var nl := c.buf.find(10, c.line_start)
		var line_size := (nl + 1 if nl >= 0 else c.buf.size()) - c.line_start  # with its line end
		if line_size > MAX_LINE:
			if c.lines.is_empty():
				_send(c, 414, "request line too long".to_utf8_buffer())
			else:
				_send(c, 431, "line too long".to_utf8_buffer())
			return
		if nl < 0:
			if c.buf.size() > MAX_HEAD:
				_send(c, 431, "headers too large".to_utf8_buffer())
			return
		var raw := c.buf.slice(c.line_start, nl)
		c.line_start = nl + 1
		if c.lines.is_empty():
			_request_line(c, _latin1(raw).rstrip("\r\n"))
		elif c.lines.size() > MAX_HEADERS:
			# MAX_HEADERS header lines are in: this line, a header or the blank end, is one more
			_send(c, 431, "too many headers".to_utf8_buffer())
		elif raw.is_empty() or (raw.size() == 1 and raw[0] == 13):
			c.buf = c.buf.slice(c.line_start)
			_on_head(c)
		else:
			c.lines.append(_latin1(raw))


## BaseHTTPRequestHandler.parse_request's checks of the request line.
func _request_line(c: _Conn, line: String) -> void:
	var words := _py_split(line)
	if words.is_empty():
		_close(c)  # http.server closes without an answer
		return
	if words.size() >= 3:
		var version := words[-1]
		var numbers := version.substr(5).split(".") if version.begins_with("HTTP/") else PackedStringArray()
		var valid := numbers.size() == 2
		for n in numbers:
			valid = valid and n.length() > 0 and n.length() <= 10 and _ascii_digits(n)
		if not valid:
			_send(c, 400, "bad request version".to_utf8_buffer())
			return
		if (numbers[0] as String).to_int() >= 2:
			_send(c, 505, "invalid http version".to_utf8_buffer())
			return
	if words.size() != 3:
		_send(c, 400, "bad request syntax".to_utf8_buffer())
		return
	c.method = words[0]
	c.path = words[1]
	if c.path.begins_with("//"):
		c.path = "/" + c.path.lstrip("/")  # http.server's gh-87389 fix
	c.lines.append(line)


func _on_head(c: _Conn) -> void:
	var headers := _headers(c.lines.slice(1))
	c.complete = not (headers.has("content-length") or headers.has("transfer-encoding"))
	if c.method != "GET" and c.method != "POST":
		_send(c, 501, "unsupported method".to_utf8_buffer(), "text/plain; charset=utf-8", c.method == "HEAD")
		return
	if headers.get("host") != "%s:%d" % [host, _port]:
		_send(c, 403, "forbidden".to_utf8_buffer())
		return
	var prefix := "/c/%s/" % secret
	var path := c.path.get_slice("?", 0)
	if not path.begins_with(prefix):
		_not_found(c)
		return
	c.sub = path.substr(prefix.length())
	if c.method == "GET":
		_serve_get(c)
		return
	if c.sub == "build" and build_tx == null:
		_not_found(c)
		return
	if c.sub != "build" and c.sub != "tx":
		_not_found(c)
		return
	var length := -1
	var parsed: Variant = ScfgPy.py_int(headers.get("content-length", "0"))  # err-ok: an ScfgErr (not a number) leaves -1, a 400
	if parsed is ScfgBigInt:
		length = MAX_BODY + 1
	elif parsed is int:
		length = parsed
	if length <= 0 or length > MAX_BODY:
		_send(c, 413 if length > MAX_BODY else 400, "bad body".to_utf8_buffer())
		return
	c.need = length
	c.state = _State.BODY
	if c.buf.size() >= c.need:
		_on_body(c, c.buf.slice(0, c.need))


## The header block as email.parser (compat32) reads it: {lowercase name: first value}.
## `lines` are the head's header lines as read at each LF (without the LF). The block they make
## is split again after each \r\n, \r or \n (feedparser's universal newlines: a bare CR inside
## a line ends a header there); the first line that is not a header ends the block. A value
## loses its leading spaces and tabs and its trailing \r and \n, not its trailing spaces; a
## continuation line (leading space or tab) is appended to the header before it, line ends and
## all (so a folded value matches nothing), and is skipped with no header before it; a line with
## an empty name or an envelope `From ` line is skipped; the first of a repeated name counts.
## As local_server.ts's pyHeaders.
static func _headers(lines: Array[String]) -> Dictionary:
	var out := {}
	var parts: Array[String] = []  # the header being read: its first line, then continuations
	for line in _universal_lines(lines):
		if line.begins_with(" ") or line.begins_with("\t"):
			if not parts.is_empty():
				parts.append(line)
			continue
		var colon := line.find(":")
		if not line.begins_with("From ") and (colon < 0 or not _header_name(line.left(colon))):
			break
		_finish_header(out, parts)
		parts = []
		if not line.begins_with("From ") and colon > 0:
			parts = [line]
	_finish_header(out, parts)
	return out


## Adds the header `parts` hold (its first line, then its continuation lines) unless its name is
## already in `out`.
static func _finish_header(out: Dictionary, parts: Array[String]) -> void:
	if parts.is_empty():
		return
	var first := parts[0]
	var colon := first.find(":")
	var name := first.left(colon).to_lower()
	if out.has(name):
		return
	var value := first.substr(colon + 1).lstrip(" \t")
	for i in range(1, parts.size()):
		value += parts[i]
	out[name] = value.rstrip("\r\n")


## `lines` (each read up to an LF, the LF removed) split after each \r\n, \r or \n of the
## block they make, line ends kept.
static func _universal_lines(lines: Array[String]) -> Array[String]:
	var out: Array[String] = []
	for line in lines:
		var start := 0
		while true:
			var cr := line.find("\r", start)
			if cr < 0 or cr == line.length() - 1:
				out.append(line.substr(start) + "\n")
				break
			out.append(line.substr(start, cr + 1 - start))
			start = cr + 1
	return out


static func _header_name(s: String) -> bool:
	for i in s.length():
		var ch := s.unicode_at(i)
		if ch < 0x21 or ch > 0x7e:
			return false
	return true


func _serve_get(c: _Conn) -> void:
	var sub := "index.html" if c.sub == "" else c.sub
	if ASSETS.has(sub):
		var body: Variant = load_asset.call(sub) if _usable(load_asset) else null
		if body is PackedByteArray:
			_send(c, 200, body, _CTYPES[sub])
		else:
			_send(c, 500, "asset missing".to_utf8_buffer())
	elif sub == "session.json":
		_json(c, 200, session_info)
	elif sub == "status":
		if _usable(get_status):
			_json(c, 200, get_status.call())
		else:
			_json(c, 503, {"error": "unavailable"})
	elif sub == "logo" and load_logo != null:
		var logo: Variant = load_logo.call() if _usable(load_logo) else null
		if (logo is Array and logo.size() == 2 and logo[0] is PackedByteArray and logo[1] is String
				and RASTER_TYPES.has(logo[1])):
			_send(c, 200, logo[0], logo[1])
		else:
			_not_found(c)
	else:
		_not_found(c)


func _on_body(c: _Conn, raw: PackedByteArray) -> void:
	c.complete = c.buf.size() == c.need
	var parsed := ScfgCoio.parse_body(raw)
	if not parsed[0] or not parsed[1] is Dictionary:
		_send(c, 400, "bad body".to_utf8_buffer())
		return
	var data: Variant = ScfgPy.from_json(parsed[1])
	if c.sub == "build":
		var account: Variant = ScfgPageMessages.parse_build_message(data)
		if account is ScfgErr:
			_send(c, 400, "bad account".to_utf8_buffer())
			return
		var job: Variant = build_tx.call(account) if _usable(build_tx) else null
		if not job is ScfgJob:  # build_tx failed (an ScfgErr), or is gone
			_json(c, 503, {"error": "unavailable"})
			return
		c.job = job
		c.state = _State.BUILDING
		c.deadline = _now() + build_timeout_s
		_check_build(c)
		return
	var hint: Variant = ScfgPageMessages.parse_tx_message(data)
	if hint is ScfgErr:
		_send(c, 400, "bad tx hash".to_utf8_buffer())
		return
	if not _usable(on_tx):
		_json(c, 503, {"error": "unavailable"})
		return
	on_tx.call(hint[0], hint[1])
	if c.state != _State.CLOSED:  # (on_tx may have stopped the server)
		_json(c, 202, {"ok": true})


## The held /build connection: answered once its job is done, 503 (and the job cancelled) after
## build_timeout_s. A failed or cancelled job, or a result that is not an object, is a 503.
func _check_build(c: _Conn) -> void:
	if c.job.done():
		var failure: Variant = c.job.exception()  # err-ok: the job's failure, answered as a 503
		var value: Variant = c.job.result() if failure == null else null  # err-ok: exception() is null, so this is the value
		if value is Dictionary:
			_json(c, 200, value)
		else:
			_json(c, 503, {"error": "unavailable"})
	elif _now() >= c.deadline:
		c.job.cancel()  # don't build it later for a page that stopped waiting
		_json(c, 503, {"error": "unavailable"})


func _not_found(c: _Conn) -> void:
	_send(c, 404, "not found".to_utf8_buffer())


func _json(c: _Conn, code: int, value: Variant) -> void:
	_send(c, code, JSON.stringify(value, "", false).to_utf8_buffer(), "application/json")


func _send(c: _Conn, code: int, body: PackedByteArray,
		ctype: String = "text/plain; charset=utf-8", head_only: bool = false) -> void:
	if c.state == _State.CLOSED:
		return
	var head := ("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
		+ "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
		+ "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n") % [
			code, _REASONS.get(code, "Unknown"), ctype, body.size()]
	c.out = head.to_ascii_buffer()
	if not head_only:
		c.out.append_array(body)
	c.sent = 0
	c.buf = PackedByteArray()
	c.state = _State.WRITING
	c.deadline = _now() + DEADLINE_S
	_write(c)


## Sends what the socket takes now (at most WRITE_CHUNK * WRITE_CHUNKS bytes); the rest waits
## for the next pump.
func _write(c: _Conn) -> void:
	if _now() >= c.deadline:
		_close(c)
		return
	for i in WRITE_CHUNKS:
		var chunk := c.out.slice(c.sent, c.sent + WRITE_CHUNK)
		var got: Array = c.peer.put_partial_data(chunk)
		if got[0] != OK:
			_close(c)
			return
		c.sent += got[1]
		if got[1] < chunk.size():
			break  # the socket is full
	if c.sent >= c.out.size():
		c.out = PackedByteArray()
		c.peer.poll()
		# (poll() closes the socket when the client has closed its side: never read it then)
		if c.peer.get_status() != StreamPeerTCP.STATUS_CONNECTED \
				or (c.complete and c.peer.get_available_bytes() == 0):
			_close(c)
			return
		c.state = _State.LINGER
		c.deadline = minf(c.deadline, _now() + LINGER_S)


## The answer is out: read and discard what the client still sends until it closes (or LINGER_S
## or LINGER_MAX_BYTES pass), so closing does not reset the connection under the answer.
func _linger(c: _Conn) -> void:
	c.peer.poll()
	if c.peer.get_status() != StreamPeerTCP.STATUS_CONNECTED or _now() >= c.deadline:
		_close(c)
		return
	var n := mini(c.peer.get_available_bytes(), LINGER_MAX_BYTES - c.drained)
	if n > 0:
		c.peer.get_partial_data(n)
		c.drained += n
	if c.drained >= LINGER_MAX_BYTES:
		_close(c)


func _close(c: _Conn) -> void:
	if c.state != _State.CLOSED:
		c.state = _State.CLOSED
		c.peer.disconnect_from_host()


static func _usable(f: Variant) -> bool:
	return f is Callable and (f as Callable).is_valid()


## Bytes as Python's str(b, "iso-8859-1"), except NUL, which a String cannot hold (U+FFFD instead:
## it still matches nothing).
static func _latin1(b: PackedByteArray) -> String:
	var parts := PackedStringArray()
	parts.resize(b.size())
	for i in b.size():
		parts[i] = String.chr(b[i]) if b[i] != 0 else "\uFFFD"
	return "".join(parts)


## Python's str.split() with no argument.
static func _py_split(s: String) -> PackedStringArray:
	var words := PackedStringArray()
	var start := -1
	for i in s.length():
		if ScfgPy.is_py_space(s.unicode_at(i)):
			if start >= 0:
				words.append(s.substr(start, i - start))
				start = -1
		elif start < 0:
			start = i
	if start >= 0:
		words.append(s.substr(start))
	return words


static func _ascii_digits(s: String) -> bool:
	for i in s.length():
		var ch := s.unicode_at(i)
		if ch < 0x30 or ch > 0x39:
			return false
	return true
