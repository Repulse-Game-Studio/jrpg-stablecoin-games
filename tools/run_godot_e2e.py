"""Run the Godot desktop end-to-end test: the demo game (godot/demo) with the addon's real payment
sheet, Worker and localhost checkout page, paying on a local chain.

The demo is played the way a player plays it, through godot/demo/e2e/e2e_driver.gd: the driver
clicks the buttons on the sheet and in the game with mouse events (and presses input actions),
and reports what the screen shows. Per run:

  0. Broken e2e configs (bad JSON, a config configure() refuses): the demo quits with code 1 by
     itself and runs nothing after that.
  1. Unlock Chapter 2 by manual transfer: copy the amount and address from the sheet, pay them
     from the chain side, wait for "Payment confirmed", check Scfg.owns("chapter2") and the
     receipt in user://scfg/state.json.
  2. Tip with the browser wallet: open the localhost page the game opens in Playwright Chromium
     with a mock wallet (EIP-6963 for evm/arc, Wallet Standard for solana) and pay there.
  3. Restart the game: Chapter 2 is still owned.

--family evm (default) pays USDC on Anvil (the arbitrum entry pointed at a mock token); --family
arc pays native USDC on Anvil dressed as Arc; --family solana pays USDC on solana-test-validator.
They need `anvil` (Foundry) or `solana-test-validator` (Agave) on PATH, Playwright's Chromium,
and `solders` (a dev dependency, so run it with `uv run`).

The game runs headless; with SCFG_E2E_VIDEODRIVER=x11 it opens a window instead, as the Ren'Py
e2e does. Its user data (HOME, XDG_*) lives in a temporary directory, never the developer's.

    uv run python tools/run_godot_e2e.py --godot /path/to/Godot_v4.3 [--family arc|solana]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from decimal import Decimal
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import run_renpy_e2e as chains  # noqa: E402  (also puts scfg_core and tests/integration on sys.path)
from sync_demo import sync_godot  # noqa: E402

from scfg_core.chains import ARC_SYSTEM_EMITTER, ARC_USDC  # noqa: E402
from scfg_core.coio import Sync  # noqa: E402
from scfg_core.rpc import RpcClient, UrllibTransport  # noqa: E402

from browser_mocks import install_mock_solana_wallet, install_mock_wallet  # noqa: E402
from mock_token import NATIVE_SEND_SELECTOR, TRANSFER_SELECTOR  # noqa: E402

ROOT = TOOLS.parent
DEMO = ROOT / "godot" / "demo"
OVERRIDE = DEMO / "override.cfg"  # git-ignored; written for the run, removed after it (the run
# refuses to start when one is already there, so a developer's own file is never deleted)
DRIVER = "res://e2e/e2e_driver.gd"
REPLY_PREFIX = b"SCFG-REPLY "
# The driver prints this when the game quits on its own while its stdin reader waits: the runner
# then closes the game's stdin, so the reader ends and the driver can join it (e2e_driver.gd).
QUITTING = b"SCFG-QUITTING\n"
REPLY_TIMEOUT = 30
ITEM = "chapter2"
ITEMS = {ITEM: {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"}}
CONFIRMED = "Payment confirmed. Thank you!"
UNLOCKED = "Chapter 2: unlocked. Thank you for supporting the game!"
# Engine output that means something went wrong: Godot prints "ERROR:" / "SCRIPT ERROR:" for
# push_error and script faults, "WARNING:" for push_warning, "leaked" for ObjectDB leaks at exit.
BAD_OUTPUT = re.compile(r"SCRIPT ERROR|^ERROR:|^WARNING:|leaked", re.M)
# Godot 4.4+ warns once per script of the fresh addon copy (sync_godot leaves out *.uid) whose
# UID its import cache still has; an "at:" line follows. The only warning an import may print.
UID_WARNING = re.compile(
    r'^WARNING: Missing \.uid file for path "res://[^"]+"\. The file was re-created from cache\.$'
)


def import_problems(log: str) -> list[str]:
    """The lines of an import (or export) log that mean something went wrong: every error and
    warning (BAD_OUTPUT) except the missing-.uid warning, with the "at:" line right under it."""
    problems = []
    after_uid_warning = False
    for line in log.splitlines():
        if after_uid_warning and line.lstrip().startswith("at: "):
            after_uid_warning = False
            continue
        after_uid_warning = bool(UID_WARNING.match(line))
        if not after_uid_warning and BAD_OUTPUT.search(line):
            problems.append(line)
    return problems


class E2EError(Exception):
    pass


def units_of(amount: str, decimals: int = 6) -> int:
    """The sheet's "1.000037" as token units."""
    value = Decimal(amount) * 10**decimals
    if value != value.to_integral_value():
        raise E2EError(f"amount {amount!r} has more than {decimals} decimals")
    return int(value)


