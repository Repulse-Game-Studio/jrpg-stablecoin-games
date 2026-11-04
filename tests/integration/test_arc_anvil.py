"""Arc on Anvil: the USDC interface and the system emitter emulated at their real
addresses (contracts/ArcMockUsdc.sol, contracts/ArcSystemEmitter.sol).

Run with:  uv run pytest -m integration
Skipped automatically when `anvil` is not on PATH.
"""
import re
import time

import pytest

from scfg_core.amounts import units_to_decimal
from scfg_core.chains import ARC_MIN_FEE_WEI, ARC_SYSTEM_EMITTER

from browser_mocks import install_mock_wallet
from conftest import addr_word, native_send, pay, send, tick_until, word
from test_browser_checkout import browser, open_checkout, status_text  # noqa: F401 (fixture)

pytestmark = pytest.mark.integration


def test_arc_native_send_is_detected(arc_world):
    engine = arc_world["engine"]
    rec = engine.start_session("unlock", "arc", item="chapter2")
    tx = native_send(arc_world, rec["units"])
    out = tick_until(engine, "paid")                 # one block is final on Arc
    assert out.result.tx_hash == tx.lower()
    assert out.result.amount == units_to_decimal(rec["units"])
    assert engine.snapshot().owned == frozenset({"chapter2"})


def test_arc_transfer_is_detected_once(arc_world):
    engine, rpc = arc_world["engine"], arc_world["rpc"]
    rec = engine.start_session("unlock", "arc", item="chapter2")
    tx = pay(arc_world, rec["units"])
    logs = rpc.call("eth_getTransactionReceipt", [tx])["logs"]
    assert [lg["address"].lower() for lg in logs] == [
        ARC_SYSTEM_EMITTER.lower(), arc_world["usdc"].lower()]
    tick_until(engine, "paid")
    assert len(engine.receipts) == 1
    (key,) = engine.receipts
    assert key == f"arc:{tx.lower()}:{int(logs[0]['logIndex'], 16)}"


def test_arc_restore_finds_native_send(arc_world):
    engine = arc_world["engine"]
    tx = native_send(arc_world, 4_990_000)           # round 4.99: the player must choose
    (c,) = engine.restore_lookup(tx)
    assert (c.chain, c.token, c.transfer.value, c.kind) == ("arc", "USDC", 4_990_000, None)
    added = engine.restore_apply([c], {0: "chapter2"})
    assert len(added) == 1
    assert engine.snapshot().owned == frozenset({"chapter2"})


def test_reverify_keeps_arc_receipts(arc_world):
    engine = arc_world["engine"]
    for send_payment in (native_send, pay):
        rec = engine.start_session("unlock", "arc", item="chapter2")
        send_payment(arc_world, rec["units"])
        tick_until(engine, "paid")
    assert len(engine.receipts) == 2
    real = dict(engine.receipts)
    fake_key = "arc:0x" + "ee" * 32 + ":0"
    engine.receipts[fake_key] = dict(next(iter(real.values())), tx_hash="0x" + "ee" * 32)
    assert engine.reverify() == [fake_key]
    assert engine.receipts == real


class RecordingRpc:
    """Passes the mock wallet's calls to Anvil and keeps every eth_sendTransaction."""

    def __init__(self, rpc):
        self.rpc, self.sent = rpc, []

    def call(self, method, params):
        if method == "eth_sendTransaction":
            self.sent.append(params[0])
        return self.rpc.call(method, params)


def test_browser_pays_on_arc_with_fee_floor(arc_world, browser):  # noqa: F811
    ctl, url = open_checkout(arc_world)
    wallet_rpc = RecordingRpc(arc_world["rpc"])
    page = browser.new_page()
    install_mock_wallet(page, wallet_rpc, arc_world["payer"])
    page.goto(url)
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    pay_button = page.get_by_role("button", name=re.compile(r"^Pay \d"))
    pay_button.wait_for()
    page.wait_for_function("document.getElementById('status').textContent.includes('Ready')",
                           timeout=10_000)
    assert "network fee" not in status_text(page)   # the gas coin is the USDC it holds
    pay_button.click()
    deadline = time.time() + 10
    while arc_world["engine"].pending[ctl.session_id]["hint_tx"] is None:
        assert time.time() < deadline, "page never reported the tx: " + status_text(page)
        # Not time.sleep: the mock wallet's Python callbacks only run inside Playwright calls.
        page.wait_for_timeout(50)
    (tx,) = wallet_rpc.sent
    assert int(tx["maxFeePerGas"], 16) >= ARC_MIN_FEE_WEI
    tick_until(arc_world["engine"], "paid", on_outcome=ctl.on_outcome)
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('Payment confirmed')",
        timeout=10_000)
    ctl.close()


def test_page_warns_on_arc_when_fee_would_not_fit(arc_world, browser):  # noqa: F811
    ctl, url = open_checkout(arc_world)
    # Leave the payer exactly the price: nothing would remain for the USDC network fee.
    have = int(arc_world["rpc"].call("eth_call", [{
        "to": arc_world["usdc"],
        "data": "0x70a08231" + "0" * 24 + arc_world["payer"][2:].lower()}, "latest"]), 16)
    need = ctl.session["units"]
    send(arc_world["rpc"], arc_world["payer"], arc_world["usdc"],
         "0xa9059cbb" + addr_word(arc_world["dev"]) + word(have - need))
    page = browser.new_page()
    install_mock_wallet(page, arc_world["rpc"], arc_world["payer"])
    page.goto(url)
    page.get_by_role("button", name="Pay with Mock Wallet").click()
    page.wait_for_function(
        "document.getElementById('status').textContent.includes('network fee')", timeout=10_000)
    assert "charges in USDC" in status_text(page)
    assert page.get_by_role("button", name=re.compile(r"^Pay \d")).is_enabled()
    ctl.close()
