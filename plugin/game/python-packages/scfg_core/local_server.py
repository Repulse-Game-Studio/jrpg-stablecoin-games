"""The localhost checkout server for desktop browser wallets.

Browser extensions (MetaMask, Rabby, ...) only talk to web pages, never to a game.
So the game serves a tiny bundled page on 127.0.0.1 and opens the browser on it. The
page connects the wallet, sends the transfer and posts the tx hash back here.

Security:
  * bound to 127.0.0.1 only, on a random port
  * every URL must contain a 128-bit secret (/c/<secret>/...), otherwise 404
  * the Host header must be exactly 127.0.0.1:<port> (defeats DNS rebinding)
  * no CORS headers, small request bodies only
  * the build route returns an *unsigned* transaction, only for the validated account,
    and gives up after BUILD_TIMEOUT_S
  * a posted tx hash is only a *hint*: the watcher verifies it on-chain
  * the developer's logo is served as a raster image with nosniff (never SVG)
"""

from __future__ import annotations

import json
import mimetypes
import secrets
import threading
from collections.abc import Callable
from concurrent.futures import Future
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .page_messages import BUILD_TIMEOUT_S, parse_build_message, parse_tx_message

MAX_BODY = 4096
ASSETS = ("index.html", "app.js", "evm.js", "solana.js")


class CheckoutServer:
    def __init__(self, *, load_asset: Callable[[str], bytes], session_info: dict,
                 on_tx: Callable[[str, str | None], None], get_status: Callable[[], dict],
                 load_logo: Callable[[], tuple[bytes, str]] | None = None,
                 build_tx: Callable[[str], Future] | None = None,
                 host: str = "127.0.0.1"):
        self.load_asset = load_asset
        self.load_logo = load_logo
        self.build_tx = build_tx
        self.session_info = session_info
        self.on_tx = on_tx
        self.get_status = get_status
        self.host = host
        self.secret = secrets.token_urlsafe(16)
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    @property
    def port(self) -> int:
        assert self._httpd is not None
        return self._httpd.server_address[1]

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/c/{self.secret}/"

    def start(self) -> str:
        if self._httpd is None:
            self._httpd = ThreadingHTTPServer((self.host, 0), _make_handler(self))
            self._httpd.daemon_threads = True
            # A short poll interval keeps stop() fast: it runs on Ren'Py's main thread.
            self._thread = threading.Thread(target=self._httpd.serve_forever, args=(0.05,),
                                            name="scfg-checkout", daemon=True)
            self._thread.start()
        return self.url

    def stop(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
            self._thread = None

    @property
    def running(self) -> bool:
        return self._httpd is not None


def _make_handler(server: CheckoutServer):
    class Handler(BaseHTTPRequestHandler):
        server_version = "scfg"
        sys_version = ""

        def log_message(self, format, *args):  # keep the game's console quiet
            pass

        def _send(self, code: int, body: bytes = b"", ctype: str = "text/plain; charset=utf-8"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, code: int, obj) -> None:
            self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

        def _route(self) -> str | None:
            """Return the path after /c/<secret>/, or None (already answered) if rejected."""
            if self.headers.get("Host") != f"{server.host}:{server.port}":
                self._send(403, b"forbidden")
                return None
            prefix = f"/c/{server.secret}/"
            path = self.path.split("?", 1)[0]
            if not path.startswith(prefix):
                self._send(404, b"not found")
                return None
            return path[len(prefix):]

        def do_GET(self):
            sub = self._route()
            if sub is None:
                return
            if sub in ("", "index.html"):
                sub = "index.html"
            if sub in ASSETS:
                try:
                    body = server.load_asset(sub)
                except Exception:
                    self._send(500, b"asset missing")
                    return
                ctype = mimetypes.guess_type(sub)[0] or "application/octet-stream"
                if ctype.startswith("text/") or ctype.endswith("javascript"):
                    ctype += "; charset=utf-8"
                self._send(200, body, ctype)
            elif sub == "session.json":
                self._json(200, server.session_info)
            elif sub == "status":
                self._json(200, server.get_status())
            elif sub == "logo" and server.load_logo is not None:
                try:
                    body, ctype = server.load_logo()
                except Exception:
                    self._send(404, b"not found")
                    return
                self._send(200, body, ctype)
            else:
                self._send(404, b"not found")

        def _read_json(self) -> dict | None:
            """The request's JSON object, or None after answering 400/413."""
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if not 0 < length <= MAX_BODY:
                self._send(413 if length > MAX_BODY else 400, b"bad body")
                return None
            try:
                data = json.loads(self.rfile.read(length).decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                data = None
            if not isinstance(data, dict):
                self._send(400, b"bad body")
                return None
            return data

        def do_POST(self):
            sub = self._route()
            if sub is None:
                return
            if sub == "build":
                if server.build_tx is None:
                    self._send(404, b"not found")
                    return
                data = self._read_json()
                if data is None:
                    return
                try:
                    account = parse_build_message(data)
                except ValueError:
                    self._send(400, b"bad account")
                    return
                fut = None
                try:
                    fut = server.build_tx(account)
                    self._json(200, fut.result(timeout=BUILD_TIMEOUT_S))
                except Exception:  # worker busy, RPC down, timeout: the page offers a retry
                    if fut is not None:
                        fut.cancel()  # don't build it later for a page that stopped waiting
                    self._json(503, {"error": "unavailable"})
                return
            if sub != "tx":
                self._send(404, b"not found")
                return
            data = self._read_json()
            if data is None:
                return
            try:
                tx_hash, sender = parse_tx_message(data)
            except ValueError:
                self._send(400, b"bad tx hash")
                return
            server.on_tx(tx_hash, sender)
            self._json(202, {"ok": True})

    return Handler