# --- the chain families ----------------------------------------------------------------------


class Evm:
    """USDC on Anvil: the built-in arbitrum entry pointed at a mock token."""

    tool, missing = "anvil", "anvil not found on PATH (install Foundry: https://getfoundry.sh)"
    wallet_button = "Pay with Mock Wallet"
    setup = staticmethod(chains.setup_evm)

    def __init__(self, exe: str, workdir: Path):
        env, self.cleanup = self.setup(exe, workdir)
        self.url, self.token = env["SCFG_E2E_RPC"], env["SCFG_E2E_USDC"]
        self.dev, self.payer = env["SCFG_E2E_DEV"], env["SCFG_E2E_PAYER"]
        self.rpc = Sync(RpcClient([self.url]), UrllibTransport())

    def config(self) -> dict:
        return {
            "configure": {"wallet": self.dev, "chains": ["arbitrum"], "tokens": ["USDC"]},
            "items": ITEMS,
        }

    def chain(self) -> dict:
        return {
            "network": "mainnet",
            "key": "arbitrum",
            "rpc": [self.url],
            "confirmations": 2,
            "tokens": {"USDC": self.token},
        }

    def check_manual(self, recipient: str, token: str) -> None:
        if recipient.lower() != self.dev.lower() or token.lower() != self.token.lower():
            raise E2EError(
                f"the sheet asks for {token} to {recipient}, not {self.token} to {self.dev}"
            )

    def pay_manual(self, amount: str, recipient: str) -> str:
        """An ERC-20 transfer of the shown amount to the shown address."""
        data = (
            "0x"
            + TRANSFER_SELECTOR
            + "0" * 24
            + recipient[2:].lower()
            + format(units_of(amount), "064x")
        )
        tx = self.rpc.call(
            "eth_sendTransaction", [{"from": self.payer, "to": self.token, "data": data}]
        )
        chains.wait_receipt(self.rpc, tx)
        return tx

    def tick(self) -> None:
        """A live chain: one more block (confirmations)."""
        self.rpc.call("anvil_mine", ["0x1"])

    def install_wallet(self, page) -> None:
        install_mock_wallet(page, self.rpc, self.payer)

    def connect(self, page):
        page.get_by_role("button", name=self.wallet_button).click()
        pay = page.get_by_role("button", name=re.compile(r"^Pay \d"))
        pay.wait_for()
        return pay


class Arc(Evm):
    """Native USDC on Anvil dressed as Arc (the real arc entry, pointed at the local chain)."""

    setup = staticmethod(chains.setup_arc)

    def config(self) -> dict:
        return {
            "configure": {"wallet": self.dev, "chains": ["arc"], "tokens": ["USDC"]},
            "items": ITEMS,
        }

    def chain(self) -> dict:
        return {"network": "mainnet", "key": "arc", "rpc": [self.url]}

    def check_manual(self, recipient: str, token: str) -> None:
        if recipient.lower() != self.dev.lower() or token.lower() != ARC_USDC.lower():
            raise E2EError(
                f"the sheet asks for {token} to {recipient}, not {ARC_USDC} to {self.dev}"
            )

    def pay_manual(self, amount: str, recipient: str) -> str:
        """A wallet "Send" of native USDC (18 decimals): only the system emitter logs it."""
        tx = self.rpc.call(
            "eth_sendTransaction",
            [
                {
                    "from": self.payer,
                    "to": ARC_SYSTEM_EMITTER,
                    "data": "0x" + NATIVE_SEND_SELECTOR + "0" * 24 + recipient[2:].lower(),
                    "value": hex(units_of(amount) * 10**12),
                    "gas": hex(200_000),
                }
            ],
        )
        chains.wait_receipt(self.rpc, tx)
        return tx


