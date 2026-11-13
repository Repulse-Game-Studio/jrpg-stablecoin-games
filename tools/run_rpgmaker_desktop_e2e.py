"""Run the RPG Maker MZ desktop end-to-end test: the desktop (NW.js) path of the SCFG plugin,
paying on a local chain.

NW.js cannot run here (no NW.js SDK and no RPG Maker MZ in the test setup), so the game is
rpgmaker/test/e2e/desktop_driver.ts under Node: the real glue (the sources SCFG.js bundles) on the
MZ shim as a desktop game, with the demo kit's plugin parameters (rpgmaker/demo/js/plugins.js)
pointed at the local chain, its saves in a temporary directory through MZ's StorageManager, the
real localhost checkout page on Node's net and the real Node https transport. Its sheet is a
headless host that reports what the plugin's sheet would show and presses its buttons by name;
the payments start from the demo map's events (the Merchant's Unlock Item, the Bard's Tip), whose
plugin commands store their status in variable 1. Per run:

  1. Unlock Chapter 2 by manual transfer: copy the amount and address from the sheet, pay them
     from the chain side, wait for "Payment confirmed", check SCFG.owns("chapter2"), switch 1
     (Chapter 2's owned switch), the status variable and the receipt in save/scfg.rmmzsave.
  2. Tip with the browser wallet: open the localhost page the game opened (nw.Shell.openExternal)
     in Playwright Chromium with a mock wallet (EIP-6963 for evm/arc, Wallet Standard for solana)
     and pay there.
  3. Restart the game on the same save folder: Chapter 2 is still owned, its switch ON.

The real sheet's DOM and input are exercised by the web e2e (tools/run_rpgmaker_web_e2e.py) and
tests/rpgmaker_ui; NW.js-only breakage (its `require`, `nw.Shell`) is covered only by unit fakes.

--family evm (default) pays USDC on Anvil (the testnet arbitrum entry pointed at a mock token);
--family arc pays native USDC on Anvil dressed as Arc; --family solana pays USDC on
solana-test-validator. They need `anvil` (Foundry) or `solana-test-validator` (Agave) on PATH,
Node 22.18+ with rpgmaker/node_modules, Playwright's Chromium, and `solders` (a dev dependency,
so run it with `uv run`):

    uv run python tools/run_rpgmaker_desktop_e2e.py [--family arc|solana]
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

import run_godot_e2e as desk  # noqa: E402  (also puts scfg_core and tests/integration on sys.path)
from js_driver import find_node  # noqa: E402
from run_godot_e2e import E2EError, launch_chromium  # noqa: E402

ROOT = TOOLS.parent
RPGMAKER = ROOT / "rpgmaker"
DRIVER = RPGMAKER / "test" / "e2e" / "desktop_driver.ts"
ITEM = "chapter2"
CONFIRMED = "Payment confirmed. Thank you!"
OWNED_SWITCH = "1"  # the demo kit: chapter2's Owned Switch
STATUS_VARIABLE = "1"  # the demo kit's events store their status here
PAID = 1  # the status the plugin commands store for a payment (SCFG.STATUS.paid)


# --- the chain families (the demo kit plays on testnets) ---------------------------------------


class Evm(desk.Evm):
    """USDC on Anvil: the testnet arbitrum entry pointed at a mock token."""

    def chain(self) -> dict:
        return {
            "network": "testnet",
            "key": "arbitrum",
            "rpc": [self.url],
            "confirmations": 2,
            "tokens": {"USDC": self.token},
        }

    def parameters(self) -> dict:
        return {
            "wallet": self.dev,
            "chains": json.dumps(["arbitrum"]),
            "tokens": json.dumps(["USDC"]),
        }


class Arc(desk.Arc):
    """Native USDC on Anvil dressed as Arc (the testnet arc entry, pointed at the local chain)."""

    def chain(self) -> dict:
        return {"network": "testnet", "key": "arc", "rpc": [self.url]}

    def parameters(self) -> dict:
        return {"wallet": self.dev, "chains": json.dumps(["arc"]), "tokens": json.dumps(["USDC"])}


class Solana(desk.Solana):
    """USDC on solana-test-validator: the testnet solana entry pointed at a mock mint."""

    def parameters(self) -> dict:
        return {
            "wallet": "",
            "solanaWallet": self.dev,
            "chains": json.dumps(["solana"]),
            "tokens": json.dumps(["USDC"]),
        }


FAMILIES = {"evm": Evm, "arc": Arc, "solana": Solana}


# --- the game --------------------------------------------------------------------------------


class Driver(desk.Game):
    """desktop_driver.ts in Node; `call(op, **args)` sends one command (the Godot runner's
    protocol: JSON lines in, `SCFG-REPLY ` lines out)."""

    def __init__(self, node: str, log: Path):  # (not Game's: no Godot)
        self.log_path = log
        self._log = log.open("wb")
        self._out = b""
        self._noise: list[str] = []
        self.proc = subprocess.Popen(
            [node, str(DRIVER)],
            cwd=RPGMAKER,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._log,
        )

    def output(self) -> str:
        if not self._log.closed:
            self._log.flush()
        return "".join(self._noise) + self.log_path.read_text(errors="replace")


def screen(snap: dict) -> dict | None:
    """The newest open screen's view (the payment sheet)."""
    return snap["screens"][-1] if snap["screens"] else None


