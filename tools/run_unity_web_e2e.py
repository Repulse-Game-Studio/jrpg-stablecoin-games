"""Run the Unity WebGL end-to-end test: the dev project's e2e scene (unity/Project) built for WebGL,
served on localhost and played in Playwright Chromium against a local chain.

The build is single-threaded and uncompressed (Scfg.E2E.ScfgE2EBuild.BuildWebGL), so any static
server serves it. The page gets a mock wallet (EIP-6963 for evm/arc, Wallet Standard for solana)
and the e2e config (window.scfgE2EConfig); the game is driven through window.scfgE2E, which the e2e
driver (unity/Project/Assets/E2E/E2EDriver.cs, THE WEB DRIVER) defines only under that config.
Sheet buttons are clicked with the browser's mouse at the position the driver reports, the done
step is closed with the browser's keyboard, and the checkout overlay is used in the DOM. The steps
are the Godot web e2e's (tools/run_godot_web_e2e.py), plus the keyboard, Paste, links, the download
cap and a blocked IndexedDB:

  0. a window.scfgE2EConfig that is not a string is refused (no driver, nothing after);
  1. browser wallet: the in-page checkout overlay pays "chapter2", which becomes owned; its wallet
     button is clicked and its pay button reached with Tab and pressed with Enter (the page has the
     keyboard while the overlay shows: WebGLInput.captureAllKeyboardInput is off); the done step's
     explorer link opens a new tab on the receipt's transaction without window.opener;
  2. manual transfer: a tip copied from the sheet (through the browser's clipboard) and sent from
     the chain side is found; before that, Paste puts the browser's clipboard (cleaned) into the
     hash field, and with the clipboard permission taken away it shows paste_blocked;
  3. reload: after the tab reloads, the receipts and an unpaid pending session are still there (in
     IndexedDB, database /idbfs, within IDB_WITHIN_S seconds of the method step), and paying that
     session after the reload is found too;
  4. shared origin: the receipts' copy in localStorage (ScfgWebMirror) holds the receipts; with the
     game's page closed, state.json is deleted from IndexedDB (what another Unity game on the same
     origin can do), and after the game loads again every receipt is back from the copy and saved
     to IndexedDB again;
  5. download cap (phase-2 finding F14): a POST through the facade's own UnityWebRequestTransport
     (the one its RPC clients use, pumped by SCFG.Pump only) to an endpoint that streams far more
     than Coio.MaxResponseBytes fails with "response too large", and the endpoint saw the download
     stop before it sent everything; and the jslib says persistentDataPath is persistent;
  6. blocked IndexedDB: in a page whose indexedDB.open throws, the jslib says persistentDataPath is
     not persistent (the package's development-build warning).

--family evm (default) pays USDC on Anvil (the mock token's code at the real Arbitrum USDC address,
the built-in arbitrum entry with only `rpc_urls` configured, as the Godot web e2e); arc pays native
USDC on Anvil dressed as Arc; solana pays USDC on solana-test-validator (the built-in testnet
solana entry pointed at the local mint through the driver's chain override). They need `anvil`
(Foundry) or `solana-test-validator` (Agave) on PATH, Playwright's Chromium and `solders` (run with
`uv run`).

    uv run python tools/run_unity_web_e2e.py --unity ~/Unity/Hub/Editor/<version>/Editor/Unity \\
        [--family evm|arc|solana] [--player DIR] [--headed]

--player gives an already built WebGL player (the folder with index.html); otherwise the editor
builds one first (a cold WebGL build takes several minutes).
"""

from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path
from urllib.parse import urlsplit

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import run_godot_e2e as desk  # noqa: E402  (families, helpers; puts scfg_core and tests/integration on sys.path)
import run_godot_web_e2e as gweb  # noqa: E402  (the web families, the IndexedDB and localStorage helpers)
from run_godot_e2e import (  # noqa: E402
    CONFIRMED,
    ITEM,
    UNLOCKED,
    E2EError,
    button,
    has_button,
    no_screen,
    sheet,
    shows,
)
from run_renpy_web_e2e import serve  # noqa: E402