class Solana:
    """USDC on solana-test-validator: the built-in testnet solana entry pointed at a mock mint."""

    tool = "solana-test-validator"
    missing = (
        "solana-test-validator not found on PATH "
        "(install an Agave release: https://github.com/anza-xyz/agave/releases)"
    )
    wallet_button = "Pay with Mock Solana Wallet"

    def __init__(self, exe: str, workdir: Path):
        self.cluster, self.mint, dev, self.player = chains.solana_world(exe)
        self.cleanup = self.cluster.close
        try:
            self.dev = str(dev.pubkey())
            self.cluster.airdrop(
                self.player.pubkey(), 2 * 10**9
            )  # the browser wallet pays its own fee
        except BaseException:
            self.cleanup()
            raise

    def config(self) -> dict:
        return {
            "configure": {
                "solana_wallet": self.dev,
                "testnet": True,
                "chains": ["solana"],
                "tokens": ["USDC"],
            },
            "items": ITEMS,
        }

    def chain(self) -> dict:
        return {
            "network": "testnet",
            "key": "solana",
            "rpc": [self.cluster.url],
            "tokens": {"USDC": self.mint},
        }

    def check_manual(self, recipient: str, token: str) -> None:
        if recipient != self.dev or token != self.mint:
            raise E2EError(
                f"the sheet asks for {token} to {recipient}, not {self.mint} to {self.dev}"
            )

    def pay_manual(self, amount: str, recipient: str) -> str:
        """A plain SPL transfer of the shown amount (no Solana Pay reference, as an exchange)."""
        return self.cluster.transfer(self.player, self.mint, recipient, units_of(amount))

    def tick(self) -> None:
        pass  # the validator makes its own blocks

    def install_wallet(self, page) -> None:
        install_mock_solana_wallet(page, self.cluster, self.player)

    def connect(self, page):
        page.get_by_role("button", name=self.wallet_button).click()
        pay = page.locator("#pay")
        pay.wait_for()
        return pay


FAMILIES = {"evm": Evm, "arc": Arc, "solana": Solana}


# --- the game --------------------------------------------------------------------------------