def has_button(name: str, step: str | None = None):
    def pred(snap):
        view = screen(snap)
        return (
            view is not None
            and (step is None or view.get("step") == step)
            and any(b["name"] == name for b in view["buttons"])
        )

    return pred


def paid(snap: dict) -> bool:
    view = screen(snap)
    return view is not None and view.get("state") == "paid" and view.get("heading") == CONFIRMED


def closed_and_done(snap: dict) -> bool:
    return not snap["screens"] and not snap["eventRunning"]


def copy_value(game: Driver, name: str) -> str:
    """Press a Copy button of the sheet and read what it put on the (recorded) clipboard."""
    before = len(game.snapshot()["copied"])
    game.call("press", name=name)
    snap = desk.wait_for(game, f"{name} to copy", lambda s: len(s["copied"]) > before, 10)
    return snap["copied"][-1]


def stored(save_file: Path) -> dict:
    """The plugin's state document as MZ's StorageManager saved it."""
    return json.loads(save_file.read_text())


def check_event_result(snap: dict, what: str) -> None:
    status = snap["variables"][STATUS_VARIABLE]
    if status != PAID:
        raise E2EError(f"{what}: the event's status variable holds {status}, not {PAID} (paid)")


# --- the scenario ----------------------------------------------------------------------------


def unlock_by_manual_transfer(game: Driver, fam, save_file: Path) -> None:
    snap = desk.wait_for(game, "the payment engine", lambda s: s["transport"] is not None)
    if snap["transport"] != "NodeHttpsTransport":
        # On NW.js the RPC goes through Node's https first (fetch() depends on CORS headers)
        raise E2EError(f"the RPC goes through {snap['transport']}, not Node's https")
    if snap["switches"][OWNED_SWITCH] or game.call("owns", item=ITEM):
        raise E2EError(f"Chapter 2 is owned before the purchase: {snap}")
    game.call("event", name="Merchant")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    desk.wait_for(game, "the method step", has_button("method_manual", "method"))
    game.call("press", name="method_manual")
    snap = desk.wait_for(game, "the manual step", has_button("copy_amount", "manual"))
    amount = copy_value(game, "copy_amount")
    recipient = copy_value(game, "copy_recipient")
    token = copy_value(game, "copy_token")
    view = screen(game.snapshot())
    if (
        not view["amount"].startswith(amount + " ")
        or view["recipient"] != recipient
        or view["token_address"] != token
    ):
        raise E2EError(f"copied {amount!r} / {recipient!r} / {token!r}; the sheet shows {view}")
    fam.check_manual(recipient, token)
    print(f"  manual step: send {amount} USDC to {recipient}")
    game.call("press", name="forward")  # I've sent it
    desk.wait_for(game, "the waiting step", lambda s: (screen(s) or {}).get("step") == "waiting")
    tx = fam.pay_manual(amount, recipient)
    print(f"  paid from the chain side: {tx}")
    desk.wait_for(game, "the payment to be confirmed", paid, 120, fam.tick)
    if not game.call("owns", item=ITEM):
        raise E2EError("the sheet says paid but SCFG.owns('chapter2') is false")
    game.call("press", name="continue")
    snap = desk.wait_for(game, "the sheet to close and the event to end", closed_and_done)
    check_event_result(snap, "Unlock Item")
    if snap["switches"][OWNED_SWITCH] is not True:
        raise E2EError(f"switch {OWNED_SWITCH} (Chapter 2 owned) is not ON: {snap['switches']}")
    receipts = stored(save_file)["receipts"]
    found = [r for r in receipts.values() if r["item"] == ITEM and r["kind"] == "unlock"]
    if len(found) != 1 or found[0]["tx_hash"] != tx:
        raise E2EError(f"{save_file.name} has no receipt for {tx}: {receipts}")
    print(f"  unlocked; switch {OWNED_SWITCH} ON; {save_file.name} holds the receipt")


