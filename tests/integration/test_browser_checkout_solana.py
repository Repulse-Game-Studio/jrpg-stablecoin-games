"""The localhost checkout page in a real (headless) Chrome with a mock Wallet Standard wallet,
paying USDC on a local Solana validator.

Covers the whole browser path: bundled page -> wallet discovery -> the game's POST build
-> the (mock) wallet signs and sends -> signature posted back -> watcher verifies on chain
-> status back on the page.

Run with anvil and solana-test-validator on PATH:  uv run pytest -m integration
Skipped when solana-test-validator, Playwright or a Chromium browser is unavailable.
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

from browser_mocks import install_mock_solana_wallet

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
        server_factory=lambda **kw: CheckoutServer(
            load_asset=lambda n: (WEB / n).read_bytes(), **kw))
    ctl.select_chain("solana")
    ctl.poll()
    assert ctl.state == "waiting"
    return ctl, ctl.start_browser()


def new_page(browser, world, reject=False, drop_first=False):
    """A page with the mock wallet; returns (page, signatures the wallet sent on chain)."""
    page = browser.new_page()
    sent = install_mock_solana_wallet(page, world["cluster"], world["player"], reject,
                                      drop_first)
    return page, sent


def status_text(page):
    return page.locator("#status").inner_text()


def pump_until_page_says(page, text, engine, ctl, timeout=90):
    """Tick the engine (routing outcomes to the controller) while the page runs.

    Playwright's sync API only delivers the wallet's exposed-function calls while the test
    thread is inside a Playwright call, so waiting is done with page.wait_for_timeout.
    """
    seen = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        for outcome in engine.tick():
            seen.append(outcome.status)
            ctl.on_outcome(outcome)
        if text in status_text(page):
            return
        page.wait_for_timeout(300)
    raise AssertionError(f"page never said {text!r}; engine saw {seen[-20:]}; "
                         f"status is {status_text(page)!r}")


def connect_wallet(page):
    page.get_by_role("button", name="Pay with Mock Solana Wallet").click()
    pay_button = page.locator("#pay")
    pay_button.wait_for()
    return pay_button


def test_pays_through_the_page(solana_world, browser):
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w)
    page.goto(url)
    assert page.locator("#title").inner_text() == "Chapter 2"
    pay_button = connect_wallet(page)
    assert re.match(r"^Pay \d", pay_button.inner_text())
    assert ctl.pay_amount_text in pay_button.inner_text()
    pay_button.click()

    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert ctl.state == "paid"
    assert w["engine"].snapshot().owned == frozenset({"chapter2"})

    # solana.js's base58 round-trips the wallet's signature bytes exactly.
    assert len(sent) == 1
    assert ctl.hint_tx == sent[0]
    assert ctl.result.tx_hash == sent[0]
    ctl.close()


def test_dropped_transaction_lets_the_player_pay_again(solana_world, browser, monkeypatch):
    monkeypatch.setattr("scfg_core.backends.solana.SOLANA_HINT_TIMEOUT_S", 3)
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w, drop_first=True)
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function("document.getElementById('pay').classList.contains('hidden')",
                           timeout=10_000)
    assert sent == [] and ctl.hint_tx is not None       # the page reported the signature

    pump_until_page_says(page, "didn't reach the network", w["engine"], ctl, timeout=30)
    assert "Click Pay to try again" in status_text(page)
    assert page.locator("#status").get_attribute("class") == "warn"
    assert pay_button.is_visible() and pay_button.is_enabled()
    assert ctl.hint_tx is None and ctl.error == "tx_not_landed"

    pay_button.click()                                  # same connected account
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert len(sent) == 1 and ctl.result.tx_hash == sent[0]
    ctl.close()


def test_sent_payment_is_found_even_if_the_game_missed_the_signature(solana_world, browser):
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w)
    page.route("**/tx", lambda route: route.fulfill(status=500, body="boom"))
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('keep this page open')",
        timeout=10_000)
    assert status_text(page) == "Sent. The game will detect the payment; keep this page open."
    assert not pay_button.is_visible() and len(sent) == 1 and ctl.hint_tx is None
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)  # found by reference
    assert ctl.result.tx_hash == sent[0]
    ctl.close()


SENT_UNREPORTED = "Sent. The game will detect the payment; keep this page open."


def pay_hidden(page):
    return page.evaluate("document.getElementById('pay').classList.contains('hidden') && "
                         "document.getElementById('wallets').children.length === 0")


def test_retry_is_not_offered_again_for_a_send_the_game_never_heard_of(
        solana_world, browser, monkeypatch):
    """The first send is dropped and Pay comes back; the second send lands but its POST /tx
    fails, so the game still reports the first send's tx_not_landed. Pay must stay hidden
    until the game finds the payment by its reference."""
    monkeypatch.setattr("scfg_core.backends.solana.SOLANA_HINT_TIMEOUT_S", 3)
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w, drop_first=True)
    posts = []

    def tx_route(route):
        posts.append(route.request.post_data)
        if len(posts) == 1:
            route.continue_()
        else:
            route.fulfill(status=500, body="boom")

    page.route("**/tx", tx_route)
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function("document.getElementById('pay').classList.contains('hidden')",
                           timeout=10_000)
    pump_until_page_says(page, "didn't reach the network", w["engine"], ctl, timeout=30)
    pay_button.click()
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('keep this page open')",
        timeout=10_000)
    assert len(sent) == 1 and len(posts) == 2
    assert ctl.status_info()["error"] == "tx_not_landed" and ctl.hint_tx is None

    page.wait_for_timeout(5_000)                        # a few polls of the stale status
    assert status_text(page) == SENT_UNREPORTED
    assert pay_hidden(page)

    pump_until_page_says(page, "Confirming on", w["engine"], ctl)   # found by its reference
    assert ctl.status_info()["tx_hash"] is None and pay_hidden(page)
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert pay_hidden(page) and ctl.result.tx_hash == sent[0]
    ctl.close()


def test_retry_is_not_offered_while_the_next_signature_is_on_its_way(
        solana_world, browser, monkeypatch):
    """Second trigger of the same race: statuses served before the game has the second
    signature still carry the first send's tx_not_landed."""
    monkeypatch.setattr("scfg_core.backends.solana.SOLANA_HINT_TIMEOUT_S", 3)
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w, drop_first=True)
    posts, held = [], []

    def tx_route(route):
        posts.append(route.request.post_data)
        if len(posts) == 1:
            route.continue_()
        else:
            held.append(route)                          # the second POST: answer it later

    page.route("**/tx", tx_route)
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function("document.getElementById('pay').classList.contains('hidden')",
                           timeout=10_000)
    pump_until_page_says(page, "didn't reach the network", w["engine"], ctl, timeout=30)
    pay_button.click()
    page.wait_for_timeout(5_000)
    assert len(held) == 1 and len(sent) == 1
    assert ctl.status_info()["error"] == "tx_not_landed"
    assert pay_hidden(page) and "didn't reach" not in status_text(page)

    held[0].continue_()
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert pay_hidden(page) and ctl.result.tx_hash == sent[0] == ctl.hint_tx
    ctl.close()


