import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from cryptopay_core.rpc import TransportError, UrllibTransport


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
        if self.path == "/garbage":
            payload = b"<html>not json</html>"
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
    assert resp["ua"].startswith("cryptopay/")


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