def tip_by_browser_wallet(game: Driver, fam, browser, save_file: Path) -> None:
    game.call("event", name="Bard")
    desk.wait_for(game, "the amount step", has_button("forward", "amount"))
    game.call("press", name="forward")  # Next: the event's 1.00 USDC is already chosen
    desk.wait_for(game, "the method step", has_button("method_browser", "method"))
    game.call("press", name="method_browser")
    desk.wait_for(game, "the browser step", has_button("forward", "browser"))
    opened = len(game.snapshot()["opened"])
    game.call("press", name="forward")  # Open checkout in browser
    url = desk.wait_for(
        game, "the game to open the browser", lambda s: len(s["opened"]) > opened, 20
    )["opened"][-1]
    if not url.startswith("http://127.0.0.1:"):
        raise E2EError(f"the game opened {url!r}, not its localhost page")
    print(f"  the game opened {url.split('?')[0]}...")
    page = browser.new_page()
    try:
        fam.install_wallet(page)
        page.goto(url)
        fam.connect(page).click()

        # The mock wallet's Python callbacks only run inside Playwright calls: wait in the page.
        def tick():
            page.wait_for_timeout(250)
            fam.tick()

        desk.wait_for(game, "the tip to be confirmed", paid, 120, tick)
        page.wait_for_function(
            "document.getElementById('status').textContent.includes('Payment confirmed')",
            timeout=20_000,
        )
    finally:
        page.close()
    tx = screen(game.snapshot())["tx"]
    game.call("press", name="continue")
    snap = desk.wait_for(game, "the sheet to close and the event to end", closed_and_done)
    check_event_result(snap, "Tip")
    receipts = stored(save_file)["receipts"]
    tips = [r for r in receipts.values() if r["kind"] == "tip"]
    if len(receipts) != 2 or len(tips) != 1 or tips[0]["tx_hash"] != tx:
        raise E2EError(f"{save_file.name} should hold the unlock and the tip {tx}: {receipts}")
    if not re.fullmatch(r"1\.\d+", tips[0]["amount"]):
        raise E2EError(f"the tip should be the 1 USDC asked for (and its digits): {tips[0]}")
    print(f"  tipped {tips[0]['amount']} USDC through the browser page; both receipts stored")


def still_owned_after_restart(game: Driver, save_file: Path) -> None:
    snap = game.snapshot()
    if not game.call("owns", item=ITEM) or snap["switches"][OWNED_SWITCH] is not True:
        raise E2EError(f"Chapter 2 is not owned after the restart: {snap}")
    if len(stored(save_file)["receipts"]) != 2:
        raise E2EError(f"{save_file.name} lost receipts: {stored(save_file)}")
    print(f"  restarted: Chapter 2 still owned, switch {OWNED_SWITCH} ON")


# --- the run ---------------------------------------------------------------------------------


def boot(node: str, fam, storage: Path, log: Path) -> Driver:
    game = Driver(node, log)
    try:
        info = game.call(
            "boot", storage=str(storage), parameters=fam.parameters(), chain=fam.chain()
        )
    except BaseException:
        game.close()
        raise
    if info != {"scene": "Scene_Map", "loaded": True, "disabled": None}:
        game.close()
        raise E2EError(f"the game did not boot to the map with payments on: {info}")
    return game


def quit_cleanly(game: Driver) -> str:
    code = game.quit()
    out = game.output()
    if code != 0:
        raise E2EError(f"the driver exited with code {code}:\n{out}")
    return out


def run(family: str) -> int:
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

    work = Path(tempfile.mkdtemp(prefix="scfg-rpgmaker-e2e-"))
    storage = work / "game"  # the game folder: MZ's save/ lands here
    save_file = storage / "save" / "scfg.rmmzsave"
    fam = game = None
    started = time.monotonic()
    try:
        fam = fam_cls(exe, work)
        print(f"RPG Maker desktop e2e: {family} (Node {node})")
        outputs = []
        with sync_playwright() as p:
            browser = launch_chromium(p)
            game = boot(node, fam, storage, work / "game1.log")
            unlock_by_manual_transfer(game, fam, save_file)
            tip_by_browser_wallet(game, fam, browser, save_file)
            outputs.append(quit_cleanly(game))
            game.close()
            game = None
            game = boot(node, fam, storage, work / "game2.log")
            still_owned_after_restart(game, save_file)
            outputs.append(quit_cleanly(game))
            browser.close()
        # A clean run prints nothing besides the replies: any line is the plugin's report
        # ("scfg: ..." through console.error / console.warn, test-play warnings included), a
        # failed frame, or a Node warning or error.
        bad = [line for out in outputs for line in out.splitlines() if line.strip()]
        if bad:
            raise E2EError("the game printed:\n" + "\n".join(bad))
    except Exception as e:
        if game is not None:
            print(game.output()[-4000:])
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(
            f"RPG Maker desktop e2e ({family}) FAILED after {time.monotonic() - started:.1f} s "
            f"(files in {work})"
        )
        return 1
    finally:
        if game is not None:
            game.close()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"RPG Maker desktop e2e ({family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--family", choices=sorted(FAMILIES), default="evm", help="chain family (default: evm)"
    )
    args = ap.parse_args(argv)
    return run(args.family)


if __name__ == "__main__":
    raise SystemExit(main())
