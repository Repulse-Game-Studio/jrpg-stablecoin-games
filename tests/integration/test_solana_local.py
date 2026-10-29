"""End-to-end Solana tests against a local solana-test-validator.

Run with anvil and solana-test-validator on PATH:  uv run pytest -m integration
"""
import base64

import pytest
from solders.keypair import Keypair

from cryptopay_core.engine import Engine
from cryptopay_core.rpc import UrllibTransport
from cryptopay_core.solana.base58 import b58encode

from conftest import tick_until_status

pytestmark = pytest.mark.integration


def test_game_built_transaction_pays_and_finalizes(solana_world):
    w = solana_world
    engine = w["engine"]
    rec = engine.start_session("unlock", "solana", item="chapter2")
    built = engine.build_solana_payment(rec["id"], str(w["player"].pubkey()))
    assert int(built["token_balance"]) == 100_000_000
    w["cluster"].sign_and_send(base64.b64decode(built["transaction"]), w["player"])
    tick_until_status(engine, "confirming")
    out = tick_until_status(engine, "paid")
    assert out.result.payer == str(w["player"].pubkey())
    assert engine.snapshot().owned == frozenset({"chapter2"})


def test_exchange_style_exact_transfer_is_found_by_scan(solana_world):
    w = solana_world
    engine = w["engine"]
    rec = engine.start_session("unlock", "solana", item="chapter2")
    w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"])
    assert tick_until_status(engine, "paid").result.amount * 10**6 == rec["units"]


def test_hinted_round_amount(solana_world):
    w = solana_world
    engine = w["engine"]
    rec = engine.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), 5_000_000)
    engine.report_hint(rec["id"], sig)
    assert tick_until_status(engine, "paid").result.tx_hash == sig


def test_reverify_and_restore(solana_world):
    w = solana_world
    engine = w["engine"]
    rec = engine.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"],
                                reference=rec["reference"])
    tick_until_status(engine, "paid")
    real_key = next(iter(engine.receipts))
    fake_sig = b58encode(bytes(range(64)))
    fake_key = f"solana:{fake_sig}:2"
    engine.receipts[fake_key] = dict(engine.receipts[real_key], tx_hash=fake_sig)
    assert engine.reverify() == [fake_key] and list(engine.receipts) == [real_key]
    engine.receipts.clear()
    [candidate] = engine.restore_lookup(sig)
    assert (candidate.chain, candidate.item) == ("solana", "chapter2")


def test_missing_token_account_marks_usdc_unready(solana_world):
    w = solana_world
    from cryptopay_core.config import add_item, build_config
    cfg = build_config(solana_wallet=str(Keypair().pubkey()), testnet=True, chains=["solana"])
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=UrllibTransport())
    engine.reverify()
    assert engine.snapshot().unready == frozenset({("solana", "USDC")})
    w["engine"].reverify()                       # the developer's token account exists
    assert w["engine"].snapshot().unready == frozenset()
