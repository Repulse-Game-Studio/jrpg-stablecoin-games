"""End-to-end tests against a local Anvil chain (Foundry).

Run with:  uv run pytest -m integration
Skipped automatically when `anvil` is not on PATH.
"""
import pytest

from cryptopay_core.amounts import units_to_decimal

from conftest import mine, pay, tick_until

pytestmark = pytest.mark.integration


def test_amount_scan_payment_is_detected_and_confirmed(world):
    engine, rpc = world["engine"], world["rpc"]
    rec = engine.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    assert tick_until(engine, "confirming").confirmations == 1
    mine(rpc, 2)
    out = tick_until(engine, "paid")
    assert out.result.amount == units_to_decimal(rec["units"])
    assert engine.snapshot().owned == frozenset({"chapter2"})


def test_hinted_exchange_style_payment(world):
    engine, rpc = world["engine"], world["rpc"]
    rec = engine.start_session("unlock", "arbitrum", item="chapter2")
    tx = pay(world, 5_000_000)                 # round amount: amount scan cannot match it
    engine.report_hint(rec["id"], tx)
    mine(rpc, 3)
    assert tick_until(engine, "paid").result.tx_hash == tx.lower()


def test_reorg_removes_unconfirmed_payment(world):
    engine, rpc = world["engine"], world["rpc"]
    rec = engine.start_session("unlock", "arbitrum", item="chapter2")
    snap_id = rpc.call("evm_snapshot", [])
    pay(world, rec["units"])
    tick_until(engine, "confirming")
    rpc.call("evm_revert", [snap_id])          # the payment block disappears
    mine(rpc, 5)                               # canonical chain moves past it without the tx
    tick_until(engine, "reorged")
    assert engine.pending[rec["id"]]["state"] == "awaiting"
    assert engine.snapshot().owned == frozenset()


def test_reverify_keeps_real_receipts_and_drops_invented_ones(world):
    engine, rpc = world["engine"], world["rpc"]
    rec = engine.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    mine(rpc, 3)
    tick_until(engine, "paid")
    real_key = next(iter(engine.receipts))
    fake = dict(engine.receipts[real_key], tx_hash="0x" + "ee" * 32)
    engine.receipts["arbitrum:" + "0x" + "ee" * 32 + ":0"] = fake
    assert engine.reverify() == ["arbitrum:" + "0x" + "ee" * 32 + ":0"]
    assert list(engine.receipts) == [real_key]


def test_developer_defined_chain(world):
    """A chain added with cryptopay.add_chain(): Anvil over plain http on localhost."""
    from cryptopay_core.config import add_item, build_chain, build_config
    from cryptopay_core.engine import Engine
    from cryptopay_core.rpc import UrllibTransport

    rpc = world["rpc"]
    local = build_chain("devnet", name="Local devnet", chain_id=int(rpc.call("eth_chainId", []), 16),
                        rpc=world["url"], tokens={"USDC": world["usdc"]}, confirmations=2,
                        explorer_tx="https://explorer.invalid/tx/{}")
    cfg = build_config(world["dev"], chains=["devnet"], custom_chains=[local])
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=UrllibTransport())
    rec = engine.start_session("unlock", "devnet", item="chapter2")
    tx = pay(world, rec["units"])
    mine(rpc, 2)
    assert tick_until(engine, "paid").result.chain == "devnet"
    assert engine.reverify() == []
    engine.receipts.clear()
    found = engine.restore_lookup(tx)
    assert [(c.chain, c.item) for c in found] == [("devnet", "chapter2")]
