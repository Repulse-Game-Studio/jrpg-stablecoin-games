"""Run the RPG Maker MZ web end-to-end test: the built SCFG.js on the MZ shim in a real page,
played in Playwright Chromium against a local Anvil chain.

The plugin is built (`node build.mjs` in rpgmaker/) and the rpgmaker/ folder is served on
localhost. The page is the harness's test/harness/e2e.html: MZ's load order (js/plugins.js, the
engine, SCFG.js, main.js) with the demo kit's js/plugins.js (rpgmaker/demo/js/plugins.js: testnet,
chapter2 with Owned Switch 1) and the shim in place of the engine. The harness, never SCFG.js,
reads this runner's test-only override, `window.scfgE2EConfig` (a JSON string set by an init
script), which replaces some of those plugin parameters as an edit of js/plugins.js would: the
wallet, the chains and tokens, the RPC URLs pointing at Anvil and (evm) the Confirmations,
{"arbitrum": 2}, as the Godot web e2e configures them; Arc keeps its one. For --family evm the
mock token's code is placed with anvil_setCode at the real Arbitrum Sepolia USDC address (the
Godot web e2e's family on TESTNET), so the built-in testnet arbitrum entry is used as is; Arc's
USDC interface and system emitter sit at their real addresses (run_renpy_e2e.setup_arc).

The payments start from the demo map's events (the shim has no map to walk on: the event's page
runs on the map's interpreter, as when the player talks to the NPC); everything after that is the
player's: real mouse clicks on the plugin's DOM sheet and on the checkout overlay, keys held for a
few frames (MZ reads the keyboard once a frame). The page gets a mock EIP-6963 wallet backed by
Anvil. Saves go through MZ's StorageManager; the shim keeps a web game's saves in the page's
localStorage (test/mz_shim/browser.ts, test only, standing in for MZ's IndexedDB), so they outlive
a reload. The three steps of the Ren'Py and Godot web e2e:

  1. browser wallet: the in-page checkout overlay pays "chapter2"; the Unlock Item command's
     status variable holds 1 (paid) and switch 1 (Chapter 2 owned) turns ON;
  2. manual transfer: a tip copied from the sheet and sent straight from Anvil is found;
  3. reload: after the tab reloads, the receipts and an unpaid pending session are still in
     storage (read before the reload too) and switch 1 is still ON; paying that session from
     Anvil after the reload is found too.

--family arc runs the same steps on Anvil dressed as Arc; its manual payments are native USDC
sends, which only Arc's system emitter logs.

Needs anvil (Foundry), Node 22.18+ with rpgmaker/node_modules, and Playwright's Chromium:

    uv run python tools/run_rpgmaker_web_e2e.py [--family arc] [--headed]
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import run_godot_web_e2e as godot_web  # noqa: E402  (also puts scfg_core, tests/integration on sys.path)
from js_driver import find_node  # noqa: E402
from run_godot_e2e import E2EError, launch_chromium  # noqa: E402
from run_godot_web_e2e import amount_of, copy_value, receipts_of  # noqa: E402
from run_renpy_web_e2e import serve  # noqa: E402

from scfg_core.chains import TESTNET  # noqa: E402

from browser_mocks import install_mock_wallet  # noqa: E402

ROOT = TOOLS.parent
RPGMAKER = ROOT / "rpgmaker"
MAP = RPGMAKER / "demo" / "data" / "Map001.json"
PAGE = "test/harness/e2e.html?test"
ITEM = "chapter2"
CONFIRMED = "Payment confirmed. Thank you!"
OWNED_SWITCH = 1  # the demo kit: chapter2's Owned Switch
STATUS_VARIABLE = 1  # the demo kit's events store their status here
PAID = 1  # the status the plugin commands store for a payment (SCFG.STATUS.paid)
# Page output that means something went wrong: console errors and warnings (the plugin's reports
# and its test-play developer warnings are "scfg: ..." console.error / console.warn lines, and the
# shim's SceneManager.onError prints what stopped the game) and uncaught errors. A clean run logs
# nothing of these.
BAD_CONSOLE = re.compile(r"^\[(error|warning|pageerror)\]")
HOLD_MS = 100  # a key held this long spans several of MZ's frames

SCREEN = ".screen[data-screen=payment]"  # in the sheet host's shadow root
SHEET = "#scfg-host " + SCREEN

# The overlay's host: the element bridge.js adds to the body (the sheet's own host is #scfg-host).
OVERLAYS_HIDDEN = """() => [...document.body.children]
    .every((e) => !e.shadowRoot || e.id === "scfg-host" || e.style.display === "none")"""

# The data-key of the focused control of the sheet, or null.
SHEET_FOCUS = """() => {
    const host = document.getElementById("scfg-host");
    const el = host && host.shadowRoot && host.shadowRoot.activeElement;
    return el ? el.getAttribute("data-key") : null;
}"""

# What stopped the game (the shim models MZ's SceneManager.onError/onReject: an uncaught error or
# an unhandled rejection stops an MZ game), as text; [] while it runs.
STOPPED_BY = "() => window.mzShim ? mzShim.errors.map((e) => String(e && e.stack || e)) : []"

# What the game holds: the map, the event, switch 1 and variable 1, what the plugin says it owns.
GAME = """([sw, v, item]) => ({
    map: SceneManager._scene instanceof Scene_Map && SceneManager._scene.isStarted(),
    loaded: SCFG.stateLoaded(),
    eventRunning: $gameMap.isEventRunning(),
    switch: $gameSwitches.value(sw),
    variable: $gameVariables.value(v),
    owns: SCFG.owns(item),
})"""

# The plugin's state document as MZ's StorageManager stored it (the shim's web saves: localStorage
# under "mzshim:" + the localforage key rmmzsave.<gameId>.scfg), or null.
STORED = """() => {
    const text = localStorage.getItem("mzshim:rmmzsave." + $dataSystem.advanced.gameId + ".scfg");
    return text === null ? null : JSON.parse(text);
}"""


# --- the chain families (the demo kit plays on testnets) ---------------------------------------


class Evm(godot_web.Evm):
    """USDC on Anvil: the mock token's code at the real Arbitrum Sepolia USDC address (the Godot
    web e2e's family on TESTNET), so the built-in testnet arbitrum entry is used with only its
    RPC and confirmations set."""

    network = TESTNET

    def parameters(self) -> dict:
        return {
            "wallet": self.dev,
            "chains": json.dumps(["arbitrum"]),
            "tokens": json.dumps(["USDC"]),
            "rpcUrls": json.dumps({"arbitrum": [self.url]}),
            "confirmations": json.dumps({"arbitrum": 2}),
        }


class Arc(godot_web.Arc):
    """Native USDC on Anvil dressed as Arc: the built-in testnet arc entry with its RPC set
    (one confirmation, Arc's own)."""

    def parameters(self) -> dict:
        return {
            "wallet": self.dev,
            "chains": json.dumps(["arc"]),
            "tokens": json.dumps(["USDC"]),
            "rpcUrls": json.dumps({"arc": [self.url]}),
        }


FAMILIES = {"evm": Evm, "arc": Arc}


# --- the game in the page --------------------------------------------------------------------


class WebGame:
    def __init__(self, page):
        self.page = page
        self.map = json.loads(MAP.read_text())

    def wait_booted(self, timeout_ms: int = 60_000) -> None:
        self.page.wait_for_function(
            "() => window.SCFG && SceneManager._scene instanceof Scene_Map"
            " && SceneManager._scene.isStarted() && SCFG.stateLoaded()",
            timeout=timeout_ms,
        )

    def check_running(self) -> None:
        """Fail at once when an error stopped the game (MZ's error screen)."""
        stopped = self.page.evaluate(STOPPED_BY)
        if stopped:
            why = "\n".join(stopped)
            raise E2EError(f"the game stopped on an error (SceneManager.onError):\n{why}")

    def state(self) -> dict:
        self.check_running()
        return self.page.evaluate(GAME, [OWNED_SWITCH, STATUS_VARIABLE, ITEM])

    def stored(self) -> dict:
        """The stored state document; {} before anything was stored."""
        return self.page.evaluate(STORED) or {}

    def snapshot(self) -> dict:
        """What the failure messages show: the sheet's step and the game state."""
        return {"step": self.step(), **self.state()}

    def talk_to(self, name: str) -> None:
        """Start the demo map's event `name` (its page 1) on the map's interpreter."""
        ev = next(e for e in self.map["events"] if e and e["name"].split(" (")[0] == name)
        self.page.evaluate(
            """([list, id]) => {
                if ($gameMap.isEventRunning()) throw new Error("an event is already running");
                $gameMap._interpreter.setup(list, id);
            }""",
            [ev["pages"][0]["list"], ev["id"]],
        )

    def key(self, name: str):
        return self.page.locator(f"{SHEET} [data-key='{name}']")

    def step(self) -> str | None:
        sheet = self.page.locator(SHEET)
        return sheet.get_attribute("data-step") if sheet.count() else None

    def wait_step(self, step: str, timeout_ms: int = 20_000) -> None:
        self.page.wait_for_function(
            "([sel, s]) => { const h = document.getElementById('scfg-host');"
            " const e = h && h.shadowRoot && h.shadowRoot.querySelector(sel);"
            " return !!e && e.getAttribute('data-step') === s; }",
            arg=[SCREEN, step],
            timeout=timeout_ms,
        )

    def click(self, name: str) -> None:
        """A real mouse click on the sheet's control (Playwright scrolls it into view)."""
        self.key(name).click()

    def hold(self, key: str) -> None:
        """A key held long enough for MZ's Input to see it (press() releases within a frame)."""
        self.page.keyboard.down(key)
        self.page.wait_for_timeout(HOLD_MS)
        self.page.keyboard.up(key)
        self.page.wait_for_timeout(HOLD_MS)

    def text(self, name: str) -> str:
        return self.key(name).inner_text()


def wait_until(game: WebGame, what: str, pred, timeout: float = 60, tick=None, read=None):
    """`read()` (default: the game state) until pred holds; the page runs between the reads (the
    mock wallet's callbacks only run inside Playwright calls) and `tick()` runs (a block)."""
    read = read or game.state
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        game.check_running()
        last = read()
        if pred(last):
            return last
        game.page.wait_for_timeout(200)
        if tick is not None:
            tick()
    raise E2EError(f"timed out waiting for {what}; last read:\n{json.dumps(last, indent=1)}")


def wait_paid(game: WebGame, fam, timeout: float = 120) -> None:
    def done(_):
        return (
            game.step() == "done"
            and game.key("heading").count()
            and game.text("heading") == CONFIRMED
        )

    wait_until(
        game, "Payment confirmed on the sheet", done, timeout, fam.tick, read=lambda: game.step()
    )


def finished(state: dict) -> bool:
    return not state["eventRunning"]


def check_paid_event(state: dict, what: str) -> None:
    if state["variable"] != PAID:
        raise E2EError(f"{what}: the event's status variable holds {state['variable']}, not {PAID}")


# --- the scenario ----------------------------------------------------------------------------


def pay_unlock_with_overlay(game: WebGame, fam) -> None:
    page = game.page
    state = game.state()
    if state["switch"] or state["owns"]:
        raise E2EError(f"Chapter 2 is owned before the purchase: {state}")
    # the Confirmations parameter reached the configuration SCFG.js built
    want = json.loads(fam.parameters().get("confirmations", "{}"))
    got = page.evaluate("() => SCFG._config.confirmations")
    if got != want:
        raise E2EError(f"the configuration's confirmations are {got}, not the parameter's {want}")
    game.talk_to("Merchant")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    game.wait_step("method")
    game.click("method_browser")
    game.wait_step("browser")
    if game.text("forward") != "Open wallet":
        raise E2EError(f"the browser step's button says {game.text('forward')!r}")
    game.click("forward")
    fam.connect(page).click()  # the overlay: "Pay with Mock Wallet", then "Pay 4.99NNNN USDC"
    wait_paid(game, fam)
    # The overlay hides itself after the wallet's send (bridge.js); the sheet takes the focus
    # back, so one held Enter presses its Continue.
    page.wait_for_function(OVERLAYS_HIDDEN, timeout=20_000)
    wait_until(
        game,
        "the sheet to focus Continue",
        lambda f: f == "continue",
        10,
        read=lambda: page.evaluate(SHEET_FOCUS),
    )
    game.hold("Enter")
    state = wait_until(game, "the sheet to close and the event to end", finished)
    if page.locator(SHEET).count():
        raise E2EError("the sheet is still open after Continue")
    check_paid_event(state, "Unlock Item")
    if state["switch"] is not True or not state["owns"]:
        raise E2EError(f"switch {OWNED_SWITCH} should be ON and chapter2 owned: {state}")
    found = [r for r in receipts_of(game.stored()) if r["item"] == ITEM and r["kind"] == "unlock"]
    if len(found) != 1:
        raise E2EError(f"storage should hold one unlock receipt: {game.stored()}")
    mined = fam.rpc.call("eth_getTransactionReceipt", [found[0]["tx_hash"]])
    if not mined or int(mined["status"], 16) != 1 or mined["from"].lower() != fam.payer.lower():
        raise E2EError(f"the receipt's transaction is not the wallet's payment: {mined}")
    print(
        f"1. browser wallet: the overlay paid, switch {OWNED_SWITCH} ON (tx {found[0]['tx_hash']})"
    )


def tip_by_manual_transfer(game: WebGame, fam) -> None:
    game.talk_to("Bard")
    game.wait_step("amount")
    game.click("forward")  # Next: the event's 1.00 USDC is already chosen
    game.wait_step("method")
    game.click("method_manual")
    game.wait_step("manual")
    amount = copy_value(game, "copy_amount")
    recipient = copy_value(game, "copy_recipient")
    token = copy_value(game, "copy_token")
    shown = [game.text("value_amount"), game.text("value_recipient"), game.text("value_token")]
    if shown != [f"{amount} USDC", recipient, token]:
        raise E2EError(f"copied {amount!r} / {recipient!r} / {token!r}; the sheet shows {shown}")
    fam.check_manual(recipient, token)
    game.click("forward")  # I've sent it
    game.wait_step("waiting")
    tx = fam.pay_manual(amount, recipient)
    wait_paid(game, fam)
    if game.page.evaluate(SHEET_FOCUS) != "continue":
        raise E2EError(f"the done step focuses {game.page.evaluate(SHEET_FOCUS)!r}, not Continue")
    game.hold("Enter")  # Continue with the keyboard
    state = wait_until(game, "the sheet to close and the event to end", finished)
    check_paid_event(state, "Tip")
    receipts = receipts_of(game.stored())
    tips = [r for r in receipts if r["kind"] == "tip"]
    if len(receipts) != 2 or len(tips) != 1 or tips[0]["tx_hash"] != tx:
        raise E2EError(f"storage should hold the unlock and the tip {tx}: {receipts}")
    print(f"2. manual transfer: copied {amount} USDC to {recipient}, paid from Anvil, found")


def pending_survives_reload(game: WebGame, fam) -> dict:
    """Returns what storage held before the reload."""
    page = game.page
    game.talk_to("Bard")
    game.wait_step("amount")
    game.click("forward")
    game.wait_step("method")
    # A new session is saved at once, not after the 10 s save interval: it is in storage before
    # the tab goes away.
    doc = wait_until(
        game,
        "the pending tip in storage",
        lambda d: bool(d.get("pending")),
        3,
        read=game.stored,
    )
    sid, session = next(iter(doc["pending"].items()))
    page.reload()  # the tab goes away with the sheet still open
    game.wait_booted()
    state = game.state()
    if state["switch"] is not True or not state["owns"]:
        raise E2EError(f"after the reload switch {OWNED_SWITCH} should be ON: {state}")
    after = game.stored()
    if len(receipts_of(after)) != 2 or sid not in (after.get("pending") or {}):
        raise E2EError(f"after the reload storage should hold 2 receipts and {sid}: {after}")
    # Watching resumed: pay the restored session from Anvil; its receipt arrives in the background.
    tx = fam.pay_manual(amount_of(session), fam.dev)
    after = wait_until(
        game,
        "the restored session's receipt",
        lambda d: len(receipts_of(d)) == 3,
        120,
        fam.tick,
        read=game.stored,
    )
    if tx not in [r["tx_hash"] for r in receipts_of(after)] or sid in (after.get("pending") or {}):
        raise E2EError(f"the restored session {sid} should be paid by {tx}: {after}")
    if game.state()["switch"] is not True:
        raise E2EError(f"switch {OWNED_SWITCH} went OFF")
    print(
        f"3. reload: 2 receipts and pending session {sid} kept, switch {OWNED_SWITCH} ON;"
        " paid after the reload, receipts=3"
    )
    return doc


# --- the run ---------------------------------------------------------------------------------


def build(node: str) -> None:
    done = subprocess.run(
        [node, "build.mjs"], cwd=RPGMAKER, capture_output=True, text=True, timeout=300
    )
    if done.returncode != 0:
        raise E2EError(f"the plugin build failed:\n{done.stdout}\n{done.stderr}")


def run(family: str, headed: bool) -> int:
    fam_cls = FAMILIES[family]
    exe = shutil.which(fam_cls.tool)
    if not exe:
        print(fam_cls.missing)
        return 2
    node, why = find_node()
    if node is None:
        print(why)
        return 2
    from playwright.sync_api import sync_playwright

    started = time.monotonic()
    work = Path(tempfile.mkdtemp(prefix="scfg-rpgmaker-web-e2e-"))
    fam = httpd = None
    console: list[str] = []
    try:
        build(node)
        url, httpd = serve(RPGMAKER)
        fam = fam_cls(exe, work)
        print(f"RPG Maker web e2e: {family}")
        with sync_playwright() as p:
            browser = launch_chromium(p, headless=not headed)
            # Chromium's Local Network Access check can block the page's fetch() to the loopback
            # Anvil (a test-environment matter: real games fetch public RPC endpoints). The
            # clipboard permissions let the runner read what the sheet's Copy buttons copied.
            context = browser.new_context(
                permissions=["local-network-access", "clipboard-read", "clipboard-write"],
                viewport={"width": 1280, "height": 720},
            )
            page = context.new_page()
            page.on("console", lambda m: console.append(f"[{m.type}] {m.text}"))
            page.on("pageerror", lambda e: console.append(f"[pageerror] {e}"))
            config = {"parameters": fam.parameters()}
            page.add_init_script(f"window.scfgE2EConfig = {json.dumps(json.dumps(config))};")
            install_mock_wallet(page, fam.rpc, fam.payer)
            page.goto(url + PAGE)
            game = WebGame(page)
            game.wait_booted()
            pay_unlock_with_overlay(game, fam)
            tip_by_manual_transfer(game, fam)
            before = pending_survives_reload(game, fam)
            game.check_running()
            print(
                f"   storage before the reload: {len(receipts_of(before))} receipts,"
                f" pending {sorted(before['pending'])}"
            )
            browser.close()
        bad = [line for line in console if BAD_CONSOLE.search(line)]
        if bad:
            raise E2EError("the page logged errors or warnings:\n" + "\n".join(bad))
    except Exception as e:
        print("\n".join(console[-80:]))
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(
            f"RPG Maker web e2e ({family}) FAILED after {time.monotonic() - started:.1f} s "
            f"(files in {work})"
        )
        return 1
    finally:
        if httpd is not None:
            httpd.shutdown()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"RPG Maker web e2e ({family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--family", choices=tuple(FAMILIES), default="evm")
    ap.add_argument("--headed", action="store_true", help="show the browser window")
    args = ap.parse_args(argv)
    return run(args.family, args.headed)


if __name__ == "__main__":
    raise SystemExit(main())
