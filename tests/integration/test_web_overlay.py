"""The web build's payment path in a real (headless) Chrome, without Ren'Py: the real
bridge.js and checkout page in a page, driven by the real WebPage, PumpWorker,
OverlayCheckout and CheckoutController in this process, whose JSON-RPC goes through the
page's fetch() to a local Anvil chain or Solana validator (so CORS is exercised too).

Run with anvil and solana-test-validator on PATH:  uv run pytest -m integration
"""
import re
import time
from pathlib import Path

import pytest

from cryptopay_core.checkout import CheckoutController
from cryptopay_core.engine import Engine
from cryptopay_core.overlay import OverlayCheckout
from cryptopay_core.payments import prepare_payment
from cryptopay_core.pump_worker import PumpWorker
from cryptopay_core.web_page import WebPage

from browser_mocks import install_mock_solana_wallet, install_mock_wallet
from conftest import mine

pytestmark = pytest.mark.integration
playwright_api = pytest.importorskip("playwright.sync_api")

WEB = Path(__file__).resolve().parents[2] / "plugin" / "game" / "cryptopay" / "web"
GAME_URL = "http://localhost:8123/"   # answered by page.route: no server, a secure context


class PageJs:
    """Ren'Py's emscripten module, played by a Playwright page."""

    def __init__(self, page):
        self.page = page

    def run_script(self, source):
        self.page.evaluate("(s) => { (0, eval)(s); }", source)

    def run_script_int(self, source):
        return int(self.page.evaluate("(s) => Number((0, eval)(s)) | 0", source))

    def run_script_string(self, source):
        return str(self.page.evaluate("(s) => String((0, eval)(s))", source))


@pytest.fixture
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


def game_page(browser):
    # The page is a route-fulfilled http://localhost document, which Chrome counts as a
    # non-local address space; without this grant its fetch() to the loopback Anvil or
    # validator is blocked ("Permission was denied for this request to access the
    # `loopback` address space"). The real game fetches public RPC endpoints instead.
    page = browser.new_context(permissions=["local-network-access"]).new_page()
    page.route(GAME_URL, lambda route: route.fulfill(
        status=200, content_type="text/html",
        body="<!doctype html><title>game</title><body><canvas></canvas></body>"))
    return page


def web_game(page, config):
    """What cryptopay_api.rpy sets up in the web build, minus Ren'Py."""
    page.goto(GAME_URL)
    web = WebPage(PageJs(page), lambda name: (WEB / name).read_bytes())
    web.install()
    worker = PumpWorker(Engine(config), web, interval=lambda: 0.5)
    spec = prepare_payment(config, "unlock", item="chapter2")
    ctl = CheckoutController(
        config=config, spec=spec, submit=worker.submit,
        server_factory=lambda **kw: OverlayCheckout(ui=web, key="o1", **kw),
        browser_wallet=web.has_wallet)
    return web, worker, ctl


def frames(page, worker, ctl, until, timeout=60):
    """The game loop: pump the worker, poll the controller, route outcomes."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        worker.pump()
        ctl.poll()
        for o in worker.drain_outcomes():
            ctl.on_outcome(o)
        if until():
            return
        page.wait_for_timeout(50)   # lets the page's fetch() and wallet callbacks run
    raise AssertionError(f"timed out: step={ctl.step} state={ctl.state} error={ctl.error}")


def overlays(page):
    return page.evaluate("document.querySelectorAll('body > div').length")


def test_overlay_pays_with_the_page_wallet_on_anvil(world, browser):
    page = game_page(browser)
    install_mock_wallet(page, world["rpc"], world["payer"])
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method")   # session started over fetch()
    assert "browser" in ctl.methods()
    ctl.choose_method("browser")
    assert ctl.open_browser() == OverlayCheckout.url
    assert ctl.step == "waiting" and overlays(page) == 1
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.wait_for()
    assert ctl.pay_amount_text in pay_button.inner_text()
    pay_button.click()
    frames(page, worker, ctl, lambda: ctl.hint_tx is not None)
    mine(world["rpc"], 3)
    frames(page, worker, ctl, lambda: ctl.state == "paid")
    assert worker.snapshot.owned == frozenset({"chapter2"})
    ctl.close()
    assert overlays(page) == 0


def test_overlay_reopens_without_a_second_instance(world, browser):
    page = game_page(browser)
    install_mock_wallet(page, world["rpc"], world["payer"])
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method")
    ctl.choose_method("browser")
    ctl.open_browser()
    page.get_by_role("button", name="Back to the game").click()
    assert page.evaluate("document.querySelector('body > div').style.display") == "none"
    ctl.open_browser()                                   # "Reopen wallet window"
    assert overlays(page) == 1
    assert page.evaluate("document.querySelector('body > div').style.display") == ""
    ctl.close()


def test_no_wallet_in_the_page_means_no_browser_method(world, browser):
    page = game_page(browser)
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step != "network")
    assert "browser" not in ctl.methods()
    ctl.close()


def test_overlay_builds_and_pays_on_solana(solana_world, browser):
    w = solana_world
    page = game_page(browser)
    sent = install_mock_solana_wallet(page, w["cluster"], w["player"])
    web, worker, ctl = web_game(page, w["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method", timeout=90)
    assert "browser" in ctl.methods()
    ctl.choose_method("browser")
    ctl.open_browser()
    page.get_by_role("button", name="Pay with Mock Solana Wallet").click()
    pay_button = page.locator("#pay")
    pay_button.wait_for()
    pay_button.click()                       # the build request goes through the overlay queue
    frames(page, worker, ctl, lambda: ctl.state == "paid", timeout=120)
    assert len(sent) == 1 and ctl.hint_tx == sent[0]
    ctl.close()


def test_copy_falls_back_when_the_clipboard_api_is_missing(browser):
    page = game_page(browser)
    page.goto(GAME_URL)
    web = WebPage(PageJs(page), lambda name: (WEB / name).read_bytes())
    web.install()
    page.evaluate("Object.defineProperty(navigator, 'clipboard', { value: undefined })")
    page.evaluate("window.__copied = null; document.execCommand = (c) => "
                  "{ window.__copied = document.activeElement.value; return true; }")
    web.copy("0xfeed")
    assert page.evaluate("window.__copied") == "0xfeed"