from browser_mocks import install_mock_solana_wallet, install_mock_wallet  # noqa: E402

ROOT = TOOLS.parent
PROJECT = ROOT / "unity" / "Project"
PRODUCT = "SCFG E2E"  # PlayerSettings.productName of the build (ScfgE2EBuild.Product)
STATE = "scfg/state.json"
IDB = "/idbfs"  # Unity's IDBFS mount: the IndexedDB database of persistentDataPath
IDB_WITHIN_S = 3
REPLY_TIMEOUT_MS = 30_000
BUILD_TIMEOUT = 3600
BLANK = gweb.BLANK
# Coio.MaxResponseBytes is 16 MiB; the endpoint offers four times that.
CAP_BYTES = 16 * 1024 * 1024
CAP_OFFER = 4 * CAP_BYTES
CAP_CHUNK = 64 * 1024
# What the page logs when something went wrong: errors, warnings and uncaught page errors.
BAD_CONSOLE_TYPES = ("error", "warning")
# Unity's own loader warns for every UnityWebRequest answer without a Content-Length (an RPC node's
# chunked reply): Unity's advice to the server, not a fault of the game.
UNITY_CHUNKED = re.escape("[UnityCache] Response is served without Content-Length header.")
PERMISSIONS = ["local-network-access", "clipboard-read", "clipboard-write"]


def idb_js(code: str) -> str:
    """The Godot web e2e's IndexedDB helpers on Unity's database."""
    return code.replace('"/userfs"', json.dumps(IDB))


IDB_READ = idb_js(gweb.IDB_READ)
IDB_DELETE = idb_js(gweb.IDB_DELETE)
IDB_LIST = idb_js(gweb.IDB_LIST)
MIRRORS = gweb.MIRRORS


def mirror_key(product: str = PRODUCT, storage_id: str | None = None) -> str:
    """ScfgWebMirror.KeyFor: scfg:<productName>:<sha256 hex of the storage id, else of the
    product name>."""
    return f"scfg:{product}:{hashlib.sha256((storage_id or product).encode()).hexdigest()}"


# --- the chain families ----------------------------------------------------------------------


class Solana(desk.Solana):
    """The desktop family, its chain override carried in the web config (no process environment)."""

    def config(self) -> dict:
        return {**super().config(), "chain": self.chain()}


FAMILIES = {"evm": gweb.Evm, "arc": gweb.Arc, "solana": Solana}


def install_wallet(fam, page) -> None:
    if isinstance(fam, Solana):
        install_mock_solana_wallet(page, fam.cluster, fam.player)
    else:
        install_mock_wallet(page, fam.rpc, fam.payer)


def check_mined(fam, tx: str) -> None:
    """The receipt's transaction is the wallet's payment (EVM and Arc; Solana's is checked by the
    watcher's own lookup)."""
    if isinstance(fam, Solana):
        return
    mined = fam.rpc.call("eth_getTransactionReceipt", [tx])
    if not mined or int(mined["status"], 16) != 1 or mined["from"].lower() != fam.payer.lower():
        raise E2EError(f"the receipt's transaction is not the wallet's payment: {mined}")


# --- the build -------------------------------------------------------------------------------


def build_webgl(unity: str, out: Path, log: Path) -> Path:
    cmd = [
        unity,
        "-batchmode",
        "-nographics",
        "-quit",
        "-projectPath",
        str(PROJECT),
        "-buildTarget",
        "WebGL",
        "-executeMethod",
        "Scfg.E2E.ScfgE2EBuild.BuildWebGL",
        "-scfgOut",
        str(out),
        "-logFile",
        str(log),
    ]
    started = time.monotonic()
    done = subprocess.run(cmd, timeout=BUILD_TIMEOUT)
    if done.returncode != 0 or not (out / "index.html").is_file():
        tail = log.read_text(errors="replace")[-6000:] if log.exists() else ""
        raise E2EError(f"the WebGL player did not build (exit {done.returncode}):\n{tail}")
    print(f"built the WebGL player in {time.monotonic() - started:.0f} s: {out}")
    return out


