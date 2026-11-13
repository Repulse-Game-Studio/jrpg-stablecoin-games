"""The GDScript core (godot/addons/scfg/core) against the same local chains as the Python core.

Twins of exactly the tests test_js_core.py twins (the non-browser tests in test_anvil_e2e.py,
test_arc_anvil.py and test_solana_local.py), with the same fixtures and assertions. They reuse
the Python fixtures (world, arc_world, solana_world) for the chain and the funded accounts, and
drive the GDScript Engine in a headless Godot through gd_driver.GdEngine. Each fixture's
registry monkeypatch is mirrored into ScfgChains.MAINNET/TESTNET through the driver's
`override`.

Run with:  SCFG_GODOT=<godot 4.3+ binary> uv run pytest -m integration tests/integration/test_gd_core.py
(after `$SCFG_GODOT --headless --path godot --import` once). Skipped when SCFG_GODOT is unset or
missing (failed instead with SCFG_REQUIRE_GD=1), or when anvil or solana-test-validator is missing.
"""
import base64
from decimal import Decimal

import pytest
from gd_driver import GdEngine
from test_js_core import (
    CHAPTER2,
    solana_override,
    tick_until,
    tick_until_status,
    units_to_amount,
)

from scfg_core.chains import ARC_SYSTEM_EMITTER
from scfg_core.solana.base58 import b58encode

from conftest import mine, native_send, pay

pytestmark = pytest.mark.integration


@pytest.fixture
def gd_world(world):
    """The `world` fixture's "arbitrum" patch (local rpc, 3 confirmations, mock USDC/USDT)."""
    chain = world["engine"].chain("arbitrum")
    override = {"network": "mainnet", "key": "arbitrum", "rpc": list(chain.rpc),
                "confirmations": chain.confirmations,
                "tokens": {name: t.address for name, t in chain.tokens.items()}}
    gd = GdEngine(override, {"wallet": world["dev"], "chains": ["arbitrum"]}, [CHAPTER2])
    yield gd
    gd.close()


@pytest.fixture
def gd_arc(arc_world):
    """The `arc_world` fixture's patch: only the rpc is replaced, the real tokens and emitter stay."""
    override = {"network": "mainnet", "key": "arc", "rpc": [arc_world["url"]]}
    gd = GdEngine(override, {"wallet": arc_world["dev"], "chains": ["arc"]}, [CHAPTER2])
    yield gd
    gd.close()


@pytest.fixture
def gd_solana(solana_world):
    cfg = {"solana_wallet": str(solana_world["dev"].pubkey()), "testnet": True, "chains": ["solana"]}
    gd = GdEngine(solana_override(solana_world), cfg, [CHAPTER2])
    yield gd
    gd.close()


# --- test_anvil_e2e.py ---------------------------------------------------------------------------

def test_amount_scan_payment_is_detected_and_confirmed(world, gd_world):
    gd, rpc = gd_world, world["rpc"]
    rec = gd.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    assert tick_until(gd, "confirming")["confirmations"] == 1
    mine(rpc, 2)
    out = tick_until(gd, "paid")
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert gd.snapshot()["owned"] == ["chapter2"]


def test_hinted_exchange_style_payment(world, gd_world):
    gd, rpc = gd_world, world["rpc"]
    rec = gd.start_session("unlock", "arbitrum", item="chapter2")
    tx = pay(world, 5_000_000)                 # round amount: amount scan cannot match it
    gd.report_hint(rec["id"], tx)
    mine(rpc, 3)
    assert tick_until(gd, "paid")["result"]["tx_hash"] == tx.lower()


def test_reorg_removes_unconfirmed_payment(world, gd_world):
    gd, rpc = gd_world, world["rpc"]
    rec = gd.start_session("unlock", "arbitrum", item="chapter2")
    snap_id = rpc.call("evm_snapshot", [])
    pay(world, rec["units"])
    tick_until(gd, "confirming")
    rpc.call("evm_revert", [snap_id])          # the payment block disappears
    mine(rpc, 5)                               # canonical chain moves past it without the tx
    tick_until(gd, "reorged")
    assert gd.snapshot()["pending"][rec["id"]]["state"] == "awaiting"
    assert gd.snapshot()["owned"] == []


def test_reverify_keeps_real_receipts_and_drops_invented_ones(world, gd_world):
    gd, rpc = gd_world, world["rpc"]
    rec = gd.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    mine(rpc, 3)
    tick_until(gd, "paid")
    receipts = gd.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_key = "arbitrum:" + "0x" + "ee" * 32 + ":0"
    gd.set_receipt(fake_key, dict(receipts[real_key], tx_hash="0x" + "ee" * 32))
    assert gd.reverify() == [fake_key]
    assert list(gd.snapshot()["receipts"]) == [real_key]


def test_developer_defined_chain(world):
    """A chain added with scfg.add_chain(): Anvil over plain http on localhost."""
    rpc = world["rpc"]
    custom = {"key": "devnet", "name": "Local devnet", "chain_id": int(rpc.call("eth_chainId", []), 16),
              "rpc": world["url"], "tokens": {"USDC": world["usdc"]}, "confirmations": 2,
              "explorer_tx": "https://explorer.invalid/tx/{}"}
    gd = GdEngine(None, {"wallet": world["dev"], "chains": ["devnet"]}, [CHAPTER2], custom_chains=[custom])
    try:
        rec = gd.start_session("unlock", "devnet", item="chapter2")
        tx = pay(world, rec["units"])
        mine(rpc, 2)
        assert tick_until(gd, "paid")["result"]["chain"] == "devnet"
        assert gd.reverify() == []
        gd.clear_receipts()
        found = gd.restore_lookup(tx)
        assert [(c["chain"], c["item"]) for c in found] == [("devnet", "chapter2")]
    finally:
        gd.close()


