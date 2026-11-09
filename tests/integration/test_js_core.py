"""The TypeScript core (rpgmaker/src/core) against the same local chains as the Python core.

Twins of the non-browser tests in test_anvil_e2e.py, test_arc_anvil.py and test_solana_local.py.
They reuse the Python fixtures (world, arc_world, solana_world) for the chain and the funded
accounts, and drive the JS Engine through js_driver.JsEngine. Each fixture's registry
monkeypatch is mirrored into the JS registry through the driver's `override`.

Run with:  uv run pytest -m integration tests/integration/test_js_core.py
Skipped when node, rpgmaker/node_modules, anvil or solana-test-validator is missing.
"""
import base64
import time
from decimal import Decimal

import pytest
from js_driver import JsEngine

from scfg_core.chains import ARC_SYSTEM_EMITTER
from scfg_core.solana.base58 import b58encode

from conftest import mine, native_send, pay

pytestmark = pytest.mark.integration

CHAPTER2 = {"item_id": "chapter2", "code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}


def tick_until(js, status, rounds=10):
    seen = []
    for _ in range(rounds):
        for o in js.tick():
            seen.append(o["status"])
            if o["status"] == status:
                return o
    raise AssertionError(f"never reached {status}; saw {seen}")


def tick_until_status(js, status, timeout=90):
    seen = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        for o in js.tick():
            seen.append(o["status"])
            if o["status"] == status:
                return o
        time.sleep(0.5)
    raise AssertionError(f"never reached {status}; saw {seen[-20:]}")


def units_to_amount(units):
    return Decimal(units) / 10**6


@pytest.fixture
def js_world(world):
    """The `world` fixture's "arbitrum" patch (local rpc, 3 confirmations, mock USDC/USDT)."""
    chain = world["engine"].chain("arbitrum")
    override = {"network": "mainnet", "key": "arbitrum", "rpc": list(chain.rpc),
                "confirmations": chain.confirmations,
                "tokens": {name: t.address for name, t in chain.tokens.items()}}
    js = JsEngine(override, {"wallet": world["dev"], "chains": ["arbitrum"]}, [CHAPTER2])
    yield js
    js.close()


@pytest.fixture
def js_arc(arc_world):
    """The `arc_world` fixture's patch: only the rpc is replaced, the real tokens and emitter stay."""
    override = {"network": "mainnet", "key": "arc", "rpc": [arc_world["url"]]}
    js = JsEngine(override, {"wallet": arc_world["dev"], "chains": ["arc"]}, [CHAPTER2])
    yield js
    js.close()


def solana_override(w):
    chain = w["engine"].chain("solana")
    return {"network": "testnet", "key": "solana", "rpc": list(chain.rpc),
            "tokens": {name: t.address for name, t in chain.tokens.items()}}


@pytest.fixture
def js_solana(solana_world):
    cfg = {"solana_wallet": str(solana_world["dev"].pubkey()), "testnet": True, "chains": ["solana"]}
    js = JsEngine(solana_override(solana_world), cfg, [CHAPTER2])
    yield js
    js.close()


# --- test_anvil_e2e.py ---------------------------------------------------------------------------

def test_amount_scan_payment_is_detected_and_confirmed(world, js_world):
    js, rpc = js_world, world["rpc"]
    rec = js.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    assert tick_until(js, "confirming")["confirmations"] == 1
    mine(rpc, 2)
    out = tick_until(js, "paid")
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert js.snapshot()["owned"] == ["chapter2"]


def test_hinted_exchange_style_payment(world, js_world):
    js, rpc = js_world, world["rpc"]
    rec = js.start_session("unlock", "arbitrum", item="chapter2")
    tx = pay(world, 5_000_000)                 # round amount: amount scan cannot match it
    js.report_hint(rec["id"], tx)
    mine(rpc, 3)
    assert tick_until(js, "paid")["result"]["tx_hash"] == tx.lower()


def test_reorg_removes_unconfirmed_payment(world, js_world):
    js, rpc = js_world, world["rpc"]
    rec = js.start_session("unlock", "arbitrum", item="chapter2")
    snap_id = rpc.call("evm_snapshot", [])
    pay(world, rec["units"])
    tick_until(js, "confirming")
    rpc.call("evm_revert", [snap_id])          # the payment block disappears
    mine(rpc, 5)                               # canonical chain moves past it without the tx
    tick_until(js, "reorged")
    assert js.snapshot()["pending"][rec["id"]]["state"] == "awaiting"
    assert js.snapshot()["owned"] == []


def test_reverify_keeps_real_receipts_and_drops_invented_ones(world, js_world):
    js, rpc = js_world, world["rpc"]
    rec = js.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    mine(rpc, 3)
    tick_until(js, "paid")
    receipts = js.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_key = "arbitrum:" + "0x" + "ee" * 32 + ":0"
    js.set_receipt(fake_key, dict(receipts[real_key], tx_hash="0x" + "ee" * 32))
    assert js.reverify() == [fake_key]
    assert list(js.snapshot()["receipts"]) == [real_key]


def test_developer_defined_chain(world):
    """A chain added with scfg.add_chain(): Anvil over plain http on localhost."""
    rpc = world["rpc"]
    custom = {"key": "devnet", "name": "Local devnet", "chain_id": int(rpc.call("eth_chainId", []), 16),
              "rpc": world["url"], "tokens": {"USDC": world["usdc"]}, "confirmations": 2,
              "explorer_tx": "https://explorer.invalid/tx/{}"}
    js = JsEngine(None, {"wallet": world["dev"], "chains": ["devnet"]}, [CHAPTER2], custom_chains=[custom])
    try:
        rec = js.start_session("unlock", "devnet", item="chapter2")
        tx = pay(world, rec["units"])
        mine(rpc, 2)
        assert tick_until(js, "paid")["result"]["chain"] == "devnet"
        assert js.reverify() == []
        js.clear_receipts()
        found = js.restore_lookup(tx)
        assert [(c["chain"], c["item"]) for c in found] == [("devnet", "chapter2")]
    finally:
        js.close()


# --- test_arc_anvil.py ---------------------------------------------------------------------------

def test_arc_native_send_is_detected(arc_world, js_arc):
    js = js_arc
    rec = js.start_session("unlock", "arc", item="chapter2")
    tx = native_send(arc_world, rec["units"])
    out = tick_until(js, "paid")                     # one block is final on Arc
    assert out["result"]["tx_hash"] == tx.lower()
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert js.snapshot()["owned"] == ["chapter2"]


def test_arc_transfer_is_detected_once(arc_world, js_arc):
    js, rpc = js_arc, arc_world["rpc"]
    rec = js.start_session("unlock", "arc", item="chapter2")
    tx = pay(arc_world, rec["units"])
    logs = rpc.call("eth_getTransactionReceipt", [tx])["logs"]
    assert [lg["address"].lower() for lg in logs] == [
        ARC_SYSTEM_EMITTER.lower(), arc_world["usdc"].lower()]
    tick_until(js, "paid")
    receipts = js.snapshot()["receipts"]
    assert len(receipts) == 1
    (key,) = receipts
    assert key == f"arc:{tx.lower()}:{int(logs[0]['logIndex'], 16)}"


def test_arc_restore_finds_native_send(arc_world, js_arc):
    js = js_arc
    tx = native_send(arc_world, 4_990_000)           # round 4.99: the player must choose
    (c,) = js.restore_lookup(tx)
    assert (c["chain"], c["token"], c["transfer"]["value"], c["kind"]) == ("arc", "USDC", 4_990_000, None)
    added = js.restore_apply([c], {0: "chapter2"})
    assert len(added) == 1
    assert js.snapshot()["owned"] == ["chapter2"]


def test_reverify_keeps_arc_receipts(arc_world, js_arc):
    js = js_arc
    for send_payment in (native_send, pay):
        rec = js.start_session("unlock", "arc", item="chapter2")
        send_payment(arc_world, rec["units"])
        tick_until(js, "paid")
    real = js.snapshot()["receipts"]
    assert len(real) == 2
    fake_key = "arc:0x" + "ee" * 32 + ":0"
    js.set_receipt(fake_key, dict(next(iter(real.values())), tx_hash="0x" + "ee" * 32))
    assert js.reverify() == [fake_key]
    assert js.snapshot()["receipts"] == real


# --- test_solana_local.py ------------------------------------------------------------------------

def test_game_built_transaction_pays_and_finalizes(solana_world, js_solana):
    w, js = solana_world, js_solana
    rec = js.start_session("unlock", "solana", item="chapter2")
    built = js.build_solana_payment(rec["id"], str(w["player"].pubkey()))
    assert int(built["token_balance"]) == 100_000_000
    w["cluster"].sign_and_send(base64.b64decode(built["transaction"]), w["player"])
    tick_until_status(js, "confirming")
    out = tick_until_status(js, "paid")
    assert out["result"]["payer"] == str(w["player"].pubkey())
    assert js.snapshot()["owned"] == ["chapter2"]


def test_exchange_style_exact_transfer_is_found_by_scan(solana_world, js_solana):
    w, js = solana_world, js_solana
    rec = js.start_session("unlock", "solana", item="chapter2")
    w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"])
    out = tick_until_status(js, "paid")
    assert Decimal(out["result"]["amount"]) * 10**6 == rec["units"]


def test_hinted_round_amount(solana_world, js_solana):
    w, js = solana_world, js_solana
    rec = js.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), 5_000_000)
    js.report_hint(rec["id"], sig)
    assert tick_until_status(js, "paid")["result"]["tx_hash"] == sig


def test_reverify_and_restore(solana_world, js_solana):
    w, js = solana_world, js_solana
    rec = js.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"],
                                reference=rec["reference"])
    tick_until_status(js, "paid")
    receipts = js.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_sig = b58encode(bytes(range(64)))
    fake_key = f"solana:{fake_sig}:2"
    js.set_receipt(fake_key, dict(receipts[real_key], tx_hash=fake_sig))
    assert js.reverify() == [fake_key] and list(js.snapshot()["receipts"]) == [real_key]
    js.clear_receipts()
    [candidate] = js.restore_lookup(sig)
    assert (candidate["chain"], candidate["item"]) == ("solana", "chapter2")


def test_missing_token_account_marks_usdc_unready(solana_world, js_solana):
    from solders.keypair import Keypair
    stranger = JsEngine(solana_override(solana_world),
                        {"solana_wallet": str(Keypair().pubkey()), "testnet": True, "chains": ["solana"]},
                        [CHAPTER2])
    try:
        stranger.reverify()
        assert stranger.snapshot()["unready"] == [["solana", "USDC"]]
    finally:
        stranger.close()
    js_solana.reverify()                         # the developer's token account exists
    assert js_solana.snapshot()["unready"] == []
