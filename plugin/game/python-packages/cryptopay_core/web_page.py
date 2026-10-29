"""The game's side of Ren'Py's web build: every call into the page's JavaScript
(cryptopay/web/bridge.js) behind a small Python method.

``js`` is Ren'Py's ``emscripten`` module (run_script, run_script_int, run_script_string);
tests pass a fake. Values cross as JSON literals, which are valid JavaScript expressions
for the same values, so game data is never spliced into JavaScript as code.

WebPage is the PumpWorker's bridge (start/poll), the OverlayCheckout's ui
(open/show/close/take/set_status/reply), and the clipboard, links and wallet detection
Ren'Py lacks on the web.
"""

from __future__ import annotations

import json
from typing import Callable

SCRIPTS = ("bridge.js", "evm.js", "solana.js", "app.js")  # bridge.js first: it marks the page


def _lit(value) -> str:
    return json.dumps(value)


class WebPage:
    SCRIPTS = SCRIPTS

    def __init__(self, js, load_asset: Callable[[str], bytes]):
        self.js = js
        self.load_asset = load_asset  # file name in cryptopay/web/ -> bytes
        self.installed = False

    def install(self) -> None:
        """Load bridge.js and the checkout page's scripts into the page, once."""
        if self.installed:
            return
        for name in SCRIPTS:
            self.js.run_script(self.load_asset(name).decode("utf-8"))
        page = self.load_asset("index.html").decode("utf-8")
        self.js.run_script(f"cryptopayBridge.setPage({_lit(page)})")
        self.installed = True

    # --- requests (pump_worker bridge) ---------------------------------------------------

    def start(self, url: str, body: str, timeout: float) -> int:
        ms = int(timeout * 1000)
        return self.js.run_script_int(f"cryptopayBridge.post({_lit(url)}, {_lit(body)}, {ms})")

    def poll(self, request_id: int) -> dict | None:
        text = self.js.run_script_string(f"cryptopayBridge.result({int(request_id)})")
        return json.loads(text) if text else None

    # --- wallets ---------------------------------------------------------------------------

    def has_wallet(self, chain) -> bool:
        args = f"{_lit(chain.family)}, {_lit(chain.wallet_chain)}"
        return bool(self.js.run_script_int(f"cryptopayBridge.hasWallet({args})"))

    # --- checkout overlay (overlay.OverlayCheckout's ui) -------------------------------------

    def open(self, key: str, session_info: dict, logo_url: str | None) -> None:
        args = f"{_lit(key)}, {_lit(session_info)}, {_lit(logo_url)}"
        self.js.run_script(f"cryptopayBridge.openCheckout({args})")

    def show(self, key: str) -> None:
        self.js.run_script(f"cryptopayBridge.show({_lit(key)})")

    def close(self, key: str) -> None:
        self.js.run_script(f"cryptopayBridge.closeCheckout({_lit(key)})")

    def take(self, key: str) -> list:
        return json.loads(self.js.run_script_string(f"cryptopayBridge.take({_lit(key)})"))

    def set_status(self, key: str, status: dict) -> None:
        self.js.run_script(f"cryptopayBridge.setStatus({_lit(key)}, {_lit(status)})")

    def reply(self, key: str, request_id: int, status: int, body: dict) -> None:
        args = f"{_lit(key)}, {int(request_id)}, {int(status)}, {_lit(body)}"
        self.js.run_script(f"cryptopayBridge.reply({args})")

    # --- clipboard and links ------------------------------------------------------------------

    def copy(self, text: str) -> None:
        self.js.run_script(f"cryptopayBridge.copy({_lit(text)})")

    def request_paste(self) -> None:
        self.js.run_script("cryptopayBridge.requestPaste()")

    def take_paste(self) -> str | None:
        """None until the browser answers; "" if it refused; else the clipboard text."""
        return json.loads(self.js.run_script_string("cryptopayBridge.takePaste()"))

    def take_copy_failed(self) -> bool:
        """True once after a copy() the browser refused; False otherwise."""
        return bool(self.js.run_script_int("cryptopayBridge.takeCopyFailed()"))

    def open_url(self, url: str) -> None:
        self.js.run_script(f"cryptopayBridge.openUrl({_lit(url)})")