# --- the game in the page --------------------------------------------------------------------


class WebGame(gweb.WebGame):
    """The WebGL player in a Playwright page. Game buttons are the driver's; sheet buttons are
    clicked with the browser's mouse at the canvas position the driver reports."""

    def click(self, name: str, where: str = "screen") -> None:
        if where == "game":
            self.call("click", name=name, **{"in": "game"})
            return
        at = self.call("click", name=name, press=False)["at"]
        x, y = self.to_page(at)
        self.page.mouse.click(x, y)

    def to_page(self, at) -> tuple[float, float]:
        """Player screen pixels from the top left ([x, y, width, height]) as page coordinates."""
        rect = self.page.evaluate(
            "() => { const r = document.querySelector('canvas').getBoundingClientRect();"
            " return [r.left, r.top, r.width, r.height]; }"
        )
        sx, sy, width, height = at
        return rect[0] + sx * rect[2] / width, rect[1] + sy * rect[3] / height

    def problems(self) -> list[str]:
        return self.page.evaluate("() => (window.scfgE2EProblems || []).slice()")


def wait_for(game: WebGame, what: str, pred, timeout: float = 60, tick=None):
    return gweb.wait_for(game, what, pred, timeout, tick)


def wait_state(game: WebGame, what: str, pred, timeout: float = 60, tick=None) -> dict:
    return gweb.wait_state(game, what, pred, timeout, tick)


def receipts_of(state: dict) -> list[dict]:
    return gweb.receipts_of(state)


def overlay_hidden(page) -> None:
    """The overlay (bridge.js: a shadow-root host on <body>) hides itself after the payment."""
    page.wait_for_function(
        "() => [...document.body.children]"
        ".every((e) => !e.shadowRoot || e.style.display === 'none')",
        timeout=20_000,
    )


# --- the scenario ----------------------------------------------------------------------------


def a_non_string_config_fails_hard(browser, url: str) -> None:
    """window.scfgE2EConfig set to a number: the driver says so and does not start, and nothing
    else runs (no configure, no "SCFG.Configure(...) first"). In a context of its own."""
    context = browser.new_context(viewport={"width": 1280, "height": 720})
    try:
        page = context.new_page()
        logs: list[str] = []
        page.on("console", lambda m: logs.append(f"[{m.type}] {m.text}"))
        page.on("pageerror", lambda e: logs.append(f"[pageerror] {e}"))
        page.add_init_script("window.scfgE2EConfig = 5;")
        page.goto(url)
        reason = "e2e config window.scfgE2EConfig: not a string but number"
        deadline = time.monotonic() + 180
        while not any(reason in line for line in logs):
            if time.monotonic() > deadline:
                raise E2EError(
                    "the player did not refuse a non-string config:\n" + "\n".join(logs[-40:])
                )
            page.wait_for_timeout(200)
        page.wait_for_timeout(1500)  # what a driver that went on would log next
        bad = [
            line
            for line in logs
            if (line.startswith(("[error]", "[warning]", "[pageerror]")) or "scfg:" in line)
            and reason not in line
        ]
        if bad:
            raise E2EError("after refusing the config the player went on:\n" + "\n".join(bad))
        if page.evaluate("typeof window.scfgE2E") != "undefined":
            raise E2EError("the e2e driver started without a usable config")
    finally:
        context.close()
    print("0. a non-string window.scfgE2EConfig: the driver refuses it and does not start")


