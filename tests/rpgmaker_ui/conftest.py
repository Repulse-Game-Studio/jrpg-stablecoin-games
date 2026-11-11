"""The RPG Maker MZ payment sheet and restore screen in a real (headless) Chrome, on the MZ shim.

The plugin is built once (`node build.mjs` in rpgmaker/, the same as `npm run build`), the
rpgmaker/ folder is served on localhost, and each test opens test/harness/ui.html: the shim, the
glue (built to dist/test/ui_driver.js from the same sources as SCFG.js, with fake chains behind
the Engine and a Worker the test steps through `scfgTest.pass()`) and MZ's boot.

Needs Node 22.18+ and rpgmaker/node_modules (`cd rpgmaker && npm ci`): skipped without them, a
failure with SCFG_REQUIRE_JS=1 (CI). Any console error or uncaught exception in the page fails
the test, and so does an error that stopped the game: the shim models MZ's
SceneManager.onError/onReject (an uncaught error or an unhandled rejection stops an MZ game), and
`mzShim.errors` must be empty when the test ends.

MZ reads the keyboard once per frame (Input.update), so a key pressed and released within one
frame is never seen: `hold()` keeps a key down for a few frames. Gamepads come from a stubbed
`navigator.getGamepads()` (`pad()`), as MZ polls them.
"""
import functools
import http.server
import os
import re
import shutil
import subprocess
import threading
from pathlib import Path

import pytest

playwright_api = pytest.importorskip("playwright.sync_api")

ROOT = Path(__file__).resolve().parents[2]
RPGMAKER = ROOT / "rpgmaker"
MIN_NODE = (22, 18)

# What the page starts with: a gamepad MZ can poll, a clipboard the test controls, and a record
# of what the sheet copied.
INIT_SCRIPT = """
(() => {
  const pad = { id: "test pad", index: 0, connected: true, mapping: "standard", timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  window.__pad = pad;
  window.__padConnected = false;
  Object.defineProperty(navigator, "getGamepads", { configurable: true, value: () => [window.__padConnected ? pad : null] });
  window.__clip = { text: "", writes: [] };
  const clip = {
    writeText: async (t) => { window.__clip.writes.push(String(t)); window.__clip.text = String(t); },
    readText: async () => window.__clip.text,
  };
  Object.defineProperty(navigator, "clipboard", { configurable: true, get: () => clip });
  window.open = () => null;
})();
"""


def _require_node():
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_JS") == "1" else pytest.skip
    node = shutil.which("node")
    if node is None:
        give_up("node not installed (the sheet tests build the plugin)")
    out = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=30).stdout
    m = re.match(r"v(\d+)\.(\d+)", out.strip())
    if m is None or (int(m.group(1)), int(m.group(2))) < MIN_NODE:
        give_up("node {} is too old (needs {}.{})".format(out.strip(), *MIN_NODE))
    if not (RPGMAKER / "node_modules").is_dir():
        give_up("rpgmaker/node_modules missing (cd rpgmaker && npm ci)")
    return node


@pytest.fixture(scope="session")
def built():
    node = _require_node()
    run = subprocess.run([node, "build.mjs"], cwd=RPGMAKER, capture_output=True, text=True, timeout=300)
    assert run.returncode == 0, f"the build failed:\n{run.stdout}\n{run.stderr}"
    return RPGMAKER / "dist"


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


@pytest.fixture(scope="session")
def server(built):
    handler = functools.partial(_Quiet, directory=str(RPGMAKER))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()
    httpd.server_close()


@pytest.fixture(scope="session")
def browser():
    with playwright_api.sync_playwright() as p:
        try:
            b = p.chromium.launch(channel="chrome", headless=True)
        except Exception:
            try:
                b = p.chromium.launch(headless=True)
            except Exception as e:
                pytest.skip(f"no Chromium browser available: {e}")
        yield b
        b.close()


