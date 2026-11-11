## The real transport (see ScfgCoio for the contract): JSON POSTs with Godot's HTTPRequest.
## Mirrors scfg_core/rpc.py's UrllibTransport (desktop) and the TS core's FetchTransport (web).
##
## `ScfgHttpTransport.new(host)` takes a Node that is inside the scene tree (the plugin's
## autoload, or a test's node under the SceneTree root). Each post_json adds one HTTPRequest
## child to it, awaits the answer and frees the child. HTTPRequest is processed by the tree, so
## the coroutine resumes on a later frame; it never blocks the game. In a web export HTTPRequest
## uses the browser's fetch(), so the same code runs there.
##
## Headers: `Content-Type: application/json`, plus `User-Agent: scfg/0.1` off the web (urllib's
## header; a browser fetch keeps its own User-Agent, and an extra header there could only fail a
## CORS preflight). `timeout` is HTTPRequest.timeout: the whole request, in seconds.
##
## Results follow ScfgCoio.response_for (rpc.UrllibTransport's rules). A request that gets no
## HTTP answer (refused, unresolvable, TLS, timeout) is a TransportError without a status. The
## body is decoded as strict UTF-8 (ScfgCoio.parse_rpc_body).
##
## THE HOST GOING AWAY. When the host is not in the tree (never added, removed, freed or queued
## for deletion), post_json returns an ScfgErr of kind HostGone at once and adds nothing to it.
## When the host leaves the tree or is freed while a request is in flight, the waiting caller
## resumes with HostGone too, but only after the removal has finished (the HTTPRequest's
## tree_exiting is handled deferred): resuming inside the exit-tree notification would let the
## caller start its next request on a host that is busy removing its children. HostGone is not a
## TransportError, so ScfgRpcClient propagates it instead of putting a healthy endpoint in
## cooldown. A request abandoned by a removed (not freed) host is cancelled and freed.
##
## CLOSING. `close()` is for the host going away for good (the game quitting): every request in
## flight ends with HostGone at once, its caller resuming inside close(), and every later
## post_json returns HostGone without touching the host. At a real quit the tree is torn down
## with no further frame, so the deferred tree_exiting above never runs: without close() a caller
## left waiting would never resume, and its suspended coroutines would be reported as leaked
## ("ObjectDB instances leaked at exit").
##
## Port limits: Godot's JSON parser accepts a few things Python's json.loads rejects (a trailing
## comma, raw control characters in strings), and reads every number as a float (see ScfgPy).
## Python's urllib raises ValueError for a malformed URL; HTTPRequest's refusal is a
## TransportError here (configured urls are validated, so neither is reachable from a config).
class_name ScfgHttpTransport
extends ScfgCoio.Transport

const USER_AGENT := "scfg/0.1"

var host: Node
var _closed := false
var _waiting: Array = []  # the _Waiters of the requests in flight


func _init(host_: Node) -> void:
	host = host_


## Ends every request in flight with HostGone now and refuses later ones (see CLOSING above).
func close() -> void:
	_closed = true
	var waiting := _waiting
	_waiting = []
	for waiter: Variant in waiting:
		(waiter as _Waiter).left_tree()


## One finished request, whichever comes first: HTTPRequest's request_completed, or the
## HTTPRequest leaving the tree before it (its host was removed or freed), which would otherwise
## leave the caller suspended forever. `left_tree` is connected deferred (see the header).
class _Waiter:
	extends RefCounted
	signal finished(result: Array)
	var done := false

	func complete(result: int, code: int, headers: PackedStringArray, body: PackedByteArray) -> void:
		if not done:
			done = true
			finished.emit([result, code, headers, body])

	func left_tree() -> void:
		if not done:
			done = true
			finished.emit([-1, 0, PackedStringArray(), PackedByteArray()])


## The HostGone error (Godot only, see the header): the host Node is not there to carry a request.
## @fallible
static func host_gone(url: String) -> Variant:
	return ScfgErr.make("HostGone", "the transport's host node is not in the scene tree (request to %s)" % url)


## @fallible
func post_json(url: String, payload: Dictionary, timeout: float) -> Variant:
	if _closed or host == null or not is_instance_valid(host) or not host.is_inside_tree() or host.is_queued_for_deletion():
		return host_gone(url)
	var req := HTTPRequest.new()
	req.timeout = timeout
	var waiter := _Waiter.new()
	req.request_completed.connect(waiter.complete)
	req.tree_exiting.connect(waiter.left_tree, CONNECT_DEFERRED)
	host.add_child(req)
	var headers := PackedStringArray(["Content-Type: application/json"])
	if not OS.has_feature("web"):
		headers.append("User-Agent: %s" % USER_AGENT)
	var started := req.request(url, headers, HTTPClient.METHOD_POST, JSON.stringify(payload))
	if started != OK:
		waiter.done = true
		req.tree_exiting.disconnect(waiter.left_tree)
		req.queue_free()
		return ScfgCoio.transport_error("cannot start the request to %s: %s" % [url, error_string(started)])
	_waiting.append(waiter)
	var got: Array = await waiter.finished
	_waiting.erase(waiter)
	var result: int = got[0]
	if result == -1:
		# The host left the tree (or was freed with the request), or close() ended the request. A
		# host still holding the HTTPRequest: stop it and free it, so it never answers later.
		if is_instance_valid(req):
			req.cancel_request()
			req.queue_free()
		return host_gone(url)
	req.tree_exiting.disconnect(waiter.left_tree)
	req.queue_free()
	if result != HTTPRequest.RESULT_SUCCESS:
		return ScfgCoio.transport_error(_result_message(result))
	return ScfgCoio.response_for(got[1], ScfgCoio.parse_rpc_body(got[3]))


static func _result_message(result: int) -> String:
	match result:
		HTTPRequest.RESULT_TIMEOUT:
			return "timed out"
		HTTPRequest.RESULT_CANT_CONNECT:
			return "connection failed"
		HTTPRequest.RESULT_CANT_RESOLVE:
			return "name resolution failed"
		HTTPRequest.RESULT_CONNECTION_ERROR:
			return "connection error"
		HTTPRequest.RESULT_TLS_HANDSHAKE_ERROR:
			return "TLS handshake failed"
		HTTPRequest.RESULT_NO_RESPONSE:
			return "no response"
		HTTPRequest.RESULT_BODY_SIZE_LIMIT_EXCEEDED:
			return "response too large"
		HTTPRequest.RESULT_REDIRECT_LIMIT_REACHED:
			return "too many redirects"
	return "request failed (HTTPRequest result %d)" % result