def press_with_the_keyboard(page, target, tabs: int = 40) -> None:
    """Tab to an overlay button and press Enter. The player's canvas would swallow every key of the
    page (WebGLInput.captureAllKeyboardInput) if the package did not hand the keyboard to the page
    while the overlay shows: Tab would not move and Enter would not press."""
    page.wait_for_function("(e) => !e.disabled", arg=target.element_handle(), timeout=20_000)
    for _ in range(tabs):
        if target.evaluate("(e) => e.getRootNode().activeElement === e"):
            break
        page.keyboard.press("Tab")
    else:
        raise E2EError(f"{tabs} Tabs did not reach the overlay's button: the page has no keyboard")
    page.keyboard.press("Enter")


def is_local(url: str) -> bool:
    return urlsplit(url).hostname in ("127.0.0.1", "localhost")


def explorer_link_opens_without_opener(game: WebGame, tx: str) -> str:
    """The done step's explorer link: ScfgWebPage.OpenUrl, window.open(url, "_blank",
    "noopener,noreferrer"), so the tab has no opener and no referrer,
    a frame after the click. The explorer itself is answered locally (no internet)."""
    page = game.page
    context = page.context

    def offline(route) -> None:
        route.fulfill(status=200, content_type="text/html", body="<!doctype html><title>x</title>")

    def external(url: str) -> bool:
        return not is_local(url)

    context.route(external, offline)
    try:
        with context.expect_page(timeout=20_000) as opened:
            game.click("explorer")
        tab = opened.value
        tab.wait_for_load_state()
        url = tab.url
        opener = tab.evaluate("() => window.opener")
        referrer = tab.evaluate("() => document.referrer")
        tab.close()
    finally:
        context.unroute(external, offline)
    page.bring_to_front()
    if not url.startswith("https://") or tx not in url:
        raise E2EError(f"the explorer link opened {url!r}, not the transaction {tx}")
    if opener is not None:
        raise E2EError(f"the explorer tab has a window.opener: {opener!r}")
    if referrer:
        raise E2EError(f"the explorer tab was told the game's page: document.referrer {referrer!r}")
    return url


def pay_unlock_with_overlay(game: WebGame, fam) -> None:
    page = game.page
    wait_for(game, "the Unlock button", lambda s: button(s["game"], "Unlock") is not None, 180)
    game.click("Unlock", "game")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    snap = wait_for(game, "the method step", has_button("method_browser"))
    if not snap["paused"]:
        raise E2EError("the demo should pause the game while the sheet is open")
    game.click("method_browser")
    wait_for(game, "the wallet step", has_button("forward", "Open wallet"))
    game.click("forward")
    # the overlay: "Pay with Mock Wallet" (clicked), then "Pay 1.0000NN USDC" with the keyboard
    press_with_the_keyboard(page, fam.connect(page))
    wait_for(game, "the payment to be confirmed", shows(CONFIRMED), 120, fam.tick)
    if not game.call("owns", item=ITEM):
        raise E2EError('the sheet says paid but SCFG.Owns("chapter2") is false')
    overlay_hidden(page)
    state = wait_state(
        game, "the unlock receipt", lambda s: any(r["item"] == ITEM for r in receipts_of(s))
    )
    paid = next(r["tx_hash"] for r in receipts_of(state) if r["item"] == ITEM)
    link = explorer_link_opens_without_opener(game, paid)
    game.click("continue")
    wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"])
    if button(snap["game"], "Unlock") is not None:
        raise E2EError("the Unlock button is still shown after the purchase")
    found = [r for r in receipts_of(game.state()) if r["item"] == ITEM and r["kind"] == "unlock"]
    if len(found) != 1:
        raise E2EError(f"state.json should hold one unlock receipt: {game.state()}")
    check_mined(fam, found[0]["tx_hash"])
    print(
        f"1. browser wallet: the overlay paid with the keyboard, chapter2 owned "
        f"(tx {found[0]['tx_hash']}); the explorer link opened {link} without an opener"
    )