class Game:
    """The demo running with the e2e driver; `call(op, **args)` sends one driver command."""

    def __init__(self, godot: str, env: dict, log: Path):
        windowed = os.environ.get("SCFG_E2E_VIDEODRIVER", "")
        display = (
            ["--display-driver", windowed, "--audio-driver", "Dummy"]
            if windowed
            else ["--headless"]
        )
        self.log_path = log
        self._log = log.open("wb")
        self._out = b""
        self._noise: list[str] = []
        self.proc = subprocess.Popen(
            [godot, *display, "--path", str(DEMO)],
            cwd=ROOT,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._log,
        )

    def call(self, op: str, **args):
        line = json.dumps({"op": op, **args}).encode()
        try:
            self.proc.stdin.write(line + b"\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            raise E2EError(f"the game exited before {op}:\n{self.output()}") from None
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            out = self._read_line(deadline)
            if out is None:
                raise E2EError(f"the driver gave no reply to {op}:\n{self.output()}")
            if out.startswith(REPLY_PREFIX):
                break
            self._noise.append(out.decode(errors="replace"))
            if out == QUITTING:
                self._close_stdin()  # the game is quitting on its own: EOF follows
        reply = json.loads(out[len(REPLY_PREFIX) :])
        if "error" in reply:
            raise E2EError(f"{op} {args}: {reply['error']}")
        return reply["ok"]

    def _close_stdin(self) -> None:
        if self.proc.stdin and not self.proc.stdin.closed:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass

    def wait_exit(self, timeout: float) -> int:
        """Wait for a game that quits on its own (closing its stdin when it asks); returns the
        exit code."""
        deadline = time.monotonic() + timeout
        while (out := self._read_line(deadline)) is not None:
            self._noise.append(out.decode(errors="replace"))
            if out == QUITTING:
                self._close_stdin()
        try:
            return self.proc.wait(max(deadline - time.monotonic(), 0.1))
        except subprocess.TimeoutExpired:
            raise E2EError(
                f"the game did not quit within {timeout:g} s:\n{self.output()}"
            ) from None

    def _read_line(self, deadline):
        fd = self.proc.stdout.fileno()
        while b"\n" not in self._out:
            ready, _, _ = select.select([fd], [], [], max(deadline - time.monotonic(), 0))
            chunk = os.read(fd, 1 << 16) if ready else b""
            if not chunk:
                return None
            self._out += chunk
        line, _, self._out = self._out.partition(b"\n")
        return line + b"\n"

    def snapshot(self) -> dict:
        return self.call("snapshot")

    def click(self, name: str, where: str = "screen"):
        return self.call("click", name=name, **{"in": where})

    def quit(self) -> int:
        """Quit the normal way; returns the exit code."""
        self.call("quit")
        self._close_stdin()
        try:
            code = self.proc.wait(30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise E2EError(f"the game did not quit:\n{self.output()}") from None
        self._drain()
        return code

    def _drain(self) -> None:
        rest = self.proc.stdout.read() if self.proc.stdout else b""
        self._noise.extend(
            x.decode(errors="replace") + "\n" for x in (self._out + rest).splitlines()
        )
        self._out = b""

    def output(self) -> str:
        self._log.flush()
        return "".join(self._noise) + self.log_path.read_text(errors="replace")

    def close(self) -> None:
        if self.proc.poll() is None:
            self._close_stdin()  # a game waiting to join its stdin reader exits by itself
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc.stdout.close()
        self._log.close()


def wait_for(game: Game, what: str, pred, timeout: float = 60, tick=None):
    """Snapshots until pred(snapshot) holds; `tick()` runs between them (a block, a page wait)."""
    deadline = time.monotonic() + timeout
    snap = None
    while time.monotonic() < deadline:
        snap = game.snapshot()
        if pred(snap):
            return snap
        if tick is not None:
            tick()
        else:
            time.sleep(0.1)
    raise E2EError(f"timed out waiting for {what}; the screen shows:\n{json.dumps(snap, indent=1)}")


def sheet(snap: dict) -> dict | None:
    """The newest open payment sheet's view."""
    for view in reversed(snap["screens"]):
        if view["kind"] == "sheet":
            return view
    return None


def button(view: dict | None, name: str) -> dict | None:
    for b in (view or {}).get("buttons", []):
        if b["name"] == name:
            return b
    return None


def has_button(name: str, text: str | None = None):
    def pred(snap):
        b = button(sheet(snap), name)
        return b is not None and not b["disabled"] and (text is None or b["text"] == text)

    return pred


def shows(text: str):
    return lambda snap: text in (sheet(snap) or {}).get("texts", [])


def no_screen(snap: dict) -> bool:
    return not snap["screens"]


def copy_value(game: Game, name: str) -> str:
    """Press a Copy button on the sheet and read what it put on the clipboard."""
    before = len(game.snapshot()["copied"])
    game.click(name)
    return wait_for(game, f"{name} to copy", lambda s: len(s["copied"]) > before, 10)["copied"][-1]


# --- the scenario ----------------------------------------------------------------------------


def unlock_by_manual_transfer(game: Game, fam, state_file: Path) -> None:
    wait_for(game, "the Unlock button", lambda s: button(s["game"], "Unlock") is not None)
    game.click("Unlock", "game")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    snap = wait_for(game, "the method step", has_button("method_manual"))
    if not snap["paused"]:
        raise E2EError("the demo should pause the game while the sheet is open")
    game.click("method_manual")
    wait_for(game, "the manual step", has_button("copy_amount"))
    amount = copy_value(game, "copy_amount")
    recipient = copy_value(game, "copy_recipient")
    token = copy_value(game, "copy_token")
    texts = sheet(game.snapshot())["texts"]
    if not any(t.startswith(amount + " ") for t in texts) or recipient not in texts:
        raise E2EError(f"copied {amount!r} / {recipient!r}, which the sheet does not show: {texts}")
    fam.check_manual(recipient, token)
    print(f"  manual step: send {amount} USDC to {recipient}")
    game.click("forward")  # I've sent it
    wait_for(
        game,
        "the waiting step",
        lambda s: any(
            t.startswith("Waiting for your payment") for t in (sheet(s) or {}).get("texts", [])
        ),
    )
    tx = fam.pay_manual(amount, recipient)
    print(f"  paid from the chain side: {tx}")
    snap = wait_for(game, "the payment to be confirmed", shows(CONFIRMED), 120, fam.tick)
    if not game.call("owns", item=ITEM):
        raise E2EError("the sheet says paid but Scfg.owns('chapter2') is false")
    # Continue with the keyboard: the done step focuses it.
    if sheet(snap)["focus"] != "continue":
        raise E2EError(f"the done step focuses {sheet(snap)['focus']!r}, not Continue")
    game.call("action", action="ui_accept")
    snap = wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"])
    if button(snap["game"], "Unlock") is not None:
        raise E2EError("the Unlock button is still shown after the purchase")
    receipts = json.loads(state_file.read_text())["receipts"]
    found = [r for r in receipts.values() if r["item"] == ITEM and r["kind"] == "unlock"]
    if len(found) != 1 or found[0]["tx_hash"] != tx:
        raise E2EError(f"state.json has no receipt for {tx}: {receipts}")
    print(f"  unlocked; state.json holds the receipt ({state_file})")


def tip_by_browser_wallet(game: Game, fam, browser, state_file: Path) -> None:
    game.click("Tip", "game")
    wait_for(game, "the amount step", has_button("forward", "Next"))
    game.click("forward")  # Next: the 1 USDC the demo asks for is already chosen
    wait_for(game, "the method step", has_button("method_browser"))
    game.click("method_browser")
    wait_for(game, "the browser step", has_button("forward", "Open checkout in browser"))
    opened = len(game.snapshot()["opened"])
    game.click("forward")
    url = wait_for(game, "the game to open the browser", lambda s: len(s["opened"]) > opened, 20)[
        "opened"
    ][-1]
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

        wait_for(game, "the tip to be confirmed", shows(CONFIRMED), 120, tick)
        page.wait_for_function(
            "document.getElementById('status').textContent.includes('Payment confirmed')",
            timeout=20_000,
        )
    finally:
        page.close()
    game.click("continue")
    wait_for(game, "the sheet to close", no_screen)
    # The demo thanks for the amount received: the 1 USDC plus the digits that identify the payment.
    thanks = re.compile(r"^Thank you for the (1\.\d+) USDC tip!$")
    snap = wait_for(
        game, "the thank-you", lambda s: any(thanks.match(t) for t in s["game"]["texts"])
    )
    shown = next(thanks.match(t)[1] for t in snap["game"]["texts"] if thanks.match(t))
    receipts = json.loads(state_file.read_text())["receipts"]
    tips = [r for r in receipts.values() if r["kind"] == "tip"]
    if len(receipts) != 2 or len(tips) != 1 or tips[0]["amount"] != shown:
        raise E2EError(f"state.json should hold the unlock and the {shown} USDC tip: {receipts}")
    print("  tipped through the browser page; state.json holds both receipts")


# Broken test hooks (the file's text, what the error printed for it says): the demo must refuse
# each one and quit with exit code 1, never fall back to its built-in testnet config.
BAD_HOOKS = (
    ("[1, 2]", "not a JSON object"),
    # a config configure() refuses (the addon prints "scfg: ...")
    (json.dumps({"configure": {"wallet": "0x123"}, "items": {}}), "scfg: "),
)


def bad_hooks_quit_at_once(godot: str, env: dict, work: Path) -> None:
    """The demo with each BAD_HOOKS config, driver loaded, as a failed run starts: it prints the
    reason and quits with code 1 by itself (the driver joins its stdin reader: no kill, no thread
    warning), and runs nothing after the quit (no "configure first" from the rest of _ready)."""
    for i, (text, reason) in enumerate(BAD_HOOKS):
        config = work / f"bad_hook_{i}.json"
        config.write_text(text)
        game = Game(godot, {**env, "SCFG_E2E_CONFIG": str(config)}, work / f"bad_hook_{i}.log")
        try:
            code = game.wait_exit(60)
            out = game.output()
        finally:
            game.close()
        bad = [line for line in out.splitlines() if BAD_OUTPUT.search(line)]
        if (
            code != 1
            or not bad
            or not all(reason in line for line in bad)
            or "Scfg.configure(...) first" in out
        ):
            raise E2EError(
                f"with the e2e config {text!r} the demo should print why ({reason!r}) "
                f"and quit with code 1 (it exited with {code}):\n{out}"
            )
    print(f"0. bad e2e configs: the demo quits with code 1 ({len(BAD_HOOKS)} cases)")


def still_owned_after_restart(game: Game) -> None:
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"])
    if button(snap["game"], "Unlock") is not None or not game.call("owns", item=ITEM):
        raise E2EError("Chapter 2 is not owned after the restart")
    print("  restarted: Chapter 2 is still owned")


# --- the run ---------------------------------------------------------------------------------


def launch_chromium(p):
    """The installed Chrome, else Playwright's Chromium (as the browser integration tests do)."""
    try:
        return p.chromium.launch(channel="chrome", headless=True)
    except Exception:
        return p.chromium.launch(headless=True)


def quit_cleanly(game: Game) -> str:
    """Quit the game; returns everything it printed."""
    code = game.quit()
    out = game.output()
    if code != 0:
        raise E2EError(f"the game exited with code {code}:\n{out}")
    return out


def run(godot: str, family: str) -> int:
    if OVERRIDE.exists():
        # The run writes this file to add the driver autoload and deletes it afterwards.
        print(
            f"Godot e2e: {OVERRIDE} already exists. The run writes its own override.cfg there and "
            "deletes it afterwards, so it will not start over yours: move it away and run again."
        )
        return 2
    fam_cls = FAMILIES[family]
    exe = shutil.which(fam_cls.tool)
    if not exe:
        print(fam_cls.missing)
        return 2
    from playwright.sync_api import sync_playwright

    work = Path(tempfile.mkdtemp(prefix="scfg-godot-e2e-"))
    home = work / "home"
    env = dict(os.environ)
    # The game's user:// (and the editor settings --import writes) stay in the run's own HOME.
    env.update(
        {
            "HOME": str(home),
            "XDG_DATA_HOME": str(home / "data"),
            "XDG_CONFIG_HOME": str(home / "config"),
            "XDG_CACHE_HOME": str(home / "cache"),
        }
    )
    fam = None
    game = None
    wrote_override = False
    started = time.monotonic()
    try:
        sync_godot()
        imported = subprocess.run(
            [godot, "--headless", "--path", str(DEMO), "--import"],
            env=env,
            capture_output=True,
            text=True,
            timeout=300,
        )
        log = imported.stdout + imported.stderr
        if imported.returncode != 0 or import_problems(log):
            raise E2EError(f"the demo did not import cleanly:\n{log[-4000:]}")
        outputs = []
        fam = fam_cls(exe, work)
        config = work / "e2e_config.json"
        config.write_text(json.dumps(fam.config()))
        env.update({"SCFG_E2E_CONFIG": str(config), "SCFG_E2E_CHAIN": json.dumps(fam.chain())})
        with open(OVERRIDE, "x") as f:  # never over a file that appeared since the check
            wrote_override = True
            f.write(
                "; written by tools/run_godot_e2e.py for one run (git-ignored)\n"
                f'[autoload]\n\nScfgE2EDriver="*{DRIVER}"\n'
            )
        print(f"Godot e2e: {family} on {godot}")
        bad_hooks_quit_at_once(godot, env, work)
        with sync_playwright() as p:
            browser = launch_chromium(p)
            game = Game(godot, env, work / "game1.log")
            state_file = Path(game.call("user_dir")) / "scfg" / "state.json"
            if not state_file.is_relative_to(home):
                raise E2EError(f"user data is not isolated: {state_file}")
            unlock_by_manual_transfer(game, fam, state_file)
            tip_by_browser_wallet(game, fam, browser, state_file)
            outputs.append(quit_cleanly(game))
            game.close()
            game = Game(godot, env, work / "game2.log")
            still_owned_after_restart(game)
            outputs.append(quit_cleanly(game))
            browser.close()
        bad = [line for out in outputs for line in out.splitlines() if BAD_OUTPUT.search(line)]
        if bad:
            raise E2EError("Godot printed errors or warnings:\n" + "\n".join(bad))
    except Exception as e:
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(
            f"Godot e2e ({family}) FAILED after {time.monotonic() - started:.1f} s (logs in {work})"
        )
        return 1
    finally:
        if wrote_override:
            OVERRIDE.unlink(missing_ok=True)
        if game is not None:
            game.close()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"Godot e2e ({family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--godot", required=True, help="Godot 4.3+ editor binary")
    ap.add_argument(
        "--family", choices=sorted(FAMILIES), default="evm", help="chain family (default: evm)"
    )
    args = ap.parse_args()
    return run(args.godot, args.family)


if __name__ == "__main__":
    raise SystemExit(main())
