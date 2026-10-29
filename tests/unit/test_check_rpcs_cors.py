import importlib.util
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "check_rpcs.py"
spec = importlib.util.spec_from_file_location("check_rpcs", TOOL)
check_rpcs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_rpcs)


def serve(allow_preflight, allow_post, preflight_status=204, allow_headers="content-type"):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_OPTIONS(self):
            self.send_response(preflight_status)
            if allow_preflight:
                self.send_header("Access-Control-Allow-Origin", allow_preflight)
            if allow_headers is not None:
                self.send_header("Access-Control-Allow-Headers", allow_headers)
            self.end_headers()

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            body = b'{"jsonrpc":"2.0","id":1,"result":"0x1"}'
            self.send_response(200)
            if allow_post:
                self.send_header("Access-Control-Allow-Origin", allow_post)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, f"http://127.0.0.1:{httpd.server_address[1]}/"


def serve_junk(method):
    """Answers `method` with bytes that are not an HTTP response at all; the other
    method is answered properly so the check reaches it."""
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_OPTIONS(self):
            if method == "OPTIONS":
                self.wfile.write(b"garbage\r\n\r\n")
                return
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "content-type")
            self.end_headers()

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if method == "POST":
                self.wfile.write(b"garbage\r\n\r\n")
                return
            body = b'{"jsonrpc":"2.0","id":1,"result":"0x1"}'
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, f"http://127.0.0.1:{httpd.server_address[1]}/"


@pytest.mark.parametrize("pre, post, ok", [
    ("*", "*", True),
    (check_rpcs.WEB_ORIGIN, "*", True),
    ("*", None, False),
    (None, "*", False),
    ("https://other.example", "*", False),
])
def test_check_cors(pre, post, ok):
    httpd, url = serve(pre, post)
    try:
        assert check_rpcs.check_cors(url) is ok
    finally:
        httpd.shutdown()
        httpd.server_close()


@pytest.mark.parametrize("kwargs", [
    {"preflight_status": 403},                  # rejected preflight, even with ACAO
    {"preflight_status": 500},
    {"allow_headers": None},                    # ACAO but no Allow-Headers
    {"allow_headers": "X-Foo"},                 # Allow-Headers does not permit content-type
])
def test_preflight_rejected(kwargs):
    httpd, url = serve("*", "*", **kwargs)
    try:
        assert check_rpcs.check_cors(url) is False
    finally:
        httpd.shutdown()
        httpd.server_close()


@pytest.mark.parametrize("allow_headers", ["Content-Type, X-Foo", "*", "x-foo, CONTENT-TYPE"])
def test_preflight_headers_accepted(allow_headers):
    httpd, url = serve("*", "*", allow_headers=allow_headers)
    try:
        assert check_rpcs.check_cors(url) is True
    finally:
        httpd.shutdown()
        httpd.server_close()


@pytest.mark.parametrize("method", ["OPTIONS", "POST"])
def test_malformed_response_is_not_web_ready(method):
    httpd, url = serve_junk(method)
    try:
        assert check_rpcs.check_cors(url, timeout=2) is False
    finally:
        httpd.shutdown()
        httpd.server_close()


def test_unreachable_endpoint_is_not_web_ready():
    assert check_rpcs.check_cors("http://127.0.0.1:9/", timeout=2) is False
