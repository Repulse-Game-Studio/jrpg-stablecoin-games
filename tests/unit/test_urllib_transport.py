import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from scfg_core.rpc import TransportError, UrllibTransport


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/500":
            self.send_response(500)
            self.end_headers()
            return
        if self.path == "/400-jsonrpc":
            payload = json.dumps({"jsonrpc": "2.0", "id": body["id"],
                                  "error": {"code": 35, "message": "ranges over 10000 blocks"}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(payload)
            return
        if self.path == "/400-long-int":
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error": {"code": ' + b"9" * 4301 + b', "message": "x"}}')
            return
        if self.path == "/garbage":
            payload = b"<html>not json</html>"
        elif self.path == "/long-int":
            payload = b'{"jsonrpc": "2.0", "id": 1, "result": ' + b"9" * 4301 + b"}"
        else:
            payload = json.dumps({"jsonrpc": "2.0", "id": body["id"], "result": body["method"],
                                  "ua": self.headers.get("User-Agent"),
                                  "ct": self.headers.get("Content-Type")}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@pytest.fixture(scope="module")
def base_url():
    server = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_address[1]}"
    server.shutdown()


def test_posts_json_and_parses_response(base_url):
    resp = UrllibTransport().post_json(base_url + "/", {"jsonrpc": "2.0", "id": 7,
                                                         "method": "eth_chainId", "params": []}, 5)
    assert resp["result"] == "eth_chainId"
    assert resp["ct"] == "application/json"
    assert resp["ua"].startswith("scfg/")


def test_http_error_raises_with_status(base_url):
    with pytest.raises(TransportError) as info:
        UrllibTransport().post_json(base_url + "/500", {"id": 1, "method": "x"}, 5)
    assert info.value.status == 500


def test_non_json_raises(base_url):
    with pytest.raises(TransportError):
        UrllibTransport().post_json(base_url + "/garbage", {"id": 1, "method": "x"}, 5)


def test_connection_refused_raises():
    with pytest.raises(TransportError) as info:
        UrllibTransport().post_json("http://127.0.0.1:9/", {"id": 1, "method": "x"}, 2)
    assert info.value.status is None


def test_http_error_with_jsonrpc_body_is_returned_not_raised(base_url):
    resp = UrllibTransport().post_json(base_url + "/400-jsonrpc", {"id": 3, "method": "x"}, 5)
    assert resp["error"]["code"] == 35


def test_an_overlong_json_integer_is_invalid_json(base_url, interpreter):
    """More than 4300 digits in an integer literal is not JSON on any interpreter."""
    with pytest.raises(TransportError, match="invalid JSON response"):
        UrllibTransport().post_json(base_url + "/long-int", {"id": 1, "method": "x"}, 5)
    with pytest.raises(TransportError) as info:
        UrllibTransport().post_json(base_url + "/400-long-int", {"id": 1, "method": "x"}, 5)
    assert info.value.status == 400


PROXY_VARS = ("http_proxy", "https_proxy", "all_proxy", "no_proxy",
              "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY")


@pytest.fixture
def no_proxy(monkeypatch):
    """No proxy from the environment (getproxies reads only the environment on Linux)."""
    for name in PROXY_VARS:
        monkeypatch.delenv(name, raising=False)


def test_a_name_lookup_that_never_answers_is_bounded_by_the_timeout(monkeypatch, no_proxy):
    """With a DNS server that never answers, getaddrinfo waits for the resolver's own timeout
    (about 20 s per endpoint measured in the Unity testbed): the request's timeout must cover it."""
    import socket
    import threading
    import time

    from scfg_core.rpc import TransportError, UrllibTransport

    release = threading.Event()

    def stuck(*args, **kwargs):
        release.wait(30)
        raise OSError("released")
    monkeypatch.setattr(socket, "getaddrinfo", stuck)
    t = time.monotonic()
    try:
        with pytest.raises(TransportError, match="timed out resolving rpc.example"):
            UrllibTransport().post_json("https://rpc.example/", {"id": 1}, 0.3)
        assert time.monotonic() - t < 2.0
    finally:
        release.set()


def test_a_literal_address_needs_no_lookup(monkeypatch):
    import socket

    from scfg_core.rpc import resolve_within

    monkeypatch.setattr(socket, "getaddrinfo", lambda *a, **k: pytest.fail("looked up"))
    resolve_within("http://127.0.0.1:8545/", 0.1)
    resolve_within("http://[::1]:8545/", 0.1)


class _ConnectProxy(threading.Thread):
    """A tiny HTTP proxy: records the first request line (a CONNECT) and refuses the tunnel."""

    def __init__(self):
        import socket
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.lines = []

    def run(self):
        conn, _ = self.sock.accept()
        with conn:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            self.lines.append(data.split(b"\r\n", 1)[0].decode())
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")


def test_behind_a_proxy_the_name_is_left_to_the_proxy(monkeypatch, no_proxy):
    """Review T1: on a proxy-only network this machine cannot resolve the RPC host, the proxy
    does. The request must reach the proxy (a CONNECT for the host), not fail on a local lookup."""
    import socket

    real = socket.getaddrinfo

    def no_dns(host, *args, **kwargs):
        if host == "rpc.unresolvable.example":
            raise socket.gaierror(socket.EAI_NONAME, "Name or service not known")
        return real(host, *args, **kwargs)
    monkeypatch.setattr(socket, "getaddrinfo", no_dns)
    proxy = _ConnectProxy()
    proxy.start()
    monkeypatch.setenv("https_proxy", f"http://127.0.0.1:{proxy.port}")
    with pytest.raises(TransportError):  # the proxy refuses the tunnel (502)
        UrllibTransport().post_json("https://rpc.unresolvable.example/", {"id": 1}, 5)
    proxy.join(5)
    assert len(proxy.lines) == 1 and proxy.lines[0].startswith("CONNECT rpc.unresolvable.example:443 ")


def test_a_lookup_error_is_left_to_urlopen(monkeypatch, no_proxy):
    """Only a lookup that does not answer in time fails the request early: an error is urlopen's
    to meet (it looks the name up again, and fails or succeeds by itself)."""
    import socket

    calls = []

    def unknown(host, *args, **kwargs):
        calls.append(host)
        raise socket.gaierror(socket.EAI_NONAME, "Name or service not known")
    monkeypatch.setattr(socket, "getaddrinfo", unknown)
    with pytest.raises(TransportError, match="Name or service not known"):
        UrllibTransport().post_json("https://rpc.example/", {"id": 1}, 2)
    assert calls == ["rpc.example", "rpc.example"], "urlopen tried after the lookup failed"


def test_the_lookup_counts_inside_the_timeout(monkeypatch, no_proxy):
    """Review T6: the lookup and the request share one timeout; urlopen gets what is left."""
    import socket
    import time
    import urllib.error
    import urllib.request

    real = socket.getaddrinfo

    def slow(host, *args, **kwargs):
        time.sleep(0.4)
        return real("127.0.0.1", *args, **kwargs)
    monkeypatch.setattr(socket, "getaddrinfo", slow)
    given = []

    def urlopen(req, timeout, context):
        given.append(timeout)
        raise urllib.error.URLError("stop")
    monkeypatch.setattr(urllib.request, "urlopen", urlopen)
    with pytest.raises(TransportError):
        UrllibTransport().post_json("https://rpc.example/", {"id": 1}, 1.0)
    assert len(given) == 1 and 0 < given[0] <= 0.65, given
