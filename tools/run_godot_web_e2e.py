"""Run the Godot web end-to-end test: the demo (godot/demo) exported for the web, served on
localhost and played in Playwright Chromium against a local Anvil chain.

The export is single-threaded (the demo's "Web" preset), so it needs no COOP/COEP headers. The
page gets a mock EIP-6963 wallet backed by Anvil, and the demo's web e2e config
(window.scfgE2EConfig). The game is driven through window.scfgE2E, which the demo's e2e driver
(godot/demo/e2e/e2e_driver.gd) registers only under that config: it reports what the screen
shows and where each button is, and this runner clicks there with the browser's own mouse and
keyboard. The checkout overlay is clicked in the DOM. First a check of the hook itself, then
the three steps of the Ren'Py web e2e, then one for shared origins:

  0. a window.scfgE2EConfig that is not a string makes the demo quit (no fallback, no driver);
  1. browser wallet: the in-page checkout overlay pays "chapter2", which becomes owned;
  2. manual transfer: a tip copied from the sheet and sent straight from Anvil is found;
  3. reload: after the tab reloads, the receipts and an unpaid pending session are still there
     (in IndexedDB within IDB_WITHIN_S seconds of the method step, read back before the reload),
     and paying that session from Anvil after the reload is found too;
  4. shared origin: the receipts' copy in localStorage (ScfgWebMirror) holds the receipts; with
     the game's page closed, state.json is deleted from IndexedDB (what another Godot game on the
     same origin can do), and after the game loads again every receipt is back from the copy and
     saved to IndexedDB again.

--family arc runs the same steps on Anvil dressed as Arc; its manual payments are native USDC
sends, which only Arc's system emitter logs.

The web build has no process environment, so the desktop driver's SCFG_E2E_CHAIN does not exist
here. The game reaches the local chain through the public configure options only: `rpc_urls`
points the built-in chain at Anvil. Arc's USDC interface and system emitter already sit at their
real addresses (run_renpy_e2e.setup_arc); for --family evm the mock token's code is placed with
anvil_setCode at the real Arbitrum USDC address, so the built-in arbitrum entry is used as is.

Needs anvil (Foundry), Playwright's Chromium, and the web export templates of this Godot
version (tools/fetch_godot.py --templates). Usage:

    uv run python tools/run_godot_web_e2e.py --godot /path/to/Godot_v4.3 [--family arc]
        [--templates-dir DIR] [--headed]

--templates-dir is the data directory the templates were installed under (fetch_godot's
--templates-dir; default: $XDG_DATA_HOME or ~/.local/share). The run itself uses a temporary
HOME and XDG directories, and exports a temporary copy of the demo with the driver as an extra
autoload (an export ignores override.cfg), so nothing in godot/demo changes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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

import fetch_godot  # noqa: E402
import run_godot_e2e as desk  # noqa: E402  (also puts scfg_core and tests/integration on sys.path)
import run_renpy_e2e as chains  # noqa: E402
from run_godot_e2e import (  # noqa: E402
    CONFIRMED,
    DRIVER,
    ITEM,
    ITEMS,
    UNLOCKED,
    E2EError,
    button,
    has_button,
    import_problems,
    no_screen,
    sheet,
    shows,
)
from run_renpy_web_e2e import serve  # noqa: E402
from sync_demo import sync_godot  # noqa: E402

from scfg_core.chains import MAINNET  # noqa: E402

from browser_mocks import install_mock_wallet  # noqa: E402

ROOT = TOOLS.parent
DEMO = ROOT / "godot" / "demo"
PRESET = "Web"
REPLY_TIMEOUT_MS = 30_000
STATE = "scfg/state.json"
# Step 3: a new session must be in IndexedDB this soon after the method step shows (the autoload
# saves it at once and Godot copies user:// to IndexedDB right after), or a tab closed then
# loses it.
IDB_WITHIN_S = 3
# Godot's web output that means something went wrong: push_error / script faults ("ERROR:",
# "SCRIPT ERROR:"), push_warning ("WARNING:"), and uncaught page errors.
BAD_CONSOLE = re.compile(r"SCRIPT ERROR|^ERROR:|^WARNING:|^USER (ERROR|WARNING):|^\[pageerror\]")

# A file of the game's user:// as Emscripten's IDBFS stored it in IndexedDB (database
# "/userfs", store FILE_DATA, keyed by the full path), as text; null if absent.
IDB_READ = """async (path) => {
  const db = await new Promise((ok, fail) => {
    const r = indexedDB.open("/userfs");
    r.onsuccess = () => ok(r.result);
    r.onerror = () => fail(r.error);
  });
  try {
    const entry = await new Promise((ok, fail) => {
      const r = db.transaction("FILE_DATA").objectStore("FILE_DATA").get(path);
      r.onsuccess = () => ok(r.result);
      r.onerror = () => fail(r.error);
    });
    return entry && entry.contents ? new TextDecoder().decode(entry.contents) : null;
  } finally {
    db.close();
  }
}"""

# Deletes files of the game's user:// from IndexedDB, as another game's IDBFS sync on the same
# origin can. Run on a page of the origin where the game is not running.
IDB_DELETE = """async (paths) => {
  const db = await new Promise((ok, fail) => {
    const r = indexedDB.open("/userfs");
    r.onsuccess = () => ok(r.result);
    r.onerror = () => fail(r.error);
  });
  try {
    await new Promise((ok, fail) => {
      const t = db.transaction("FILE_DATA", "readwrite");
      for (const p of paths) t.objectStore("FILE_DATA").delete(p);
      t.oncomplete = () => ok();
      t.onerror = () => fail(t.error);
    });
  } finally {
    db.close();
  }
}"""

# The page's localStorage entries whose key starts with "scfg:", as {key: text}.
MIRRORS = """() => {
  const out = {};
  for (let i = 0; i < localStorage.length; i++) {
    const k = localStorage.key(i);
    if (k.startsWith("scfg:")) out[k] = localStorage.getItem(k);
  }
  return out;
}"""

# A page of the game's origin without the game (step 4 edits IndexedDB from there).
BLANK = "e2e-blank.html"

# What the page keeps in IndexedDB: every database, and every path in /userfs.
IDB_LIST = """async () => {
  const dbs = (await indexedDB.databases()).map((d) => d.name);
  if (!dbs.includes("/userfs")) return { dbs, paths: [] };
  const db = await new Promise((ok, fail) => {
    const r = indexedDB.open("/userfs");
    r.onsuccess = () => ok(r.result);
    r.onerror = () => fail(r.error);
  });
  try {
    const paths = await new Promise((ok, fail) => {
      const r = db.transaction("FILE_DATA").objectStore("FILE_DATA").getAllKeys();
      r.onsuccess = () => ok(r.result);
      r.onerror = () => fail(r.error);
    });
    return { dbs, paths: paths.map(String).sort() };
  } finally {
    db.close();
  }
}"""


# --- the chain families ----------------------------------------------------------------------


class Evm(desk.Evm):
    """USDC on Anvil: the mock token's code at the real Arbitrum USDC address, so the built-in
    arbitrum entry is used with only its RPC (and confirmations) configured. `network` is the
    chain table whose address is used (the RPG Maker web e2e plays on testnet: TESTNET)."""

    network = MAINNET

    def __init__(self, exe: str, workdir: Path):
        super().__init__(exe, workdir)  # Anvil with the mock token deployed at self.token
        try:
            code = self.rpc.call("eth_getCode", [self.token, "latest"])
            self.token = self.network["arbitrum"].tokens["USDC"].address
            self.rpc.call("anvil_setCode", [self.token, code])
            chains.mint(self.rpc, self.payer, self.token, self.payer)  # (anyone may mint)
        except BaseException:
            self.cleanup()
            raise

    def config(self) -> dict:
        return {
            "configure": {
                "wallet": self.dev,
                "chains": ["arbitrum"],
                "tokens": ["USDC"],
                "rpc_urls": {"arbitrum": [self.url]},
                "confirmations": {"arbitrum": 2},
            },
            "items": ITEMS,
        }


class Arc(desk.Arc):
    """Native USDC on Anvil dressed as Arc: the built-in arc entry with its RPC configured."""

    def config(self) -> dict:
        return {
            "configure": {
                "wallet": self.dev,
                "chains": ["arc"],
                "tokens": ["USDC"],
                "rpc_urls": {"arc": [self.url]},
            },
            "items": ITEMS,
        }


FAMILIES = {"evm": Evm, "arc": Arc}


# --- the export ------------------------------------------------------------------------------


def godot_version(godot: str) -> tuple[str, str]:
    """(release tag, templates directory name) of a Godot binary: ("4.3-stable", "4.3.stable")."""
    out = subprocess.run([godot, "--version"], capture_output=True, text=True, timeout=60).stdout
    m = re.match(r"^(\d+\.\d+(?:\.\d+)*)\.([a-z]+\d*)\.", out.strip())
    if not m:
        raise E2EError(f"cannot read the Godot version from {out!r}")
    return f"{m[1]}-{m[2]}", f"{m[1]}.{m[2]}"


def link_templates(templates_dir, version_dir: str, tag: str, data_home: Path) -> None:
    """Make the installed web templates visible under the run's XDG_DATA_HOME."""
    src = fetch_godot.data_dir(templates_dir) / "godot" / "export_templates" / version_dir
    if not (src / "web_nothreads_release.zip").is_file():
        raise E2EError(
            f"no web export templates in {src}; install them with\n"
            f"  uv run python tools/fetch_godot.py --version {tag} --templates"
            + (f" --templates-dir {templates_dir}" if templates_dir else "")
        )
    dst = data_home / "godot" / "export_templates"
    dst.mkdir(parents=True, exist_ok=True)
    (dst / version_dir).symlink_to(src.resolve(), target_is_directory=True)