def copy_value(game: WebGame, name: str) -> str:
    return gweb.copy_value(game, name)


PASTE = "  0xC0FFEE!12 and more\n"  # what the player has on the clipboard
PASTED = "0xC0FFEE12"  # SCFG.CleanPaste: the first word, hash characters only


def paste_into_the_hash_field(game: WebGame) -> None:
    """Paste (ScfgWebPage.RequestPaste / TakePaste: navigator.clipboard.readText, answered a frame
    or more later), then the same with the clipboard permission taken away: paste_blocked."""
    page = game.page
    page.evaluate("(t) => navigator.clipboard.writeText(t)", PASTE)
    game.click("paste")
    snap = wait_for(
        game,
        "the pasted hash",
        lambda s: ((sheet(s) or {}).get("fields") or {}).get("tx_input") == PASTED,
        20,
    )
    if sheet(snap)["error"] is not None:
        raise E2EError(f"the paste left an error: {sheet(snap)}")
    context = page.context
    context.clear_permissions()
    try:
        game.click("paste")
        snap = wait_for(
            game, "paste_blocked", lambda s: (sheet(s) or {}).get("error") == "paste_blocked", 20
        )
    finally:
        context.grant_permissions(PERMISSIONS)
    if sheet(snap)["fields"].get("tx_input") != PASTED:
        raise E2EError(f"a refused paste should keep the field: {sheet(snap)}")


def tip_by_manual_transfer(game: WebGame, fam) -> None:
    game.click("Tip", "game")
    wait_for(game, "the amount step", has_button("forward", "Next"))
    game.click("forward")  # Next: the 1 USDC the demo asks for is already chosen
    wait_for(game, "the method step", has_button("method_manual"))
    game.click("method_manual")
    wait_for(game, "the manual step", has_button("copy_amount"))
    amount = copy_value(game, "copy_amount")
    recipient = copy_value(game, "copy_recipient")
    token = copy_value(game, "copy_token")
    texts = sheet(game.snapshot())["texts"]
    if not any(t.startswith(amount + " ") for t in texts) or recipient not in texts:
        raise E2EError(f"copied {amount!r} / {recipient!r}, which the sheet does not show: {texts}")
    fam.check_manual(recipient, token)
    paste_into_the_hash_field(game)
    game.click("forward")  # I've sent it
    wait_for(
        game,
        "the waiting step",
        lambda s: any(
            t.startswith("Waiting for your payment") for t in (sheet(s) or {}).get("texts", [])
        ),
    )
    tx = fam.pay_manual(amount, recipient)
    snap = wait_for(game, "the tip to be confirmed", shows(CONFIRMED), 120, fam.tick)
    if sheet(snap)["focus"] != "continue":
        raise E2EError(f"the done step focuses {sheet(snap)['focus']!r}, not Continue")
    game.page.keyboard.press("Enter")  # Continue with the keyboard: the done step focuses it
    wait_for(game, "the sheet to close", no_screen)
    thanks = re.compile(r"^Thank you for the (1\.\d+) USDC tip!$")
    snap = wait_for(
        game, "the thank-you", lambda s: any(thanks.match(t) for t in s["game"]["texts"])
    )
    shown = next(thanks.match(t)[1] for t in snap["game"]["texts"] if thanks.match(t))
    receipts = receipts_of(game.state())
    tips = [r for r in receipts if r["kind"] == "tip"]
    if (
        len(receipts) != 2
        or len(tips) != 1
        or tips[0]["tx_hash"] != tx
        or tips[0]["amount"] != shown
    ):
        raise E2EError(
            f"state.json should hold the unlock and the {shown} USDC tip {tx}: {receipts}"
        )
    print(
        f"2. manual transfer: pasted {PASTED} (and paste_blocked without the permission); copied "
        f"{amount} USDC to {recipient}, paid from the chain side, found"
    )


