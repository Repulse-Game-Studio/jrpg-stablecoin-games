"""The localhost checkout server's behaviour and hardening, over real HTTP, against both the Python
server and the GDScript one (the `server` / `make_server` fixtures in conftest.py run each test
twice). Ports every test of tests/unit/test_local_server.py, plus
test_solana_build.py::test_build_route and test_checkout_solana.py::
test_local_server_accepts_a_signature_hint, under their own names."""
import http.client
import json
import socket
import time

import pytest
from solders.keypair import Keypair

from scfg_core.local_server import MAX_BODY
from scfg_core.solana.base58 import b58encode

from server_handles import GodotServer

TX = "0x" + "ab" * 32
SIG = b58encode((7).to_bytes(64, "big"))
DEV = str(Keypair().pubkey())
PLAYER = str(Keypair().pubkey())
SECURITY_HEADERS = {"Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
                    "X-Content-Type-Options": "nosniff"}


def request(srv, method, path, body=None, host=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
    hdrs = {"Host": host or f"127.0.0.1:{srv.port}"}
    if body is not None:
        hdrs["Content-Type"] = "application/json"
    hdrs.update(headers or {})
    conn.request(method, path, body=body, headers=hdrs)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp, data


def raw(srv, data, timeout=10):
    """Send `data` as is; return (status, {lowercase header: value}, body) of the answer, or None
    when the server closed without one."""
    with socket.create_connection(("127.0.0.1", srv.port), timeout=timeout) as s:
        s.sendall(data)
        out = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    if not out:
        return None
    head, _, body = out.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers.setdefault(name.strip().lower(), value.strip())
    return int(lines[0].split()[1]), headers, body


def base(srv):
    return f"/c/{srv.secret}/"


def host_line(srv):
    return f"Host: 127.0.0.1:{srv.port}\r\n".encode()


# --- tests/unit/test_local_server.py -------------------------------------------------------


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
    assert server.calls() == [["on_tx", [TX, "0x52908400098527886E0F7030069857D2E4169EE7"]]]


@pytest.mark.parametrize("body", ['{"tx_hash": "0x123"}', "not json", "{}", '{"tx_hash": 5}'])
def test_post_bad_body_is_400(server, body):
    assert request(server, "POST", base(server) + "tx", body)[0].status == 400
    assert server.calls() == []


def test_post_oversized_body_is_rejected(server):
    resp, _ = request(server, "POST", base(server) + "tx", "x" * (MAX_BODY + 1))
    assert resp.status == 413


def test_invalid_sender_is_dropped_but_hint_kept(server):
    request(server, "POST", base(server) + "tx", json.dumps({"tx_hash": TX, "from": "nonsense"}))
    assert server.calls() == [["on_tx", [TX, None]]]


def test_stop_releases_port(make_server):
    srv = make_server(assets={}, session_info={}, status={})
    port = srv.port
    srv.stop()
    assert not srv.running
    with pytest.raises(OSError):
        http.client.HTTPConnection("127.0.0.1", port, timeout=1).request("GET", "/")


def test_logo_is_served_when_configured(make_server):
    srv = make_server(logo=(b"\x89PNG...", "image/png"))
    resp, data = request(srv, "GET", base(srv) + "logo")
    assert resp.status == 200 and data == b"\x89PNG..."
    assert resp.getheader("Content-Type") == "image/png"
    assert resp.getheader("X-Content-Type-Options") == "nosniff"


def test_logo_is_404_without_one(server):
    assert request(server, "GET", base(server) + "logo")[0].status == 404


def test_unreadable_logo_is_404(make_server):
    srv = make_server(logo="broken")
    assert request(srv, "GET", base(srv) + "logo")[0].status == 404


@pytest.mark.parametrize("name", ["evm.js", "solana.js"])
def test_family_scripts_are_served(server, name):
    resp, body = request(server, "GET", base(server) + name)
    assert resp.status == 200 and body == b"// " + name.encode()


def test_build_that_never_finishes_answers_503_and_is_cancelled(make_server):
    srv = make_server(build={"mode": "never"}, build_timeout_s=0.05)
    body = json.dumps({"account": "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"})
    resp, data = request(srv, "POST", base(srv) + "build", body)
    assert resp.status == 503 and json.loads(data) == {"error": "unavailable"}
    assert srv.jobs() == [{"done": True, "cancelled": True}]


# --- test_solana_build.py::test_build_route, test_checkout_solana.py ------------------------


@pytest.mark.parametrize("build, account, status", [
    (None, PLAYER, 404),
    ({"mode": "ok", "value": {"transaction": "AA=="}}, "0xnope", 400),
    ({"mode": "fail"}, PLAYER, 503),
    ({"mode": "raise"}, PLAYER, 503),
    ({"mode": "ok", "value": {"transaction": "AA==", "token_balance": "1", "sol_balance": "2"}},
     PLAYER, 200),
])
def test_build_route(make_server, build, account, status):
    srv = make_server(build=build, session_info={}, status={})
    resp, raw_body = request(srv, "POST", base(srv) + "build", json.dumps({"account": account}))
    assert resp.status == status
    if status == 200:
        assert json.loads(raw_body)["transaction"] == "AA=="
        assert json.loads(raw_body) == build["value"]
        assert srv.calls() == [["build_tx", [PLAYER]]]


def test_local_server_accepts_a_signature_hint(make_server):
    srv = make_server(assets={}, session_info={}, status={})
    resp, _ = request(srv, "POST", base(srv) + "tx", json.dumps({"tx_hash": SIG, "from": DEV}))
    assert resp.status == 202
    assert srv.calls() == [["on_tx", [SIG, DEV]]]


# --- hardening (Review Focus 3) -------------------------------------------------------------


def test_secret_is_128_bit_urlsafe(server):
    assert len(server.secret) == 22  # secrets.token_urlsafe(16): 16 bytes, base64url, no padding
    assert set(server.secret) <= set(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")


def test_every_answer_carries_the_security_headers_and_no_cors(make_server):
    srv = make_server(logo=(b"\x89PNG", "image/png"),
                      build={"mode": "ok", "value": {"transaction": "AA=="}})
    b = base(srv)
    answers = [
        request(srv, "GET", b), request(srv, "GET", b + "app.js"),
        request(srv, "GET", b + "session.json"), request(srv, "GET", b + "status"),
        request(srv, "GET", b + "logo"), request(srv, "GET", b + "nope"),
        request(srv, "GET", "/"), request(srv, "GET", b, host="evil.example"),
        request(srv, "POST", b + "tx", json.dumps({"tx_hash": TX})),
        request(srv, "POST", b + "tx", "{}"),
        request(srv, "POST", b + "tx", "x" * (MAX_BODY + 1)),
        request(srv, "POST", b + "build", json.dumps({"account": PLAYER})),
        request(srv, "OPTIONS", b, headers={"Origin": "http://evil.example",
                                           "Access-Control-Request-Method": "POST"}),
    ]
    assert [r.status for r, _ in answers] == [200, 200, 200, 200, 200, 404, 404, 403, 202, 400,
                                              413, 200, 501]
    for resp, _ in answers:
        assert not [h for h, _ in resp.getheaders() if h.lower().startswith("access-control-")]
    # Python's 501 comes from BaseHTTPRequestHandler.send_error, which adds none of them (a gap
    # of scfg_core's server, kept as it is here); the GDScript server sends them on every answer
    checked = answers if isinstance(srv, GodotServer) else answers[:-1]
    for resp, _ in checked:
        for name, value in SECURITY_HEADERS.items():
            assert resp.getheader(name) == value, (resp.status, name)


def test_assets_carry_charset_utf8(server):
    resp, _ = request(server, "GET", base(server) + "index.html")
    assert resp.getheader("Content-Type") == "text/html; charset=utf-8"
    for name in ("app.js", "evm.js", "solana.js"):
        resp, _ = request(server, "GET", base(server) + name)
        ctype = resp.getheader("Content-Type")
        assert ctype.endswith("javascript; charset=utf-8"), (name, ctype)
    resp, data = request(server, "GET", base(server) + "session.json")
    assert resp.getheader("Content-Type") == "application/json"
    assert resp.getheader("Content-Length") == str(len(data))


def test_missing_asset_is_500(make_server):
    srv = make_server(assets={"index.html": b"<html>page</html>"})
    resp, data = request(srv, "GET", base(srv) + "solana.js")
    assert resp.status == 500 and data == b"asset missing"


def test_query_string_is_ignored(server):
    resp, data = request(server, "GET", base(server) + "?x=1")
    assert resp.status == 200 and data == b"<html>page</html>"
    resp, data = request(server, "GET", base(server) + "session.json?cache=1")
    assert resp.status == 200 and json.loads(data)["chain_id"] == 42161


def test_leading_slashes_are_reduced_to_one(server):
    """http.server reduces a path starting with // to a single / (gh-87389)."""
    assert request(server, "GET", "//c/" + server.secret + "/")[0].status == 200
    assert request(server, "GET", "///c/" + server.secret + "/status")[0].status == 200


@pytest.mark.parametrize("path", ["/c/{s}/index.html/", "/c/{s}/INDEX.HTML", "/c/{s}//",
                                  "/c/{s}/%69ndex.html", "/c/{s}x/", "/C/{s}/",
                                  "http://127.0.0.1/c/{s}/"])
def test_near_miss_paths_are_404(server, path):
    assert request(server, "GET", path.format(s=server.secret))[0].status == 404


def test_missing_host_header_is_403(server):
    status, _, body = raw(server, f"GET {base(server)} HTTP/1.1\r\n\r\n".encode())
    assert (status, body) == (403, b"forbidden")


def test_host_header_is_matched_exactly(server):
    b = base(server).encode()
    port = str(server.port).encode()
    assert raw(server, b"GET " + b + b" HTTP/1.1\r\nHost: 127.0.0.1:" + port + b" \r\n\r\n")[0] == 403
    assert raw(server, b"GET " + b + b" HTTP/1.1\r\nHost:\t127.0.0.1:" + port + b"\r\n\r\n")[0] == 200
    assert raw(server, b"GET " + b + b" HTTP/1.1\r\nhOsT: 127.0.0.1:" + port + b"\r\n\r\n")[0] == 200
    # the first Host header counts
    assert raw(server, b"GET " + b + b" HTTP/1.1\r\nHost: 127.0.0.1:" + port
               + b"\r\nHost: evil.example\r\n\r\n")[0] == 200
    assert raw(server, b"GET " + b + b" HTTP/1.1\r\nHost: evil.example\r\nHost: 127.0.0.1:"
               + port + b"\r\n\r\n")[0] == 403


def test_secret_is_checked_after_the_host(server):
    assert request(server, "GET", "/c/wrong/", host="evil.example")[0].status == 403


@pytest.mark.parametrize("method", ["HEAD", "PUT", "DELETE", "OPTIONS", "PATCH", "get"])
def test_other_methods_are_501(server, method):
    resp, data = request(server, method, base(server))
    assert resp.status == 501
    if method == "HEAD":
        assert data == b""


def test_head_gets_no_body(server):
    status, _, body = raw(server, b"HEAD " + base(server).encode() + b" HTTP/1.1\r\n"
                          + host_line(server) + b"\r\n")
    assert status == 501 and body == b""


@pytest.mark.parametrize("line, status", [
    (b"GET {b} HTTP/2.0", 505),
    (b"GET {b} HTTP/1", 400),
    (b"GET {b} HTTP/x.1", 400),
    (b"GET {b} FTP/1.1", 400),
    (b"GET {b} extra HTTP/1.1", 400),
])
def test_bad_request_lines(server, line, status):
    request_line = line.replace(b"{b}", base(server).encode())
    with socket.create_connection(("127.0.0.1", server.port), timeout=10) as s:
        s.sendall(request_line + b"\r\n" + host_line(server) + b"\r\n")
        out = b""
        while chunk := s.recv(65536):
            out += chunk
    # http.server answers a request line it cannot read without a status line (as HTTP/0.9),
    # with only its error page; the GDScript server sends a full response
    if out.startswith(b"HTTP/"):
        assert int(out.split()[1]) == status
    else:
        assert b"Error code: %d" % status in out
    assert b"<html>page</html>" not in out


def test_header_count_limit_matches_http_client(server):
    # http.client._MAXHEADERS (100) counts the blank line that ends the head: 99 headers at most
    head = b"GET " + base(server).encode() + b" HTTP/1.1\r\n" + host_line(server)
    head += b"".join(b"X-%d: 1\r\n" % i for i in range(98))
    assert raw(server, head + b"\r\n")[0] == 200
    assert raw(server, head + b"X-98: 1\r\n\r\n")[0] == 431


def test_header_line_length_limit_matches_http_client(server):
    # http.client reads a header line with readline(_MAXLINE + 1): 65536 bytes with the line end
    # are read, one more is a 431 ("Line too long"). A browser can send 127.0.0.1 a Cookie
    # header that large. The too-long line is the last thing sent, so http.server reads all of it
    # and its close does not reset the connection.
    head = b"GET " + base(server).encode() + b" HTTP/1.1\r\n" + host_line(server)
    prefix = b"Cookie: c="
    fill = 65536 - len(prefix) - 2
    assert raw(server, head + prefix + b"a" * fill + b"\r\n\r\n")[0] == 200
    assert raw(server, head + prefix + b"a" * (fill + 1) + b"\r\n")[0] == 431


def test_request_line_length_limit_matches_http_server(server):
    # http.server reads the request line with readline(65537): 65536 bytes with the line end are
    # read (the query string is ignored), one more is a 414
    start = b"GET " + base(server).encode() + b"?q="
    end = b" HTTP/1.1\r\n"
    fill = 65536 - len(start) - len(end)
    assert raw(server, start + b"a" * fill + end + host_line(server) + b"\r\n")[0] == 200
    assert raw(server, start + b"a" * (fill + 1) + end)[0] == 414


def test_a_client_that_closes_its_side_after_the_request_gets_the_answer(server):
    # its FIN arrives with the request: the server must not read the socket it then closes
    with socket.create_connection(("127.0.0.1", server.port), timeout=10) as s:
        s.sendall(b"GET " + base(server).encode() + b" HTTP/1.1\r\n" + host_line(server) + b"\r\n")
        s.shutdown(socket.SHUT_WR)
        out = b""
        while chunk := s.recv(65536):
            out += chunk
    assert out.startswith(b"HTTP/") and int(out.split()[1]) == 200
    assert out.endswith(b"<html>page</html>")


def test_an_empty_request_line_is_closed_without_an_answer(server):
    assert raw(server, b"\r\n") is None


def test_bare_lf_line_endings_are_accepted(server):
    status, _, body = raw(server, b"GET " + base(server).encode() + b" HTTP/1.1\nHost: 127.0.0.1:"
                          + str(server.port).encode() + b"\n\n")
    assert (status, body) == (200, b"<html>page</html>")


@pytest.mark.parametrize("length, status", [
    (None, 400), ("", 400), ("abc", 400), ("-5", 400), ("0", 400), ("4097", 413),
    ("99999999999999999999999999", 413), ("1.5", 400), ("0x10", 400),
])
def test_content_length_is_checked_before_the_body(server, length, status):
    head = b"POST " + base(server).encode() + b"tx HTTP/1.1\r\n" + host_line(server)
    if length is not None:
        head += b"Content-Length: " + length.encode() + b"\r\n"
    got = raw(server, head + b"\r\n")  # no body sent: the answer must not wait for one
    assert got is not None and got[0] == status


def test_content_length_takes_python_int_syntax(server):
    body = json.dumps({"tx_hash": TX}).encode()
    head = (b"POST " + base(server).encode() + b"tx HTTP/1.1\r\n" + host_line(server)
            + b"Content-Length:  " + str(len(body)).encode() + b" \r\n\r\n")
    assert raw(server, head + body)[0] == 202
    assert server.calls() == [["on_tx", [TX, None]]]


def test_body_at_the_cap_is_read(server):
    pad = MAX_BODY - len(json.dumps({"tx_hash": TX, "pad": ""}))
    body = json.dumps({"tx_hash": TX, "pad": "x" * pad})
    assert len(body) == MAX_BODY
    resp, _ = request(server, "POST", base(server) + "tx", body)
    assert resp.status == 202


@pytest.mark.parametrize("body", [b"\xff\xfe{}", b"\xef\xbb\xbf{\"tx_hash\": \"" + TX.encode()
                                  + b"\"}", b"[]", b"null", b"\"0x\""])
def test_body_must_be_a_utf8_json_object(server, body):
    assert request(server, "POST", base(server) + "tx", body)[0].status == 400
    assert server.calls() == []


def test_unknown_post_route_is_404_before_the_body(server):
    head = b"POST " + base(server).encode() + b"nope HTTP/1.1\r\n" + host_line(server)
    assert raw(server, head + b"Content-Length: 99999\r\n\r\n")[0] == 404


def test_get_on_post_routes_is_404(make_server):
    srv = make_server(build={"mode": "ok", "value": {}})
    assert request(srv, "GET", base(srv) + "tx")[0].status == 404
    assert request(srv, "GET", base(srv) + "build")[0].status == 404
    assert request(srv, "POST", base(srv) + "status", "{}")[0].status == 404


def test_tx_route_answers_202_with_ok(server):
    resp, data = request(server, "POST", base(server) + "tx", json.dumps({"tx_hash": SIG}))
    assert resp.status == 202 and json.loads(data) == {"ok": True}
    assert resp.getheader("Content-Type") == "application/json"


def test_a_stalled_client_does_not_block_others(server):
    body = json.dumps({"tx_hash": TX}).encode()
    with socket.create_connection(("127.0.0.1", server.port), timeout=5) as stalled:
        stalled.sendall(b"POST " + base(server).encode() + b"tx HTTP/1.1\r\n" + host_line(server)
                        + b"Content-Length: %d\r\n\r\n" % len(body) + body[:4])
        started = time.monotonic()
        resp, data = request(server, "GET", base(server) + "status")
        assert resp.status == 200 and json.loads(data) == {"state": "waiting"}
        assert time.monotonic() - started < 2
        stalled.sendall(body[4:])  # the stalled request is still served when it completes
        assert stalled.recv(65536).startswith(b"HTTP/1.")
    assert server.calls() == [["on_tx", [TX, None]]]


def test_build_is_answered_when_the_job_finishes(make_server):
    srv = make_server(build={"mode": "ok", "value": {"transaction": "AA=="}, "delay_s": 0.4})
    started = time.monotonic()
    resp, data = request(srv, "POST", base(srv) + "build", json.dumps({"account": PLAYER}))
    assert resp.status == 200 and json.loads(data) == {"transaction": "AA=="}
    assert time.monotonic() - started >= 0.35
    assert srv.jobs() == [{"done": True, "cancelled": False}]


def test_build_waits_without_blocking_other_requests(make_server):
    srv = make_server(build={"mode": "ok", "value": {"transaction": "AA=="}, "delay_s": 1.5})
    conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
    conn.request("POST", base(srv) + "build", json.dumps({"account": PLAYER}),
                 {"Host": f"127.0.0.1:{srv.port}", "Content-Type": "application/json"})
    started = time.monotonic()
    resp, data = request(srv, "GET", base(srv) + "status")
    assert resp.status == 200 and time.monotonic() - started < 1.0
    built = conn.getresponse()
    assert built.status == 200 and json.loads(built.read()) == {"transaction": "AA=="}
    conn.close()


def test_build_with_a_bad_account_never_calls_build_tx(make_server):
    srv = make_server(build={"mode": "ok", "value": {}})
    for body in ['{"account": "0xnope"}', "{}", '{"account": 5}']:
        assert request(srv, "POST", base(srv) + "build", body)[0].status == 400
    assert srv.calls() == [] and srv.jobs() == []


def test_build_needs_a_valid_body(make_server):
    srv = make_server(build={"mode": "ok", "value": {}})
    assert request(srv, "POST", base(srv) + "build", "x" * (MAX_BODY + 1))[0].status == 413
    assert request(srv, "POST", base(srv) + "build", "not json")[0].status == 400
    assert srv.calls() == []
