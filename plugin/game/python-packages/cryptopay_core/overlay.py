"""The browser checkout as an overlay inside the game's own web page (Ren'Py web build).

On the web the game already runs in a browser, and wallet extensions inject into its
page, so the checkout page (app.js, evm.js, solana.js) is shown over the game instead of
being served on localhost. OverlayCheckout has local_server.CheckoutServer's interface,
so CheckoutController drives both alike. The page's messages are read by pump(), which
the controller calls every frame; they are validated like the local server's requests,
and a tx hash stays a hint the watcher verifies on-chain.
"""

from __future__ import annotations

import time
from typing import Callable

from .page_messages import BUILD_TIMEOUT_S, parse_build_message, parse_tx_message


class OverlayCheckout:
    url = "overlay:checkout"  # what start() returns: there is no page to open

    def __init__(self, *, ui, key: str, session_info: dict,
                 on_tx: Callable[[str, str | None], None], get_status: Callable[[], dict],
                 logo_url: str | None = None, build_tx: Callable | None = None,
                 clock: Callable[[], float] = time.monotonic):
        self.ui = ui
        self.key = key
        self.session_info = session_info
        self.on_tx = on_tx
        self.get_status = get_status
        self.logo_url = logo_url
        self.build_tx = build_tx
        self.clock = clock
        self._builds: dict = {}   # page request id -> (Future, deadline)
        self._status: dict | None = None
        self._open = False

    @property
    def running(self) -> bool:
        return self._open

    def start(self) -> str:
        self.ui.open(self.key, self.session_info, self.logo_url)
        self._open = True
        self._push_status()
        return self.url

    def reopen(self) -> None:
        if self._open:
            self.ui.show(self.key)

    def stop(self) -> None:
        if not self._open:
            return
        self._open = False
        for fut, _ in self._builds.values():
            fut.cancel()
        self._builds.clear()
        self.ui.close(self.key)

    def pump(self) -> None:
        if not self._open:
            return
        for message in self.ui.take(self.key):
            self._handle(message)
        self._finish_builds()
        self._push_status()

    def _handle(self, message) -> None:
        if not isinstance(message, dict):
            return
        kind = message.get("type")
        if kind == "tx":
            try:
                tx_hash, sender = parse_tx_message(message.get("body"))
            except ValueError:
                return  # the page only posts what a wallet returned: nothing to answer
            self.on_tx(tx_hash, sender)
            return
        rid = message.get("id")
        if kind != "build" or not isinstance(rid, int) or isinstance(rid, bool):
            return
        if self.build_tx is None:
            self.ui.reply(self.key, rid, 404, {"error": "not_found"})
            return
        try:
            account = parse_build_message(message.get("body"))
        except ValueError:
            self.ui.reply(self.key, rid, 400, {"error": "bad account"})
            return
        self._builds[rid] = (self.build_tx(account), self.clock() + BUILD_TIMEOUT_S)

    def _finish_builds(self) -> None:
        for rid, (fut, deadline) in list(self._builds.items()):
            if fut.done():
                del self._builds[rid]
                if fut.cancelled() or fut.exception() is not None:
                    self.ui.reply(self.key, rid, 503, {"error": "unavailable"})
                else:
                    self.ui.reply(self.key, rid, 200, fut.result())
            elif self.clock() >= deadline:
                del self._builds[rid]
                fut.cancel()  # don't build it later for a page that stopped waiting
                self.ui.reply(self.key, rid, 503, {"error": "unavailable"})

    def _push_status(self) -> None:
        status = self.get_status()
        if status != self._status:
            self._status = dict(status)
            self.ui.set_status(self.key, status)
