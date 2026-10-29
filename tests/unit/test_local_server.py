import http.client
import json

import pytest

from scfg_core.local_server import MAX_BODY, CheckoutServer

TX = "0x" + "ab" * 32


@pytest.fixture
def server():
    hints = []
    srv = CheckoutServer(
        load_asset=lambda name: {"index.html": b"<html>page</html>", "app.js": b"// js"}[name],
        session_info={"chain_id": 42161, "amount_units": "4993742"},
        on_tx=lambda tx, sender: hints.append((tx, sender)),
        get_status=lambda: {"state": "waiting"},
    )
    srv.start()
    srv.hints = hints
    yield srv
    srv.stop()


def request(srv, method, path, body=None, host=None):
    conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=5)
    headers = {"Host": host or f"127.0.0.1:{srv.port}"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp, data


def base(srv):
    return f"/c/{srv.secret}/"


def test_url_shape(server):
    assert server.url == f"http://127.0.0.1:{server.port}/c/{server.secret}/"
    assert len(server.secret) >= 20


def test_serves_page_and_assets(server):
    resp, data = request(server, "GET", base(server))
    assert resp.status == 200 and data == b"<html>page</html>"
    assert resp.getheader("Content-Type").startswith("text/html")
    assert resp.getheader("Referrer-Policy") == "no-referrer"
    assert resp.getheader("Access-Control-Allow-Origin") is None
    resp, _ = request(server, "GET", base(server) + "app.js")
    assert resp.status == 200 and "javascript" in resp.getheader("Content-Type")


def test_session_and_status_json(server):
    _, data = request(server, "GET", base(server) + "session.json")
    assert json.loads(data)["chain_id"] == 42161
    _, data = request(server, "GET", base(server) + "status")
    assert json.loads(data) == {"state": "waiting"}


@pytest.mark.parametrize("path", ["/", "/c/wrong-secret/", "/c//session.json", "/index.html"])
def test_wrong_or_missing_secret_is_404(server, path):
    assert request(server, "GET", path)[0].status == 404


def test_unknown_file_is_404(server):
    assert request(server, "GET", base(server) + "../etc/passwd")[0].status == 404


@pytest.mark.parametrize("host", ["evil.example:80", "localhost", "127.0.0.1"])
def test_wrong_host_header_is_403(server, host):
    assert request(server, "GET", base(server), host=host)[0].status == 403


def test_post_tx_hint(server):
    body = json.dumps({"tx_hash": TX.upper().replace("0X", "0x"),
                       "from": "0x52908400098527886e0f7030069857d2e4169ee7"})
    resp, _ = request(server, "POST", base(server) + "tx", body)
    assert resp.status == 202
    assert server.hints == [(TX, "0x52908400098527886E0F7030069857D2E4169EE7")]


@pytest.mark.parametrize("body", ['{"tx_hash": "0x123"}', "not json", "{}", '{"tx_hash": 5}'])
def test_post_bad_body_is_400(server, body):
    assert request(server, "POST", base(server) + "tx", body)[0].status == 400
    assert server.hints == []


def test_post_oversized_body_is_rejected(server):
    resp, _ = request(server, "POST", base(server) + "tx", "x" * (MAX_BODY + 1))
    assert resp.status == 413


def test_invalid_sender_is_dropped_but_hint_kept(server):
    request(server, "POST", base(server) + "tx", json.dumps({"tx_hash": TX, "from": "nonsense"}))
    assert server.hints == [(TX, None)]


def test_stop_releases_port():
    srv = CheckoutServer(load_asset=lambda n: b"", session_info={}, on_tx=lambda *a: None,
                         get_status=dict)
    srv.start()
    port = srv.port
    srv.stop()
    assert not srv.running
    with pytest.raises(OSError):
        http.client.HTTPConnection("127.0.0.1", port, timeout=1).request("GET", "/")


def test_logo_is_served_when_configured():
    srv = CheckoutServer(load_asset=lambda n: b"", session_info={}, on_tx=lambda *a: None,
                         get_status=dict, load_logo=lambda: (b"\x89PNG...", "image/png"))
    srv.start()
    try:
        resp, data = request(srv, "GET", base(srv) + "logo")
        assert resp.status == 200 and data == b"\x89PNG..."
        assert resp.getheader("Content-Type") == "image/png"
        assert resp.getheader("X-Content-Type-Options") == "nosniff"
    finally:
        srv.stop()


def test_logo_is_404_without_one(server):
    assert request(server, "GET", base(server) + "logo")[0].status == 404


def test_unreadable_logo_is_404():
    def broken():
        raise OSError("gone")
    srv = CheckoutServer(load_asset=lambda n: b"", session_info={}, on_tx=lambda *a: None,
                         get_status=dict, load_logo=broken)
    srv.start()
    try:
        assert request(srv, "GET", base(srv) + "logo")[0].status == 404
    finally:
        srv.stop()


@pytest.mark.parametrize("name", ["evm.js", "solana.js"])
def test_family_scripts_are_served(name):
    srv = CheckoutServer(load_asset=lambda n: b"// " + n.encode(), session_info={},
                         on_tx=lambda tx, sender: None, get_status=lambda: {})
    srv.start()
    try:
        resp, body = request(srv, "GET", base(srv) + name)
    finally:
        srv.stop()
    assert resp.status == 200 and body == b"// " + name.encode()


def test_build_that_never_finishes_answers_503_and_is_cancelled(monkeypatch):
    from concurrent.futures import Future

    monkeypatch.setattr("scfg_core.local_server.BUILD_TIMEOUT_S", 0.05)
    never = Future()
    srv = CheckoutServer(
        load_asset=lambda name: b"", session_info={}, on_tx=lambda tx, sender: None,
        get_status=lambda: {}, build_tx=lambda account: never,
    )
    srv.start()
    try:
        body = json.dumps({"account": "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"})
        resp, data = request(srv, "POST", base(srv) + "build", body)
    finally:
        srv.stop()
    assert resp.status == 503 and json.loads(data) == {"error": "unavailable"}
    assert never.cancelled()
