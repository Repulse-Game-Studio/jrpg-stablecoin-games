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

from scfg_core.checkout import CheckoutController
from scfg_core.engine import Engine
from scfg_core.overlay import OverlayCheckout
from scfg_core.payments import prepare_payment
from scfg_core.pump_worker import PumpWorker
from scfg_core.web_page import WebPage

from browser_mocks import (
    NAMELESS_WALLETS,
    THROWING_ETHEREUM,
    assert_nothing_escaped,
    install_malformed_extensions,
    install_mock_solana_wallet,
    install_mock_wallet,
    record_escapes,
    try_bad_wallets,
)
from conftest import mine

pytestmark = pytest.mark.integration
playwright_api = pytest.importorskip("playwright.sync_api")

WEB = Path(__file__).resolve().parents[2] / "plugin" / "game" / "scfg" / "web"
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
    """What scfg_api.rpy sets up in the web build, minus Ren'Py."""
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
    assert ctl.price_text in pay_button.inner_text()
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


def overlay_hosts(page):
    """The overlay hosts bridge.js tagged: [key, ...]."""
    return page.evaluate("[...document.querySelectorAll('[data-scfg-overlay]')]"
                         ".filter((e) => e.shadowRoot).map((e) => e.getAttribute('data-scfg-overlay'))")


def test_overlay_host_is_tagged_with_its_key(world, browser):
    page = game_page(browser)
    install_mock_wallet(page, world["rpc"], world["payer"])
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method")
    ctl.choose_method("browser")
    ctl.open_browser()
    assert overlay_hosts(page) == ["o1"]
    assert page.evaluate("document.querySelector('[data-scfg-overlay=\"o1\"]')"
                         " === document.querySelector('body > div')")
    ctl.close()
    assert overlay_hosts(page) == []


def test_overlay_survives_malformed_extensions_and_pays_on_anvil(world, browser):
    """Broken EIP-6963 announcements and Wallet Standard registrations in the game's page: the
    game still sees the good wallet, the overlay shows each bad one as its wallet error, nothing
    escapes into the game's page, and the good wallet pays."""
    page = game_page(browser)
    install_mock_wallet(page, world["rpc"], world["payer"])
    errors = install_malformed_extensions(page)
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method")
    assert "browser" in ctl.methods()
    ctl.choose_method("browser")
    ctl.open_browser()
    page.get_by_role("button", name="Pay with Mock Wallet").wait_for()
    page.wait_for_timeout(700)                         # past the page's "no wallet" check
    assert_nothing_escaped(page, errors)
    try_bad_wallets(page, errors)
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.wait_for()
    pay_button.click()
    frames(page, worker, ctl, lambda: ctl.hint_tx is not None)
    mine(world["rpc"], 3)
    frames(page, worker, ctl, lambda: ctl.state == "paid")
    assert_nothing_escaped(page, errors)
    ctl.close()


def test_overlay_survives_malformed_extensions_and_pays_on_solana(solana_world, browser):
    """The same on Solana, the broken wallets registering before the good one (the game's
    wallet check must look past them)."""
    w = solana_world
    page = game_page(browser)
    errors = install_malformed_extensions(page)
    sent = install_mock_solana_wallet(page, w["cluster"], w["player"])
    web, worker, ctl = web_game(page, w["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method", timeout=90)
    assert "browser" in ctl.methods()
    ctl.choose_method("browser")
    ctl.open_browser()
    page.get_by_role("button", name="Pay with Mock Solana Wallet").wait_for()
    page.wait_for_timeout(700)
    assert_nothing_escaped(page, errors)
    try_bad_wallets(page, errors, family="solana")
    page.get_by_role("button", name="Pay with Mock Solana Wallet").click()
    pay_button = page.locator("#pay")
    pay_button.wait_for()
    pay_button.click()
    frames(page, worker, ctl, lambda: ctl.state == "paid", timeout=120)
    assert len(sent) == 1 and ctl.hint_tx == sent[0]
    assert_nothing_escaped(page, errors)
    ctl.close()


def test_wallets_the_page_cannot_list_are_not_offered(world, browser):
    """An announcement whose name is not a string: the overlay would list no wallet for it,
    so the game offers no "Browser wallet"."""
    page = game_page(browser)
    errors = record_escapes(page)
    page.add_init_script(NAMELESS_WALLETS)
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step != "network")
    assert "browser" not in ctl.methods()
    assert_nothing_escaped(page, errors)
    ctl.close()


def test_solana_wallets_the_page_cannot_list_are_not_offered(solana_world, browser):
    page = game_page(browser)
    errors = record_escapes(page)
    page.add_init_script(NAMELESS_WALLETS)
    web, worker, ctl = web_game(page, solana_world["config"])
    frames(page, worker, ctl, lambda: ctl.step != "network", timeout=90)
    assert "browser" not in ctl.methods()
    assert_nothing_escaped(page, errors)
    ctl.close()


def test_a_throwing_window_ethereum_is_no_wallet(world, browser):
    """window.ethereum whose getter throws: no wallet, answered quietly (the bridge logs
    nothing, which it would for every failed call)."""
    page = game_page(browser)
    errors = record_escapes(page)
    page.add_init_script(THROWING_ETHEREUM)
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step != "network")
    assert "browser" not in ctl.methods()
    assert_nothing_escaped(page, errors)
    ctl.close()


def test_a_hash_that_cannot_be_sent_to_the_game_is_found_by_reference(world, browser):
    """The wallet sends the payment but answers its hash as a value with no JSON (a BigInt):
    the overlay's request to the game fails as a request (the page says the game will find
    the payment), nothing breaks the overlay's message queue, and the game finds it."""
    page = game_page(browser)
    install_mock_wallet(page, world["rpc"], world["payer"], odd_hash=True)
    errors = record_escapes(page)
    web, worker, ctl = web_game(page, world["config"])
    frames(page, worker, ctl, lambda: ctl.step == "method")
    ctl.choose_method("browser")
    ctl.open_browser()
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.wait_for()
    pay_button.click()
    page.wait_for_function(
        "document.querySelector('[data-scfg-overlay]').shadowRoot.getElementById('status')"
        ".textContent.includes('keep this page open')", timeout=10_000)
    mine(world["rpc"], 3)
    frames(page, worker, ctl, lambda: ctl.state == "paid")
    assert ctl.hint_tx is None
    assert worker.snapshot.owned == frozenset({"chapter2"})
    assert_nothing_escaped(page, errors)
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
