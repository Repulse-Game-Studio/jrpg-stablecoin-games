"""The C++ core (unreal/Plugins/Scfg/Source/ScfgCore) against the same local chains as the Python core.

Twins of exactly the tests test_cs_core.py twins (the non-browser tests in test_anvil_e2e.py,
test_arc_anvil.py and test_solana_local.py, plus test_driver_smoke), with the same fixtures and
assertions. They reuse the Python fixtures (world, arc_world, solana_world) for the chain and the
funded accounts, and drive the C++ Engine in a console process (unreal/core-driver) through
cpp_driver.CppEngine. Each fixture's registry monkeypatch is mirrored into chains::MAINNET()/TESTNET()
through the driver's `override`.

Run with:  uv run pytest -m integration tests/integration/test_cpp_core.py
(cmake is found on PATH or in ~/.local/bin, the compiler is $CXX or c++/g++/clang++; the driver is
built once per session into unreal/build/integration with ninja, else make). Skipped when cmake or a
compiler is missing, when cmake is older than 3.25 (or reports no version), or when neither ninja
nor make is found (failed instead with SCFG_REQUIRE_CPP=1; once those checks pass, a configure or
build that fails always fails), or when anvil or solana-test-validator is missing.
test_driver_smoke needs no chain.
"""
import base64
from decimal import Decimal

import pytest
from cpp_driver import CppEngine, CppEngineError
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
def cpp_world(world):
    """The `world` fixture's "arbitrum" patch (local rpc, 3 confirmations, mock USDC/USDT)."""
    chain = world["engine"].chain("arbitrum")
    override = {"network": "mainnet", "key": "arbitrum", "rpc": list(chain.rpc),
                "confirmations": chain.confirmations,
                "tokens": {name: t.address for name, t in chain.tokens.items()}}
    cpp = CppEngine(override, {"wallet": world["dev"], "chains": ["arbitrum"]}, [CHAPTER2])
    yield cpp
    cpp.close()


@pytest.fixture
def cpp_arc(arc_world):
    """The `arc_world` fixture's patch: only the rpc is replaced, the real tokens and emitter stay."""
    override = {"network": "mainnet", "key": "arc", "rpc": [arc_world["url"]]}
    cpp = CppEngine(override, {"wallet": arc_world["dev"], "chains": ["arc"]}, [CHAPTER2])
    yield cpp
    cpp.close()


@pytest.fixture
def cpp_solana(solana_world):
    cfg = {"solana_wallet": str(solana_world["dev"].pubkey()), "testnet": True, "chains": ["solana"]}
    cpp = CppEngine(solana_override(solana_world), cfg, [CHAPTER2])
    yield cpp
    cpp.close()


# --- the driver itself (no chain needed) ----------------------------------------------------------

def test_driver_smoke():
    """init + snapshot + transport_stats + an error reply, over the real JSON-lines protocol."""
    cpp = CppEngine(None, {"wallet": "0x" + "11" * 20, "chains": ["arbitrum"]}, [CHAPTER2])
    try:
        snap = cpp.snapshot()
        assert snap["pending"] == {} and snap["receipts"] == {} and snap["owned"] == []
        assert cpp.transport_stats() == {"kind": "PosixHttpTransport", "posts": 0}
        cpp.clear_receipts()
        with pytest.raises(CppEngineError) as err:
            cpp.start_session("unlock", "nowhere", item="chapter2")
        assert err.value.type == "ConfigError" and err.value.code
        assert cpp.reverify() == []
    finally:
        cpp.close()


# --- test_anvil_e2e.py ---------------------------------------------------------------------------

def test_amount_scan_payment_is_detected_and_confirmed(world, cpp_world):
    cpp, rpc = cpp_world, world["rpc"]
    rec = cpp.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    assert tick_until(cpp, "confirming")["confirmations"] == 1
    mine(rpc, 2)
    out = tick_until(cpp, "paid")
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert cpp.snapshot()["owned"] == ["chapter2"]