def demo_copy(work: Path) -> Path:
    """A copy of the synced demo whose project.godot adds the e2e driver as an autoload, and whose
    Web preset exports it (the demo's own preset leaves e2e/* out)."""
    copy = work / "demo"
    shutil.copytree(DEMO, copy, ignore=shutil.ignore_patterns("override.cfg", "*.uid"))
    project = copy / "project.godot"
    text = project.read_text()
    autoload = '[autoload]\n\nScfg="*res://addons/scfg/scfg.gd"\n'
    if autoload not in text:
        raise E2EError("godot/demo/project.godot no longer lists the Scfg autoload as expected")
    project.write_text(text.replace(autoload, autoload + f'ScfgE2EDriver="*{DRIVER}"\n'))
    presets = copy / "export_presets.cfg"
    text = presets.read_text()
    exclude = 'exclude_filter="e2e/*"\n'
    if text.count(exclude) != 1:
        raise E2EError(
            'godot/demo/export_presets.cfg no longer has exclude_filter="e2e/*" as expected'
        )
    presets.write_text(text.replace(exclude, 'exclude_filter=""\n'))
    return copy


def export_web(godot: str, project: Path, out: Path, env: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for args, what in (
        (["--import"], "import"),
        (["--export-release", PRESET, str(out / "index.html")], "export"),
    ):
        done = subprocess.run(
            [godot, "--headless", "--path", str(project), *args],
            env=env,
            capture_output=True,
            text=True,
            timeout=600,
        )
        log = done.stdout + done.stderr
        if done.returncode != 0 or import_problems(log):
            raise E2EError(f"the demo's web {what} failed (code {done.returncode}):\n{log[-4000:]}")
    if not (out / "index.pck").is_file():
        raise E2EError(f"the export wrote no index.pck in {out}")


# --- the game in the page --------------------------------------------------------------------


class WebGame:
    """The exported demo in a Playwright page; `call(op, **args)` sends one driver command."""

    def __init__(self, page):
        self.page = page

    def wait_hook(self, timeout_ms: int = 120_000) -> None:
        self.page.wait_for_function(
            "typeof window.scfgE2E === 'function' && Array.isArray(window.scfgE2EReplies)",
            timeout=timeout_ms,
        )

    def call(self, op: str, **args):
        line = json.dumps({"op": op, **args})
        n = self.page.evaluate(
            "(line) => { const n = window.scfgE2EReplies.length; window.scfgE2E(line); return n; }",
            line,
        )
        self.page.wait_for_function(
            "(n) => window.scfgE2EReplies.length > n", arg=n, timeout=REPLY_TIMEOUT_MS
        )
        reply = json.loads(self.page.evaluate("(n) => window.scfgE2EReplies[n]", n))
        if "error" in reply:
            raise E2EError(f"{op} {args}: {reply['error']}")
        return reply["ok"]

    def snapshot(self) -> dict:
        return self.call("snapshot")

    def click(self, name: str, where: str = "screen") -> None:
        """A real browser click at the button (the driver scrolls it into view first)."""
        at = self.call("click", name=name, press=False, **{"in": where})["at"]
        self.page.mouse.click(at[0], at[1])

    def state(self) -> dict:
        return self.call("state") or {}


def wait_for(game: WebGame, what: str, pred, timeout: float = 60, tick=None):
    """Snapshots until pred(snapshot) holds; between them the page runs for a moment (the
    mock wallet's callbacks only run inside Playwright calls) and `tick()` runs (a block)."""
    deadline = time.monotonic() + timeout
    snap = None
    while time.monotonic() < deadline:
        snap = game.snapshot()
        if pred(snap):
            return snap
        game.page.wait_for_timeout(200)
        if tick is not None:
            tick()
    raise E2EError(f"timed out waiting for {what}; the screen shows:\n{json.dumps(snap, indent=1)}")


def wait_state(game: WebGame, what: str, pred, timeout: float = 60, tick=None) -> dict:
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        state = game.state()
        if pred(state):
            return state
        game.page.wait_for_timeout(250)
        if tick is not None:
            tick()
    raise E2EError(
        f"timed out waiting for {what}; state.json holds:\n{json.dumps(state, indent=1)}"
    )


def clipboard(page) -> str:
    return page.evaluate("() => navigator.clipboard.readText()")


def copy_value(game: WebGame, name: str) -> str:
    """Press a Copy button on the sheet and read what it put on the browser's clipboard."""
    marker = f"(before {name})"
    game.page.evaluate("(t) => navigator.clipboard.writeText(t)", marker)
    game.click(name)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        text = clipboard(game.page)
        if text != marker:
            return text
        game.page.wait_for_timeout(100)
    raise E2EError(f"{name} put nothing on the clipboard; the sheet shows:\n{game.snapshot()}")


def receipts_of(state: dict) -> list[dict]:
    return list((state.get("receipts") or {}).values())


# --- the scenario ----------------------------------------------------------------------------


def pay_unlock_with_overlay(game: WebGame, fam) -> None:
    page = game.page
    wait_for(game, "the Unlock button", lambda s: button(s["game"], "Unlock") is not None, 120)
    game.click("Unlock", "game")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    snap = wait_for(game, "the method step", has_button("method_browser"))
    if not snap["paused"]:
        raise E2EError("the demo should pause the game while the sheet is open")
    game.click("method_browser")
    wait_for(game, "the wallet step", has_button("forward", "Open wallet"))
    game.click("forward")
    fam.connect(page).click()  # the overlay: "Pay with Mock Wallet", then "Pay 1.0000NN USDC"
    wait_for(game, "the payment to be confirmed", shows(CONFIRMED), 120, fam.tick)
    if not game.call("owns", item=ITEM):
        raise E2EError("the sheet says paid but Scfg.owns('chapter2') is false")
    # The overlay (bridge.js: a shadow-root host on <body>) hides itself after the payment.
    page.wait_for_function(
        "() => [...document.body.children]"
        ".every((e) => !e.shadowRoot || e.style.display === 'none')",
        timeout=20_000,
    )
    game.click("continue")
    wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"])
    if button(snap["game"], "Unlock") is not None:
        raise E2EError("the Unlock button is still shown after the purchase")
    found = [r for r in receipts_of(game.state()) if r["item"] == ITEM and r["kind"] == "unlock"]
    if len(found) != 1:
        raise E2EError(f"state.json should hold one unlock receipt: {game.state()}")
    mined = fam.rpc.call("eth_getTransactionReceipt", [found[0]["tx_hash"]])
    if not mined or int(mined["status"], 16) != 1 or mined["from"].lower() != fam.payer.lower():
        raise E2EError(f"the receipt's transaction is not the wallet's payment: {mined}")
    print(f"1. browser wallet: the overlay paid, chapter2 owned (tx {found[0]['tx_hash']})")


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
    print(f"2. manual transfer: copied {amount} USDC to {recipient}, paid from Anvil, found")


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
    # The autoload writes a new session at once; Godot then copies user:// to IndexedDB.
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
    game.wait_hook()
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"], 120)
    if button(snap["game"], "Unlock") is not None or not game.call("owns", item=ITEM):
        raise E2EError("Chapter 2 is not owned after the reload")
    state = game.state()
    if len(receipts_of(state)) != 2 or sid not in (state.get("pending") or {}):
        raise E2EError(f"after the reload state.json should hold 2 receipts and {sid}: {state}")
    # Watching resumed: pay the restored session from Anvil; its receipt arrives in the background.
    tx = fam.pay_manual(amount_of(session), fam.dev)
    state = wait_state(
        game, "the restored session's receipt", lambda s: len(receipts_of(s)) == 3, 120, fam.tick
    )
    if tx not in [r["tx_hash"] for r in receipts_of(state)] or sid in (state.get("pending") or {}):
        raise E2EError(f"the restored session {sid} should be paid by {tx}: {state}")
    print(
        f"3. reload: 2 receipts and pending session {sid} kept; paid after the reload, receipts=3"
    )
    return idb


def mirror_key() -> str:
    """ScfgWebMirror's localStorage key for the demo: scfg:<application/config/name>:<sha256 hex
    of the storage id>, the storage id being [scfg] web/storage_id when set, else the name."""
    text = (DEMO / "project.godot").read_text()

    def setting(section: str, name: str):
        body = re.search(rf"^\[{section}\]$(.*?)(?=^\[|\Z)", text, re.M | re.S)
        m = body and re.search(rf'^{re.escape(name)}="([^"]*)"$', body[1], re.M)
        return m[1] if m else None

    project = setting("application", "config/name")
    if project is None:
        raise E2EError("godot/demo/project.godot has no application/config/name")
    storage_id = setting("scfg", "web/storage_id") or project
    return f"scfg:{project}:{hashlib.sha256(storage_id.encode()).hexdigest()}"


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
        raise E2EError(f"one copy, under the demo's key {key!r}: {list(copies)}")
    page.goto(url + BLANK)  # the game's page is gone: nothing writes IndexedDB back
    page.evaluate(IDB_DELETE, [path, path + ".tmp"])
    if page.evaluate(IDB_READ, path) is not None:
        raise E2EError(f"{path} is still in IndexedDB after deleting it")
    page.goto(url)
    game.wait_hook()
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


# Godot's own web teardown when the game quits while the page still starts its audio: the page
# throws this, which is not the demo going on.
AUDIO_TEARDOWN = "Failed to construct 'AudioWorkletNode'"


def refusal_problems(logs: list[str], reason: str) -> list[str]:
    """The console lines of step 0 that mean the demo did not stop at its refusal (`reason`): any
    error or warning but that refusal, any page error but AUDIO_TEARDOWN, and the rest of _ready
    running ("Scfg.configure(...) first")."""
    return [
        line
        for line in logs
        if (BAD_CONSOLE.search(line) or "Scfg.configure(...) first" in line)
        and reason not in line
        and not (line.startswith("[pageerror]") and AUDIO_TEARDOWN in line)
    ]


def a_non_string_config_fails_hard(browser, url: str) -> None:
    """window.scfgE2EConfig set to something other than a JSON string (a number): the demo says so
    and quits, never running its built-in config or the rest of its _ready, and the driver does
    not start. In a context of its own (its own storage and console)."""
    context = browser.new_context(viewport={"width": 1280, "height": 720})
    try:
        page = context.new_page()
        logs: list[str] = []
        page.on("console", lambda m: logs.append(m.text))
        page.on("pageerror", lambda e: logs.append(f"[pageerror] {e}"))
        page.add_init_script("window.scfgE2EConfig = 5;")
        page.goto(url)
        reason = "e2e config window.scfgE2EConfig: not a string but number"
        deadline = time.monotonic() + 120
        while not any(reason in line for line in logs):
            if time.monotonic() > deadline:
                raise E2EError(
                    "the demo did not refuse a non-string config:\n" + "\n".join(logs[-40:])
                )
            page.wait_for_timeout(200)
        page.wait_for_timeout(1000)  # what a _ready that went on would log next
        if refusal_problems(logs, reason):
            raise E2EError("after refusing the config the demo went on:\n" + "\n".join(logs[-40:]))
        if page.evaluate("typeof window.scfgE2E") != "undefined":
            raise E2EError("the e2e driver started without a usable config")
    finally:
        context.close()
    print("0. a non-string window.scfgE2EConfig: the demo refuses it and quits")


def amount_of(session: dict) -> str:
    """A pending session's amount as the sheet shows it (6 decimals)."""
    units = int(session["units"])
    return f"{units // 10**6}.{units % 10**6:06d}"


# --- the run ---------------------------------------------------------------------------------


def run(godot: str, family: str, templates_dir, headed: bool) -> int:
    fam_cls = FAMILIES[family]
    exe = shutil.which(fam_cls.tool)
    if not exe:
        print(fam_cls.missing)
        return 2
    from playwright.sync_api import sync_playwright

    started = time.monotonic()
    work = Path(tempfile.mkdtemp(prefix="scfg-godot-web-e2e-"))
    home = work / "home"
    env = dict(os.environ)
    env.update(
        {
            "HOME": str(home),
            "XDG_DATA_HOME": str(home / "data"),
            "XDG_CONFIG_HOME": str(home / "config"),
            "XDG_CACHE_HOME": str(home / "cache"),
        }
    )
    fam = httpd = None
    console: list[str] = []
    try:
        tag, version_dir = godot_version(godot)
        link_templates(templates_dir, version_dir, tag, home / "data")
        sync_godot()
        t0 = time.monotonic()
        export_web(godot, demo_copy(work), work / "web", env)
        (work / "web" / BLANK).write_text("<!doctype html><title>blank</title>\n")
        print(f"Godot web e2e: {family} on {godot} (exported in {time.monotonic() - t0:.1f} s)")
        url, httpd = serve(work / "web")
        fam = fam_cls(exe, work)
        with sync_playwright() as p:
            options = dict(
                headless=not headed,
                args=[
                    "--use-angle=swiftshader",
                    "--enable-unsafe-swiftshader",
                    "--autoplay-policy=no-user-gesture-required",
                ],
            )
            try:  # the installed Chrome, like the integration tests; else Playwright's
                browser = p.chromium.launch(channel="chrome", **options)
            except Exception:
                browser = p.chromium.launch(**options)
            # Chromium's Local Network Access check can block the page's fetch() to the loopback
            # Anvil (a test-environment matter: real games fetch public RPC endpoints). The
            # clipboard permissions let the runner read what the sheet's Copy buttons copied.
            a_non_string_config_fails_hard(browser, url)
            context = browser.new_context(
                permissions=["local-network-access", "clipboard-read", "clipboard-write"],
                viewport={"width": 1280, "height": 720},
            )
            page = context.new_page()
            page.on("console", lambda m: console.append(m.text))
            page.on("pageerror", lambda e: console.append(f"[pageerror] {e}"))
            page.add_init_script(f"window.scfgE2EConfig = {json.dumps(json.dumps(fam.config()))};")
            install_mock_wallet(page, fam.rpc, fam.payer)
            page.goto(url)
            game = WebGame(page)
            game.wait_hook()
            pay_unlock_with_overlay(game, fam)
            tip_by_manual_transfer(game, fam)
            idb = pending_survives_reload(game, fam)
            receipts_survive_a_deleted_state_file(game, url)
            saved = [x for x in idb["paths"] if x.endswith("/" + STATE)]
            print(f"   IndexedDB: databases {idb['dbs']}, {len(idb['paths'])} entries, {saved}")
            browser.close()
        bad = [line for line in console if BAD_CONSOLE.search(line)]
        if bad:
            raise E2EError("the page logged errors or warnings:\n" + "\n".join(bad))
    except Exception as e:
        print("\n".join(console[-80:]))
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(
            f"Godot web e2e ({family}) FAILED after {time.monotonic() - started:.1f} s "
            f"(files in {work})"
        )
        return 1
    finally:
        if httpd is not None:
            httpd.shutdown()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"Godot web e2e ({family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--godot", required=True, help="the Godot editor binary")
    ap.add_argument("--family", choices=tuple(FAMILIES), default="evm")
    ap.add_argument(
        "--templates-dir",
        help="data directory holding godot/export_templates/<version> (fetch_godot.py's)",
    )
    ap.add_argument("--headed", action="store_true", help="show the browser window")
    args = ap.parse_args(argv)
    return run(args.godot, args.family, args.templates_dir, args.headed)


if __name__ == "__main__":
    raise SystemExit(main())
