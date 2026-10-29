"""Run the plugin in Ren'Py's web build: real web build, headless Chrome, local Anvil.

Builds tests/renpy_web_e2e for the web with the SDK's launcher, serves it on localhost,
opens it in headless Chromium with a mock EIP-6963 wallet backed by Anvil, and drives the
payment sheet through Ren'Py's renpy_exec() page hook:

  1. browser wallet: the checkout overlay pays "chapter2", which becomes owned;
  2. manual transfer: a tip sent straight from Anvil is found by the frame-pumped watcher;
  3. reload: after the tab reloads, receipts and an unpaid pending session are still there.

Needs anvil (Foundry), Playwright with Chromium, and a Ren'Py SDK with web support (its
web/ directory). Usage:

    uv run python tools/run_renpy_web_e2e.py --sdk /path/to/renpy-sdk
"""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "tests" / "renpy_web_e2e"
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(ROOT / "tools"))

from run_renpy_e2e import setup_evm, wait_receipt  # noqa: E402

from scfg_core.coio import Sync  # noqa: E402
from scfg_core.rpc import RpcClient, UrllibTransport  # noqa: E402

from browser_mocks import install_mock_wallet  # noqa: E402

STATE = "result = e2e_state()"

# The time (ms) Emscripten's IDBFS stored for a file in IndexedDB, or null if absent.
IDB_MTIME = """async (path) => {
  const db = await new Promise((ok, fail) => {
    const r = indexedDB.open("/home/web_user/.renpy");
    r.onsuccess = () => ok(r.result);
    r.onerror = () => fail(r.error);
  });
  try {
    const entry = await new Promise((ok, fail) => {
      const r = db.transaction("FILE_DATA").objectStore("FILE_DATA").get(path);
      r.onsuccess = () => ok(r.result);
      r.onerror = () => fail(r.error);
    });
    if (!entry) return null;
    const t = entry.timestamp;
    return t instanceof Date ? t.getTime() : Number(t);
  } finally {
    db.close();
  }
}"""