def pending_survives_reload(game: WebGame, fam) -> dict:
    """Returns what IndexedDB held before the reload (IDB_LIST)."""
    page = game.page
    game.click("Tip", "game")
    wait_for(game, "the amount step", has_button("forward", "Next"))
    game.click("forward")
    wait_for(game, "the method step", has_button("method_manual"))
    shown = time.monotonic()
    state = wait_state(
        game, "the pending tip in state.json", lambda s: s.get("pending"), IDB_WITHIN_S
    )
    sid, session = next(iter(state["pending"].items()))
    # The facade writes a new session at once and asks the page to write it to IndexedDB.
    path = game.call("user_dir") + "/" + STATE
    while True:
        stored = page.evaluate(IDB_READ, path)
        if stored and sid in (json.loads(stored).get("pending") or {}):
            break
        if time.monotonic() > shown + IDB_WITHIN_S:
            raise E2EError(
                f"the pending session was not in IndexedDB ({path}) {IDB_WITHIN_S} s after the "
                f"method step showed: {stored!r}"
            )
        page.wait_for_timeout(100)
    idb = page.evaluate(IDB_LIST)
    page.reload()  # the tab goes away with the sheet still open
    game.wait_hook(180_000)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"], 120)
    if button(snap["game"], "Unlock") is not None or not game.call("owns", item=ITEM):
        raise E2EError("Chapter 2 is not owned after the reload")
    state = game.state()
    if len(receipts_of(state)) != 2 or sid not in (state.get("pending") or {}):
        raise E2EError(f"after the reload state.json should hold 2 receipts and {sid}: {state}")
    tx = fam.pay_manual(gweb.amount_of(session), fam.dev)
    state = wait_state(
        game, "the restored session's receipt", lambda s: len(receipts_of(s)) == 3, 120, fam.tick
    )
    if tx not in [r["tx_hash"] for r in receipts_of(state)] or sid in (state.get("pending") or {}):
        raise E2EError(f"the restored session {sid} should be paid by {tx}: {state}")
    print(
        f"3. reload: 2 receipts and pending session {sid} kept; paid after the reload, receipts=3"
    )
    return idb


def receipts_survive_a_deleted_state_file(game: WebGame, url: str) -> None:
    page = game.page
    key = mirror_key()
    path = game.call("user_dir") + "/" + STATE
    want = (game.state() or {}).get("receipts") or {}
    if len(want) != 3:
        raise E2EError(f"step 4 expects the 3 receipts of steps 1-3: {want}")
    deadline = time.monotonic() + 10
    while True:
        copies = page.evaluate(MIRRORS)
        stored = page.evaluate(IDB_READ, path)
        copy = json.loads(copies[key]) if key in copies else None
        on_disk = json.loads(stored)["receipts"] if stored else None
        if copy == {"version": 1, "receipts": want} and on_disk == want:
            break
        if time.monotonic() > deadline:
            raise E2EError(
                f"localStorage[{key!r}] should hold the 3 receipts, and only them: {copies}; "
                f"IndexedDB holds {stored!r}"
            )
        page.wait_for_timeout(100)
    if list(copies) != [key]:
        raise E2EError(f"one copy, under the product's key {key!r}: {list(copies)}")
    page.goto(url + BLANK)  # the game's page is gone: nothing writes IndexedDB back
    page.evaluate(IDB_DELETE, [path, path + ".tmp"])
    if page.evaluate(IDB_READ, path) is not None:
        raise E2EError(f"{path} is still in IndexedDB after deleting it")
    page.goto(url)
    game.wait_hook(180_000)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"], 120)
    if button(snap["game"], "Unlock") is not None or not game.call("owns", item=ITEM):
        raise E2EError("Chapter 2 is not owned after state.json was deleted from IndexedDB")
    got = (game.state() or {}).get("receipts") or {}
    if got != want:
        raise E2EError(f"the receipts should come back from the copy: {got} != {want}")
    deadline = time.monotonic() + 10
    while True:
        stored = page.evaluate(IDB_READ, path)
        if stored and json.loads(stored)["receipts"] == want:
            break
        if time.monotonic() > deadline:
            raise E2EError(f"the merged receipts never reached IndexedDB ({path}): {stored!r}")
        page.wait_for_timeout(100)
    print(f"4. shared origin: state.json deleted from IndexedDB, 3 receipts back from {key}")