# --- test_arc_anvil.py ---------------------------------------------------------------------------

def test_arc_native_send_is_detected(arc_world, gd_arc):
    gd = gd_arc
    rec = gd.start_session("unlock", "arc", item="chapter2")
    tx = native_send(arc_world, rec["units"])
    out = tick_until(gd, "paid")                     # one block is final on Arc
    assert out["result"]["tx_hash"] == tx.lower()
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert gd.snapshot()["owned"] == ["chapter2"]


def test_arc_transfer_is_detected_once(arc_world, gd_arc):
    gd, rpc = gd_arc, arc_world["rpc"]
    rec = gd.start_session("unlock", "arc", item="chapter2")
    tx = pay(arc_world, rec["units"])
    logs = rpc.call("eth_getTransactionReceipt", [tx])["logs"]
    assert [lg["address"].lower() for lg in logs] == [
        ARC_SYSTEM_EMITTER.lower(), arc_world["usdc"].lower()]
    tick_until(gd, "paid")
    receipts = gd.snapshot()["receipts"]
    assert len(receipts) == 1
    (key,) = receipts
    assert key == f"arc:{tx.lower()}:{int(logs[0]['logIndex'], 16)}"


def test_arc_restore_finds_native_send(arc_world, gd_arc):
    gd = gd_arc
    tx = native_send(arc_world, 4_990_000)           # round 4.99: the player must choose
    (c,) = gd.restore_lookup(tx)
    assert (c["chain"], c["token"], c["transfer"]["value"], c["kind"]) == ("arc", "USDC", 4_990_000, None)
    added = gd.restore_apply([c], {0: "chapter2"})
    assert len(added) == 1
    assert gd.snapshot()["owned"] == ["chapter2"]


def test_reverify_keeps_arc_receipts(arc_world, gd_arc):
    gd = gd_arc
    for send_payment in (native_send, pay):
        rec = gd.start_session("unlock", "arc", item="chapter2")
        send_payment(arc_world, rec["units"])
        tick_until(gd, "paid")
    real = gd.snapshot()["receipts"]
    assert len(real) == 2
    fake_key = "arc:0x" + "ee" * 32 + ":0"
    gd.set_receipt(fake_key, dict(next(iter(real.values())), tx_hash="0x" + "ee" * 32))
    assert gd.reverify() == [fake_key]
    assert gd.snapshot()["receipts"] == real


# --- test_solana_local.py ------------------------------------------------------------------------

def test_game_built_transaction_pays_and_finalizes(solana_world, gd_solana):
    w, gd = solana_world, gd_solana
    rec = gd.start_session("unlock", "solana", item="chapter2")
    built = gd.build_solana_payment(rec["id"], str(w["player"].pubkey()))
    assert int(built["token_balance"]) == 100_000_000
    w["cluster"].sign_and_send(base64.b64decode(built["transaction"]), w["player"])
    tick_until_status(gd, "confirming")
    out = tick_until_status(gd, "paid")
    assert out["result"]["payer"] == str(w["player"].pubkey())
    assert gd.snapshot()["owned"] == ["chapter2"]


def test_exchange_style_exact_transfer_is_found_by_scan(solana_world, gd_solana):
    w, gd = solana_world, gd_solana
    rec = gd.start_session("unlock", "solana", item="chapter2")
    w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"])
    out = tick_until_status(gd, "paid")
    assert Decimal(out["result"]["amount"]) * 10**6 == rec["units"]


def test_hinted_round_amount(solana_world, gd_solana):
    w, gd = solana_world, gd_solana
    rec = gd.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), 5_000_000)
    gd.report_hint(rec["id"], sig)
    assert tick_until_status(gd, "paid")["result"]["tx_hash"] == sig


def test_reverify_and_restore(solana_world, gd_solana):
    w, gd = solana_world, gd_solana
    rec = gd.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"],
                                reference=rec["reference"])
    tick_until_status(gd, "paid")
    receipts = gd.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_sig = b58encode(bytes(range(64)))
    fake_key = f"solana:{fake_sig}:2"
    gd.set_receipt(fake_key, dict(receipts[real_key], tx_hash=fake_sig))
    assert gd.reverify() == [fake_key] and list(gd.snapshot()["receipts"]) == [real_key]
    gd.clear_receipts()
    [candidate] = gd.restore_lookup(sig)
    assert (candidate["chain"], candidate["item"]) == ("solana", "chapter2")


def test_missing_token_account_marks_usdc_unready(solana_world, gd_solana):
    from solders.keypair import Keypair
    stranger = GdEngine(solana_override(solana_world),
                        {"solana_wallet": str(Keypair().pubkey()), "testnet": True, "chains": ["solana"]},
                        [CHAPTER2])
    try:
        stranger.reverify()
        assert stranger.snapshot()["unready"] == [["solana", "USDC"]]
    finally:
        stranger.close()
    gd_solana.reverify()                         # the developer's token account exists
    assert gd_solana.snapshot()["unready"] == []