def test_hinted_exchange_style_payment(world, cpp_world):
    cpp, rpc = cpp_world, world["rpc"]
    rec = cpp.start_session("unlock", "arbitrum", item="chapter2")
    tx = pay(world, 5_000_000)                 # round amount: amount scan cannot match it
    cpp.report_hint(rec["id"], tx)
    mine(rpc, 3)
    assert tick_until(cpp, "paid")["result"]["tx_hash"] == tx.lower()


def test_reorg_removes_unconfirmed_payment(world, cpp_world):
    cpp, rpc = cpp_world, world["rpc"]
    rec = cpp.start_session("unlock", "arbitrum", item="chapter2")
    snap_id = rpc.call("evm_snapshot", [])
    pay(world, rec["units"])
    tick_until(cpp, "confirming")
    rpc.call("evm_revert", [snap_id])          # the payment block disappears
    mine(rpc, 5)                               # canonical chain moves past it without the tx
    tick_until(cpp, "reorged")
    assert cpp.snapshot()["pending"][rec["id"]]["state"] == "awaiting"
    assert cpp.snapshot()["owned"] == []


def test_reverify_keeps_real_receipts_and_drops_invented_ones(world, cpp_world):
    cpp, rpc = cpp_world, world["rpc"]
    rec = cpp.start_session("unlock", "arbitrum", item="chapter2")
    pay(world, rec["units"])
    mine(rpc, 3)
    tick_until(cpp, "paid")
    receipts = cpp.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_key = "arbitrum:" + "0x" + "ee" * 32 + ":0"
    cpp.set_receipt(fake_key, dict(receipts[real_key], tx_hash="0x" + "ee" * 32))
    assert cpp.reverify() == [fake_key]
    assert list(cpp.snapshot()["receipts"]) == [real_key]


def test_developer_defined_chain(world):
    """A chain added with scfg.add_chain(): Anvil over plain http on localhost."""
    rpc = world["rpc"]
    custom = {"key": "devnet", "name": "Local devnet", "chain_id": int(rpc.call("eth_chainId", []), 16),
              "rpc": world["url"], "tokens": {"USDC": world["usdc"]}, "confirmations": 2,
              "explorer_tx": "https://explorer.invalid/tx/{}"}
    cpp = CppEngine(None, {"wallet": world["dev"], "chains": ["devnet"]}, [CHAPTER2], custom_chains=[custom])
    try:
        rec = cpp.start_session("unlock", "devnet", item="chapter2")
        tx = pay(world, rec["units"])
        mine(rpc, 2)
        assert tick_until(cpp, "paid")["result"]["chain"] == "devnet"
        assert cpp.reverify() == []
        cpp.clear_receipts()
        found = cpp.restore_lookup(tx)
        assert [(c["chain"], c["item"]) for c in found] == [("devnet", "chapter2")]
    finally:
        cpp.close()


# --- test_arc_anvil.py ---------------------------------------------------------------------------

def test_arc_native_send_is_detected(arc_world, cpp_arc):
    cpp = cpp_arc
    rec = cpp.start_session("unlock", "arc", item="chapter2")
    tx = native_send(arc_world, rec["units"])
    out = tick_until(cpp, "paid")                     # one block is final on Arc
    assert out["result"]["tx_hash"] == tx.lower()
    assert Decimal(out["result"]["amount"]) == units_to_amount(rec["units"])
    assert cpp.snapshot()["owned"] == ["chapter2"]


def test_arc_transfer_is_detected_once(arc_world, cpp_arc):
    cpp, rpc = cpp_arc, arc_world["rpc"]
    rec = cpp.start_session("unlock", "arc", item="chapter2")
    tx = pay(arc_world, rec["units"])
    logs = rpc.call("eth_getTransactionReceipt", [tx])["logs"]
    assert [lg["address"].lower() for lg in logs] == [
        ARC_SYSTEM_EMITTER.lower(), arc_world["usdc"].lower()]
    tick_until(cpp, "paid")
    receipts = cpp.snapshot()["receipts"]
    assert len(receipts) == 1
    (key,) = receipts
    assert key == f"arc:{tx.lower()}:{int(logs[0]['logIndex'], 16)}"