class _Flood(http.server.BaseHTTPRequestHandler):
    """An RPC endpoint that answers every POST with CAP_OFFER bytes, a chunk at a time, and counts
    what it got to send before the client went away."""

    sent = 0
    finished = threading.Event()

    def log_message(self, *args):
        pass

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Allow-Methods", "POST")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def do_OPTIONS(self):  # noqa: N802 (http.server's naming)
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):  # noqa: N802
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(CAP_OFFER))
        self.end_headers()
        chunk = b" " * CAP_CHUNK
        try:
            while type(self).sent < CAP_OFFER:
                self.wfile.write(chunk)
                self.wfile.flush()
                type(self).sent += len(chunk)
                time.sleep(0.002)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            type(self).finished.set()


def the_download_cap_stops_a_flood(game: WebGame) -> None:
    flood = type("Flood", (_Flood,), {"sent": 0, "finished": threading.Event()})
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), flood)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{httpd.server_address[1]}/rpc"
        game.call(
            "post",
            url=url,
            payload={"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber", "params": []},
            timeout=60,
        )
        deadline = time.monotonic() + 90
        while (result := game.call("post_result")) is None:
            if time.monotonic() > deadline:
                raise E2EError(
                    f"the flooded POST did not end; the endpoint sent {flood.sent} bytes"
                )
            game.page.wait_for_timeout(200)
        if "response too large" not in json.dumps(result):
            raise E2EError(f"the flooded POST should fail with 'response too large': {result}")
        flood.finished.wait(30)
        if flood.sent >= CAP_OFFER:
            raise E2EError(
                f"the endpoint sent all {CAP_OFFER} bytes: the cap refused the body but did not "
                "stop the download"
            )
    finally:
        httpd.shutdown()
    persistent = game.call("fs_persistent")
    if persistent is not True:
        raise E2EError(f"the jslib says persistentDataPath is not persistent: {persistent!r}")
    print(
        f"5. download cap: {result['error']}; the download stopped after {flood.sent // 1024} KiB "
        f"of {CAP_OFFER // 1024} KiB (cap {CAP_BYTES // 1024} KiB); IndexedDB is persistent"
    )


BLOCK_IDB = (
    "IDBFactory.prototype.open = function () {"
    " throw new DOMException('the e2e blocks IndexedDB', 'SecurityError'); };"
)


def a_blocked_indexeddb_is_not_persistent(browser, url: str, fam) -> None:
    """A browser that has indexedDB but refuses its database (blocked site data, a partitioned
    iframe): Unity runs persistentDataPath on memory, and the jslib must say so. In a context of
    its own."""
    context = browser.new_context(viewport={"width": 1280, "height": 720})
    try:
        page = context.new_page()
        page.add_init_script(BLOCK_IDB)
        page.add_init_script(f"window.scfgE2EConfig = {json.dumps(json.dumps(fam.config()))};")
        page.goto(url)
        game = WebGame(page)
        game.wait_hook(180_000)
        persistent = game.call("fs_persistent")
        if persistent is not False:
            raise E2EError(f"with IndexedDB blocked the jslib answered {persistent!r}, not false")
    finally:
        context.close()
    print("6. blocked IndexedDB: the jslib says persistentDataPath is not persistent")


# --- the run ---------------------------------------------------------------------------------


def console_problems(console: list[tuple[str, str]], allowed=()) -> list[str]:
    """The page's errors and warnings (and uncaught page errors), but those `allowed` matches."""
    return [
        text
        for kind, text in console
        if (kind in BAD_CONSOLE_TYPES or kind == "pageerror")
        and not any(re.search(a, text) for a in allowed)
    ]