class _Files(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def log_message(self, *args):
        pass


def serve(directory: Path):
    handler = functools.partial(_Files, directory=str(directory))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{httpd.server_address[1]}/", httpd


def py(page, code):
    """Run Python in the game; returns its `result` variable."""
    return page.evaluate("(code) => window.renpy_exec(code)", code)


def wait_for(page, test, what, timeout=120):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = py(page, STATE)
            if test(last):
                return last
        except Exception as e:  # still loading: renpy_exec missing or the store not ready
            last = repr(e)[:200]
        page.wait_for_timeout(500)   # lets the wallet's exposed-function calls run
    raise RuntimeError(f"timed out waiting for {what}; last state: {last!r}")


def wait_saved(page, timeout=8):
    """Wait until the game itself has saved a pending session to its persistent file and
    that file has reached IndexedDB. Pending sessions are saved at once, so this is well
    within the plugin's 10 s throttle for other pending changes."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        disk = py(page, "result = e2e_disk()")
        if disk["pending"] >= 1:
            stored = page.evaluate(IDB_MTIME, disk["path"])
            last = (disk, stored)
            if stored is not None and stored >= disk["mtime"]:
                return
        else:
            last = disk
        page.wait_for_timeout(250)
    raise RuntimeError(f"the pending session never reached IndexedDB; last: {last!r}")


def wait_loaded(page, timeout=300):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if py(page, "result = e2e_ready"):
                return
        except Exception:
            pass
        page.wait_for_timeout(1000)
    raise RuntimeError("the web build never reached label start")


def build(sdk: Path, dest: Path) -> None:
    proc = subprocess.run([str(sdk / "renpy.sh"), str(sdk / "launcher"), "web_build", str(GAME),
                           "--destination", str(dest)], capture_output=True, text=True,
                          timeout=900)
    if proc.returncode != 0 or not (dest / "index.html").exists():
        print(proc.stdout[-4000:])
        print(proc.stderr[-4000:])
        raise RuntimeError("web build failed")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True, type=Path, help="Ren'Py SDK directory")
    ap.add_argument("--headed", action="store_true", help="show the browser window")
    args = ap.parse_args()
    anvil = shutil.which("anvil")
    if not anvil:
        print("anvil not found on PATH (install Foundry: https://getfoundry.sh)")
        return 2
    if not (args.sdk / "web").is_dir():
        print("this Ren'Py SDK has no web support (no web/ directory)")
        return 2
    from playwright.sync_api import sync_playwright

    for name in ("scfg", "python-packages"):
        target = GAME / "game" / name
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ROOT / "plugin" / "game" / name, target,
                        ignore=shutil.ignore_patterns("__pycache__", "*.rpyc"))
    work = Path(tempfile.mkdtemp(prefix="scfg-web-e2e-"))
    env, cleanup = setup_evm(anvil, work)
    httpd = None
    console: list = []   # the page's console: the game's Python output and errors
    try:
        (GAME / "game" / "e2e_web_config.json").write_text(json.dumps({
            "rpc": env["SCFG_E2E_RPC"], "usdc": env["SCFG_E2E_USDC"],
            "dev": env["SCFG_E2E_DEV"]}))
        print("building the web game (this takes a minute)…")
        build(args.sdk, work / "web")
        url, httpd = serve(work / "web")
        rpc = Sync(RpcClient([env["SCFG_E2E_RPC"]]), UrllibTransport())
        payer, usdc, dev = (env["SCFG_E2E_PAYER"], env["SCFG_E2E_USDC"],
                            env["SCFG_E2E_DEV"])

        with sync_playwright() as p:
            options = dict(headless=not args.headed, args=[
                "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
                "--autoplay-policy=no-user-gesture-required"])
            try:   # the installed Chrome, like the integration tests; else Playwright's
                browser = p.chromium.launch(channel="chrome", **options)
            except Exception:
                browser = p.chromium.launch(**options)
            # Granted as in the integration tests: Chromium's Local Network Access check
            # can block the page's fetch() to the loopback Anvil (a test-environment
            # matter: real games fetch public RPC endpoints).
            context = browser.new_context(permissions=["local-network-access"])
            page = context.new_page()
            page.on("console", lambda m: console.append(f"[{m.type}] {m.text}"))
            page.on("pageerror", lambda e: console.append(f"[pageerror] {e}"))
            install_mock_wallet(page, rpc, payer)
            page.goto(url)
            page.mouse.click(10, 10)                 # some builds wait for a first click
            wait_loaded(page)
            s = wait_for(page, lambda s: s["worker"] == "PumpWorker", "the PumpWorker")
            assert s["web"] and s["supported"], s

            # 1. Browser wallet through the overlay.
            py(page, "scfg._open_checkout('unlock', 'chapter2', None, None, None, False)")
            s = wait_for(page, lambda s: s["step"] == "method", "the method step")
            assert "browser" in s["methods"], s
            cid = s["cid"]
            py(page, f"scfg._choose_method({cid!r}, 'browser')")
            py(page, f"scfg._open_browser({cid!r})")
            page.get_by_role("button", name="Pay with Mock Wallet").click()
            page.get_by_role("button", name=re.compile(r"^Pay \d")).click()
            wait_for(page, lambda s: s["hint"], "the overlay's tx hint")
            rpc.call("anvil_mine", ["0x3"])
            s = wait_for(page, lambda s: s["owned"], "chapter2 to be owned")
            print("1. browser wallet: paid, owned")
            py(page, f"scfg._close_checkout({cid!r}, 'finish')")

            # 2. Manual transfer found by the watcher.
            py(page, "scfg._open_checkout('tip', None, '2', 'USDC', None, False)")
            s = wait_for(page, lambda s: s["step"] == "amount", "the tip's amount step")
            cid = s["cid"]
            py(page, f"scfg._next_step({cid!r})")
            s = wait_for(page, lambda s: s["step"] == "method" and s["units"], "the tip session")
            py(page, f"scfg._choose_method({cid!r}, 'manual')")
            data = ("0xa9059cbb" + "0" * 24 + dev[2:].lower() + format(s["units"], "064x"))
            wait_receipt(rpc, rpc.call("eth_sendTransaction",
                                       [{"from": payer, "to": usdc, "data": data}]))
            rpc.call("anvil_mine", ["0x3"])
            s = wait_for(page, lambda s: s["receipts"] == 2, "the tip's receipt")
            # The second receipt is the tip: the session's amount (2 USDC plus its
            # identifying micro-units), all of it counted as tipped.
            assert Decimal(s["tip_total"]) == Decimal(s["units"]).scaleb(-6), s
            print(f"2. manual transfer: found, receipts=2, tip_total={s['tip_total']}")
            py(page, f"scfg._close_checkout({cid!r}, 'finish')")

            # 3. Reload: receipts and a pending session survive.
            py(page, "scfg._open_checkout('tip', None, '3', 'USDC', None, False)")
            s = wait_for(page, lambda s: s["step"] == "amount", "a third checkout")
            py(page, f"scfg._next_step({s['cid']!r})")
            wait_for(page, lambda s: s["pending"] >= 1, "the pending tip in persistent")
            wait_saved(page)                 # the plugin saves it itself; none forced here
            page.reload()
            wait_loaded(page)
            s = wait_for(page, lambda s: (s["owned"] and s["receipts"] == 2 and s["pending"] >= 1
                                          and s["worker"] == "PumpWorker"
                                          and s["pending_units"]),
                         "state after reload")
            # Watching resumed: pay the restored session from Anvil and wait for its receipt.
            data = ("0xa9059cbb" + "0" * 24 + dev[2:].lower()
                    + format(s["pending_units"], "064x"))
            wait_receipt(rpc, rpc.call("eth_sendTransaction",
                                       [{"from": payer, "to": usdc, "data": data}]))
            rpc.call("anvil_mine", ["0x3"])
            s = wait_for(page, lambda s: s["receipts"] == 3, "the restored session's receipt")
            print("3. reload: receipts and the pending session kept, watching resumed "
                  "(paid after reload, receipts=3)")
            browser.close()
        print("Ren'Py web e2e PASSED")
        return 0
    except Exception as e:
        print("\n".join(console[-60:]))
        print(f"Ren'Py web e2e FAILED: {e}")
        return 1
    finally:
        if httpd is not None:
            httpd.shutdown()
        cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