def test_pay_is_hidden_while_a_payment_made_elsewhere_confirms(solana_world, browser):
    """A payment the game matched by its reference (no signature) still hides Pay."""
    import base64

    w = solana_world
    ctl, url = open_checkout(w)
    page, _ = new_page(browser, w)
    page.goto(url)
    pay_button = connect_wallet(page)
    assert pay_button.is_visible()
    built = w["engine"].build_solana_payment(ctl.session_id, str(w["player"].pubkey()))
    sig = w["cluster"].sign_and_send(base64.b64decode(built["transaction"]), w["player"])

    pump_until_page_says(page, "Confirming on", w["engine"], ctl)
    assert ctl.status_info()["tx_hash"] is None and pay_hidden(page)
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert pay_hidden(page) and ctl.result.tx_hash == sig
    ctl.close()


def test_retry_after_a_reload_offers_the_wallets(solana_world, browser, monkeypatch):
    """A reloaded page knows the send only from the game's status and has no connected
    account, so the retry must show the wallet list, not an empty Pay button."""
    monkeypatch.setattr("scfg_core.backends.solana.SOLANA_HINT_TIMEOUT_S", 3)
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w, drop_first=True)
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function("document.getElementById('pay').classList.contains('hidden')",
                           timeout=10_000)
    assert ctl.hint_tx is not None
    page.reload()
    page.wait_for_timeout(1_500)
    assert pay_hidden(page)                             # sent, as told by the game's status

    pump_until_page_says(page, "didn't reach the network", w["engine"], ctl, timeout=30)
    assert not pay_button.is_visible()
    pay_button = connect_wallet(page)                   # the wallet list is back
    assert re.match(r"^Pay \d", pay_button.inner_text()) and pay_button.is_enabled()
    pay_button.click()
    pump_until_page_says(page, "Payment confirmed", w["engine"], ctl)
    assert len(sent) == 1 and ctl.result.tx_hash == sent[0]
    ctl.close()


def test_rejection_is_reported(solana_world, browser):
    w = solana_world
    ctl, url = open_checkout(w)
    page, sent = new_page(browser, w, reject=True)
    page.goto(url)
    pay_button = connect_wallet(page)
    pay_button.click()
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('You rejected the request')",
        timeout=10_000)
    assert pay_button.is_enabled()                      # player can try again
    assert sent == []
    assert w["engine"].pending[ctl.session_id]["hint_tx"] is None
    ctl.close()


def test_session_json_is_solana_shaped(solana_world, browser):
    w = solana_world
    ctl, url = open_checkout(w)
    page, _ = new_page(browser, w)
    page.goto(url)
    session = page.evaluate("fetch('session.json').then(r => r.json())")
    assert session["family"] == "solana"
    assert session["wallet_chain"] == "solana:devnet"
    ctl.close()
