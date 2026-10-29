"""The localhost checkout page in a real (headless) Chrome with a mock EIP-6963 wallet.

Covers the whole browser path: bundled page -> game's CheckoutServer -> tx hint ->
watcher verification on a local Anvil chain -> status back on the page.

Run with:  uv run pytest -m integration
Skipped when anvil, Playwright or a Chromium browser is unavailable.
"""
import inspect
import re
import time
from concurrent.futures import Future
from pathlib import Path

import pytest

from scfg_core.checkout import CheckoutController
from scfg_core.local_server import CheckoutServer
from scfg_core.payments import prepare_payment

from browser_mocks import install_mock_wallet
from conftest import mine, tick_until

pytestmark = pytest.mark.integration
playwright_api = pytest.importorskip("playwright.sync_api")

WEB = Path(__file__).resolve().parents[2] / "plugin" / "game" / "scfg" / "web"


def immediate(engine):
    def submit(fn):
        fut = Future()
        try:
            result = fn(engine)
            if inspect.isgenerator(result):
                result = engine.run(result)
            fut.set_result(result)
        except Exception as e:
            fut.set_exception(e)
        return fut
    return submit


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


def open_checkout(world):
    engine, cfg = world["engine"], world["config"]
    spec = prepare_payment(cfg, "unlock", item="chapter2")
    ctl = CheckoutController(
        config=cfg, spec=spec, submit=immediate(engine), mobile=False,
        server_factory=lambda **kw: CheckoutServer(load_asset=lambda n: (WEB / n).read_bytes(), **kw))
    ctl.select_chain("arbitrum")
    ctl.poll()
    assert ctl.state == "waiting"
    return ctl, ctl.start_browser()


def new_page(browser, world, account, reject=False):
    page = browser.new_page()
    install_mock_wallet(page, world["rpc"], account, reject)
    return page


def status_text(page):
    return page.locator("#status").inner_text()


def test_browser_wallet_pays_and_page_shows_confirmation(world, browser):
    ctl, url = open_checkout(world)
    page = new_page(browser, world, world["payer"])
    page.goto(url)
    assert page.locator("#title").inner_text() == "Chapter 2"
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.wait_for()
    assert ctl.pay_amount_text in pay_button.inner_text()
    pay_button.click()

    # The page posts the tx hash to the game's local server.
    deadline = time.time() + 10
    while world["engine"].pending[ctl.session_id]["hint_tx"] is None:
        assert time.time() < deadline, "page never reported the transaction"
        time.sleep(0.05)
    assert ctl.hint_tx is not None

    mine(world["rpc"], 3)
    tick_until(world["engine"], "paid", on_outcome=ctl.on_outcome)
    assert ctl.state == "paid"
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('Payment confirmed')",
        timeout=10_000)
    assert world["engine"].snapshot().owned == frozenset({"chapter2"})
    ctl.close()


def test_page_warns_when_wallet_has_no_tokens(world, browser):
    ctl, url = open_checkout(world)
    page = new_page(browser, world, world["dev"])      # the dev account holds no mock USDC
    page.goto(url)
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    page.wait_for_function("document.getElementById('status').textContent.includes('You need')")
    assert page.get_by_role("button", name=re.compile(r"^Pay \d")).is_disabled()
    assert "0.000000 USDC" in status_text(page)
    ctl.close()


def test_page_handles_wallet_rejection(world, browser):
    ctl, url = open_checkout(world)
    page = new_page(browser, world, world["payer"], reject=True)
    page.goto(url)
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.click()
    page.wait_for_function("document.getElementById('status').textContent.includes('rejected')")
    assert pay_button.is_enabled()                      # player can try again
    assert world["engine"].pending[ctl.session_id]["hint_tx"] is None
    ctl.close()