def test_arc_restore_finds_native_send(arc_world, cpp_arc):
    cpp = cpp_arc
    tx = native_send(arc_world, 4_990_000)           # round 4.99: the player must choose
    (c,) = cpp.restore_lookup(tx)
    assert (c["chain"], c["token"], c["transfer"]["value"], c["kind"]) == ("arc", "USDC", 4_990_000, None)
    added = cpp.restore_apply([c], {0: "chapter2"})
    assert len(added) == 1
    assert cpp.snapshot()["owned"] == ["chapter2"]


def test_reverify_keeps_arc_receipts(arc_world, cpp_arc):
    cpp = cpp_arc
    for send_payment in (native_send, pay):
        rec = cpp.start_session("unlock", "arc", item="chapter2")
        send_payment(arc_world, rec["units"])
        tick_until(cpp, "paid")
    real = cpp.snapshot()["receipts"]
    assert len(real) == 2
    fake_key = "arc:0x" + "ee" * 32 + ":0"
    cpp.set_receipt(fake_key, dict(next(iter(real.values())), tx_hash="0x" + "ee" * 32))
    assert cpp.reverify() == [fake_key]
    assert cpp.snapshot()["receipts"] == real


# --- test_solana_local.py ------------------------------------------------------------------------

def test_game_built_transaction_pays_and_finalizes(solana_world, cpp_solana):
    w, cpp = solana_world, cpp_solana
    rec = cpp.start_session("unlock", "solana", item="chapter2")
    built = cpp.build_solana_payment(rec["id"], str(w["player"].pubkey()))
    assert int(built["token_balance"]) == 100_000_000
    w["cluster"].sign_and_send(base64.b64decode(built["transaction"]), w["player"])
    tick_until_status(cpp, "confirming")
    out = tick_until_status(cpp, "paid")
    assert out["result"]["payer"] == str(w["player"].pubkey())
    assert cpp.snapshot()["owned"] == ["chapter2"]


def test_exchange_style_exact_transfer_is_found_by_scan(solana_world, cpp_solana):
    w, cpp = solana_world, cpp_solana
    rec = cpp.start_session("unlock", "solana", item="chapter2")
    w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"])
    out = tick_until_status(cpp, "paid")
    assert Decimal(out["result"]["amount"]) * 10**6 == rec["units"]


def test_hinted_round_amount(solana_world, cpp_solana):
    w, cpp = solana_world, cpp_solana
    rec = cpp.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), 5_000_000)
    cpp.report_hint(rec["id"], sig)
    assert tick_until_status(cpp, "paid")["result"]["tx_hash"] == sig


def test_reverify_and_restore(solana_world, cpp_solana):
    w, cpp = solana_world, cpp_solana
    rec = cpp.start_session("unlock", "solana", item="chapter2")
    sig = w["cluster"].transfer(w["player"], w["mint"], str(w["dev"].pubkey()), rec["units"],
                                reference=rec["reference"])
    tick_until_status(cpp, "paid")
    receipts = cpp.snapshot()["receipts"]
    real_key = next(iter(receipts))
    fake_sig = b58encode(bytes(range(64)))
    fake_key = f"solana:{fake_sig}:2"
    cpp.set_receipt(fake_key, dict(receipts[real_key], tx_hash=fake_sig))
    assert cpp.reverify() == [fake_key] and list(cpp.snapshot()["receipts"]) == [real_key]
    cpp.clear_receipts()
    [candidate] = cpp.restore_lookup(sig)
    assert (candidate["chain"], candidate["item"]) == ("solana", "chapter2")


def test_missing_token_account_marks_usdc_unready(solana_world, cpp_solana):
    from solders.keypair import Keypair
    stranger = CppEngine(solana_override(solana_world),
                        {"solana_wallet": str(Keypair().pubkey()), "testnet": True, "chains": ["solana"]},
                        [CHAPTER2])
    try:
        stranger.reverify()
        assert stranger.snapshot()["unready"] == [["solana", "USDC"]]
    finally:
        stranger.close()
    cpp_solana.reverify()                         # the developer's token account exists
    assert cpp_solana.snapshot()["unready"] == []