def run(unity: str | None, family: str, player: str | None, headed: bool) -> int:
    fam_cls = FAMILIES[family]
    exe = shutil.which(fam_cls.tool)
    if not exe:
        print(fam_cls.missing)
        return 2
    if not player and not unity:
        print("give --unity (the editor, to build the player) or --player (a built one)")
        return 2
    from playwright.sync_api import sync_playwright

    started = time.monotonic()
    work = Path(tempfile.mkdtemp(prefix="scfg-unity-web-e2e-"))
    fam = httpd = None
    console: list[tuple[str, str]] = []
    game = None
    try:
        if player:
            web = Path(player)
            if not (web / "index.html").is_file():
                raise E2EError(f"{web} holds no WebGL player (no index.html)")
            served = work / "web"
            shutil.copytree(web, served)
        else:
            served = build_webgl(unity, work / "web", work / "build.log")
        # (the icon link: without one Chromium asks for /favicon.ico; the 404 is a console error)
        (served / BLANK).write_text(
            '<!doctype html><title>blank</title><link rel="icon" href="data:,">\n'
        )
        print(f"Unity web e2e: {family} on {served}")
        url, httpd = serve(served)
        fam = fam_cls(exe, work)
        with sync_playwright() as p:
            browser = desk.launch_chromium(
                p,
                headless=not headed,
                args=[
                    "--use-angle=swiftshader",
                    "--enable-unsafe-swiftshader",
                    "--autoplay-policy=no-user-gesture-required",
                ],
            )
            a_non_string_config_fails_hard(browser, url)
            context = browser.new_context(
                permissions=PERMISSIONS, viewport={"width": 1280, "height": 720}
            )
            page = context.new_page()
            page.on("console", lambda m: console.append((m.type, m.text)))
            page.on("pageerror", lambda e: console.append(("pageerror", str(e))))
            page.on("response", lambda r: r.status < 400 or console.append(("http", r.url)))
            page.add_init_script(f"window.scfgE2EConfig = {json.dumps(json.dumps(fam.config()))};")
            install_wallet(fam, page)
            page.goto(url)
            game = WebGame(page)
            game.wait_hook(180_000)
            pay_unlock_with_overlay(game, fam)
            tip_by_manual_transfer(game, fam)
            idb = pending_survives_reload(game, fam)
            receipts_survive_a_deleted_state_file(game, url)
            the_download_cap_stops_a_flood(game)
            saved = [x for x in idb["paths"] if x.endswith("/" + STATE)]
            print(f"   IndexedDB: databases {idb['dbs']}, {len(idb['paths'])} entries, {saved}")
            reported = game.problems()
            a_blocked_indexeddb_is_not_persistent(browser, url, fam)
            browser.close()
        if reported:
            raise E2EError("the player reported errors or warnings:\n" + "\n".join(reported))
        bad = console_problems(console, (UNITY_CHUNKED,))
        if bad:
            raise E2EError("the page logged errors or warnings:\n" + "\n".join(bad))
    except Exception as e:
        print("\n".join(f"[{k}] {t}" for k, t in console[-80:]))
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(
            f"Unity web e2e ({family}) FAILED after {time.monotonic() - started:.1f} s "
            f"(files in {work})"
        )
        return 1
    finally:
        if httpd is not None:
            httpd.shutdown()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"Unity web e2e ({family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--unity", help="the Unity editor binary (builds the WebGL player)")
    ap.add_argument("--player", help="an already built WebGL player (the folder with index.html)")
    ap.add_argument("--family", choices=tuple(FAMILIES), default="evm")
    ap.add_argument("--headed", action="store_true", help="show the browser window")
    args = ap.parse_args(argv)
    return run(args.unity, args.family, args.player, args.headed)


if __name__ == "__main__":
    raise SystemExit(main())
