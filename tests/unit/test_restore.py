from decimal import Decimal

import pytest

from scfg_core.chains import MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.receipts import receipt_key
from scfg_core.restore import TIP_CHOICE, RestoreError, apply, classify, lookup

from fakes import FakeChain, drive, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
PAYER = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
ARB_USDC = MAINNET["arbitrum"].tokens["USDC"].address
BASE_USDC = MAINNET["base"].tokens["USDC"].address
TX = "0x" + "ab" * 32


def cfg():
    c = build_config(DEV, previous_wallets=[OLD])
    add_item(c, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    add_item(c, "ost", code=2, price="2.00", token="USDT", title="OST")
    return c


# --- classify -------------------------------------------------------------------

def test_classify_item_code():
    assert classify(cfg(), 4_993_742) == ("unlock", "chapter2", ())


def test_classify_tip_code():
    assert classify(cfg(), 3_000_017) == ("tip", None, ())


def test_classify_round_amount_asks_player():
    kind, item, choices = classify(cfg(), 5_000_000)
    assert kind is None and item is None
    assert choices == ("chapter2", "ost", TIP_CHOICE)


def test_classify_unknown_code_offers_affordable_items():
    kind, _, choices = classify(cfg(), 2_509_942)   # 2.50 | 99 | 42, code 99 unknown
    assert kind is None and choices == ("ost", TIP_CHOICE)


def test_classify_known_code_but_below_min_price_asks():
    kind, _, choices = classify(cfg(), 1_003_742)   # code 37 but only ~1.00 paid
    assert kind is None and choices == (TIP_CHOICE,)


# --- lookup ---------------------------------------------------------------------

def make_chains(**receipts_by_chain):
    chains = {k: MAINNET[k] for k in ("arbitrum", "base")}
    fakes = {k: FakeChain() for k in chains}
    for k, r in receipts_by_chain.items():
        fakes[k].receipts[TX] = r
    return chains, fakes


def run_lookup(chains, fakes, receipts=None, tx=TX):
    return drive(lookup(tx, config=cfg(), chains=chains, rpc_for=lambda k: fakes[k],
                        receipts=receipts or {}))


def test_lookup_finds_chain_and_classifies():
    chains, fakes = make_chains(base={"status": "0x1", "logs": [
        transfer_log(BASE_USDC, PAYER, DEV, 4_993_742, block=50, tx_hash=TX, log_index=1)]})
    [c] = run_lookup(chains, fakes)
    assert (c.chain, c.token, c.kind, c.item, c.already_restored) == ("base", "USDC", "unlock", "chapter2", False)
    assert fakes["arbitrum"].calls == ["get_transaction_receipt"]   # tried first, not found


def test_lookup_accepts_previous_wallet():
    chains, fakes = make_chains(arbitrum={"status": "0x1", "logs": [
        transfer_log(ARB_USDC, PAYER, OLD, 3_000_005, block=50, tx_hash=TX)]})
    assert run_lookup(chains, fakes)[0].kind == "tip"


def test_lookup_marks_already_restored():
    chains, fakes = make_chains(arbitrum={"status": "0x1", "logs": [
        transfer_log(ARB_USDC, PAYER, DEV, 4_993_742, block=50, tx_hash=TX, log_index=3)]})
    receipts = {receipt_key("arbitrum", TX, 3): {}}
    assert run_lookup(chains, fakes, receipts)[0].already_restored is True


@pytest.mark.parametrize("receipt, code", [
    ({"status": "0x0", "logs": []}, "tx_reverted"),
    ({"status": "0x1", "logs": [transfer_log(ARB_USDC, PAYER, PAYER, 5_000_000, block=1, tx_hash=TX)]},
     "tx_not_a_payment"),
    ({"status": "0x1", "logs": []}, "tx_not_a_payment"),
])
def test_lookup_errors(receipt, code):
    chains, fakes = make_chains(arbitrum=receipt)
    with pytest.raises(RestoreError) as info:
        run_lookup(chains, fakes)
    assert info.value.code == code


def test_lookup_not_found_anywhere():
    chains, fakes = make_chains()
    with pytest.raises(RestoreError) as info:
        run_lookup(chains, fakes)
    assert info.value.code == "tx_not_found"


def test_lookup_network_trouble_is_reported_when_not_found():
    chains, fakes = make_chains()
    fakes["base"].down = True
    with pytest.raises(RestoreError) as info:
        run_lookup(chains, fakes)
    assert info.value.code == "network_unavailable"


def test_lookup_invalid_hash():
    chains, fakes = make_chains()
    with pytest.raises(RestoreError) as info:
        run_lookup(chains, fakes, tx="0x123")
    assert info.value.code == "invalid_hash"


# --- apply ----------------------------------------------------------------------

def test_apply_auto_and_chosen_candidates():
    chains, fakes = make_chains(arbitrum={"status": "0x1", "logs": [
        transfer_log(ARB_USDC, PAYER, DEV, 4_993_742, block=50, tx_hash=TX, log_index=0),
        transfer_log(ARB_USDC, PAYER, DEV, 5_000_000, block=50, tx_hash=TX, log_index=1),
    ]})
    cands = run_lookup(chains, fakes)
    store = {}
    added = apply(store, cands, {1: "ost"})
    assert [(r["kind"], r["item"], r["source"]) for r in added] == [
        ("unlock", "chapter2", "restore"), ("unlock", "ost", "restore")]
    assert apply(store, cands, {1: "ost"}) == []          # idempotent
    assert added[1]["amount"] == str(Decimal("5.000000"))


def test_apply_skips_unchosen_and_rejects_invalid_choice():
    chains, fakes = make_chains(arbitrum={"status": "0x1", "logs": [
        transfer_log(ARB_USDC, PAYER, DEV, 1_000_000, block=50, tx_hash=TX)]})
    cands = run_lookup(chains, fakes)
    assert apply({}, cands) == []
    with pytest.raises(ValueError):
        apply({}, cands, {0: "chapter2"})                  # 1.00 can't buy chapter2
    assert apply({}, cands, {0: TIP_CHOICE})[0]["kind"] == "tip"