class Game:
    """One harness page: helpers to drive the sheet as a player would."""

    def __init__(self, page):
        self.page = page

    def js(self, expr, arg=None):
        return self.page.evaluate(expr, arg)

    def frames(self, n=2):
        self.page.evaluate("(n) => scfgTest.frames(n)", n)

    def pass_(self):
        self.page.evaluate("() => scfgTest.pass()")

    def view(self):
        return self.page.evaluate("() => scfgTest.view()")

    def restore_view(self):
        return self.page.evaluate("() => scfgTest.restoreView()")

    def setup(self, **opts):
        self.page.evaluate("(o) => scfgTest.setup(o)", opts)

    def pay(self, kind="unlock", **opts):
        if kind == "unlock" and not opts:
            opts = {"item": "chapter2"}
        self.page.evaluate("([k, o]) => scfgTest.pay(k, o)", [kind, opts])
        self.frames(2)

    def box(self):
        return self.page.evaluate("() => scfgTest.box")

    def screen(self, kind="payment"):
        return self.page.locator(f"#scfg-host .screen[data-screen={kind}]")

    def key(self, name):
        """The control registered under `name` on the open screen."""
        return self.page.locator(f"#scfg-host .screen [data-key='{name}']")

    def step(self):
        return self.page.locator("#scfg-host .screen[data-screen=payment]").get_attribute("data-step")

    def focused_key(self):
        return self.page.evaluate("""() => {
            const host = document.getElementById("scfg-host");
            const el = host && host.shadowRoot.activeElement;
            return el ? el.getAttribute("data-key") : null;
        }""")

    def hold(self, key, frames=3):
        """A key held long enough for MZ's Input to see it (Playwright's press() releases at once)."""
        self.page.keyboard.down(key)
        self.frames(frames)
        self.page.keyboard.up(key)
        self.frames(2)

    def pad(self, button=None, axes=None, frames=3):
        """Press a gamepad button (standard mapping index) or push the stick, for a few frames."""
        self.page.evaluate("""([b, a]) => {
            window.__padConnected = true;
            if (b !== null) { window.__pad.buttons[b].pressed = true; window.__pad.buttons[b].value = 1; }
            if (a !== null) window.__pad.axes = a;
        }""", [button, axes])
        self.frames(frames)
        self.page.evaluate("""() => {
            for (const b of window.__pad.buttons) { b.pressed = false; b.value = 0; }
            window.__pad.axes = [0, 0, 0, 0];
        }""")
        self.frames(2)

    def wait_step(self, step):
        self.page.wait_for_function(
            "(s) => { const h = document.getElementById('scfg-host');"
            " const e = h && h.shadowRoot.querySelector('.screen[data-screen=payment]');"
            " return !!e && e.getAttribute('data-step') === s; }", arg=step, timeout=5000)


@pytest.fixture
def make_game(browser, server):
    """make_game(test_play=True, before_goto=None) -> Game: a fresh harness page, booted to the
    map. `before_goto(page)` runs before the page loads (a mock wallet's init script)."""
    pages = []

    def make(test_play=True, before_goto=None):
        context = browser.new_context(viewport={"width": 1280, "height": 720})
        context.add_init_script(INIT_SCRIPT)
        page = context.new_page()
        if before_goto is not None:
            before_goto(page)
        page.set_default_timeout(5000)
        errors = []
        page.on("console", lambda m: errors.append(f"console.{m.type}: {m.text}") if m.type == "error" else None)
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        pages.append((context, page, errors))
        page.goto(f"{server}/test/harness/ui.html" + ("?test" if test_play else ""))
        page.wait_for_function("() => window.scfgTest && scfgTest.booted()", timeout=10000)
        game = Game(page)
        game.errors = errors  # a test that expects an error takes its lines out
        return game

    yield make
    problems = []
    for context, page, errors in pages:
        problems.extend(errors)
        try:
            stopped = page.evaluate("() => window.mzShim ? mzShim.errors.map((e) => String(e && e.stack || e)) : []")
        except Exception as e:  # a page that is gone has nothing left to tell
            stopped = [] if page.is_closed() else [f"cannot read mzShim.errors: {e}"]
        problems.extend(f"the game stopped on an error (SceneManager.onError): {e}" for e in stopped)
        context.close()
    assert not problems, "the page reported errors:\n" + "\n".join(problems)


@pytest.hookimpl(tryfirst=True)   # before -m deselects
def pytest_collection_modifyitems(items):
    """Mark every test here `rpgmaker_ui`: the default run deselects them (they have their own
    CI job, with SCFG_REQUIRE_JS=1); run them with `pytest -m rpgmaker_ui`."""
    here = os.path.dirname(os.path.abspath(__file__))
    for item in items:
        if str(item.path).startswith(here + os.sep):
            item.add_marker(pytest.mark.rpgmaker_ui)


def _explicit_markexpr(config):
    """Whether `-m` was given on the command line (not only by pyproject's addopts)."""
    return any(a == "-m" or a.startswith("-m") for a in config.invocation_params.args)


def pytest_configure(config):
    """The default `-m` (pyproject addopts) deselects `rpgmaker_ui` tests, which have their own CI
    job. Naming this directory (or a file in it) on the command line, or SCFG_REQUIRE_JS=1,
    runs them; an explicit `-m` from the command line is left alone."""
    term = " and not rpgmaker_ui"
    if term not in (config.option.markexpr or "") or _explicit_markexpr(config):
        return
    here = Path(__file__).resolve().parent
    named = False
    for arg in config.args:
        path = Path(config.invocation_params.dir, arg.split("::")[0]).resolve()
        if path == here or here in path.parents:
            named = True
    if named or os.environ.get("SCFG_REQUIRE_JS") == "1":
        config.option.markexpr = config.option.markexpr.replace(term, "")


def pytest_terminal_summary(terminalreporter, config):
    """A run that ran nothing because the default `-m` deselected it says how to select the
    rpgmaker_ui tests; not after --collect-only or a `-m` of the caller's own."""
    if config.option.collectonly or _explicit_markexpr(config):
        return
    if config.option.markexpr and "not rpgmaker_ui" in config.option.markexpr:
        stats = terminalreporter.stats
        if stats.get("deselected") and not any(
            stats.get(k) for k in ("passed", "failed", "skipped", "error")
        ):
            terminalreporter.write_line(
                "hint: the rpgmaker_ui tests are deselected by default; run them with "
                "`pytest -m rpgmaker_ui`", yellow=True)
