from decimal import Decimal

from scfg_core.chains import MAINNET
from scfg_core.models import Item
from scfg_core.receipts import receipt_key
from scfg_core.sessions import create_session
from scfg_core.watcher import advance

from fakes import FakeChain, drive, transfer_log

CHAIN = MAINNET["arbitrum"]          # 20 confirmations
USDC = CHAIN.tokens["USDC"].address
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
ITEMS = {"chapter2": Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))}
TX = "0x" + "ab" * 32
UNITS = 4_993_742


def matched(head=1000, block=998):
    pending, receipts, chain = {}, {}, FakeChain(head=head)
    rec = create_session(pending, kind="unlock", item="chapter2", title="Chapter 2",
                         token="USDC", chain="arbitrum", price="4.99", code=37, nonce=42,
                         start_block=995, now=0.0)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, UNITS, block=block, tx_hash=TX, log_index=2))
    out = step(pending, receipts, chain, rec)
    assert out.status == "confirming"
    return pending, receipts, chain, rec


def step(pending, receipts, chain, rec, now=10.0):
    return drive(advance(pending, receipts, rec["id"], rpc=chain, chain=CHAIN, wallet=DEV,
                         items=ITEMS, now=now))


def test_counts_confirmations_until_required():
    pending, receipts, chain, rec = matched()
    chain.head = 1010
    out = step(pending, receipts, chain, rec)
    assert (out.status, out.confirmations, out.required) == ("confirming", 13, 20)


def test_paid_writes_receipt_and_removes_session():
    pending, receipts, chain, rec = matched()
    chain.head = 998 + 19                       # exactly 20 confirmations
    out = step(pending, receipts, chain, rec)
    assert out.status == "paid"
    r = out.result
    assert (r.status, r.kind, r.item, r.token, r.chain) == ("paid", "unlock", "chapter2", "USDC", "arbitrum")
    assert r.amount == Decimal("4.993742") and r.tx_hash == TX and r.payer == PAYER
    assert r.session_id == rec["id"]
    assert pending == {}
    stored = receipts[receipt_key("arbitrum", TX, 2)]
    assert stored["source"] == "payment" and stored["block"] == 998 and stored["amount"] == "4.993742"


def test_confirmations_override_is_respected():
    from dataclasses import replace
    pending, receipts, chain, rec = matched()
    fast = replace(CHAIN, confirmations=3)
    out = drive(advance(pending, receipts, rec["id"], rpc=chain, chain=fast, wallet=DEV,
                        items=ITEMS, now=1))
    assert out.status == "paid"


def test_reorg_returns_to_awaiting_and_rewinds_cursor():
    pending, receipts, chain, rec = matched()
    chain.hashes[998] = "0x" + "ee" * 32       # block 998 replaced
    chain.logs.clear()                         # transfer no longer in canonical chain
    chain.head = 1005
    out = step(pending, receipts, chain, rec)
    assert out.status == "reorged"
    assert rec["state"] == "awaiting" and rec["match"] is None
    assert rec["cursor"] == 998


def test_reorg_then_transfer_reappears_in_new_block():
    pending, receipts, chain, rec = matched()
    chain.hashes[998] = "0x" + "ee" * 32
    chain.logs = [transfer_log(USDC, PAYER, DEV, UNITS, block=1001, tx_hash=TX, log_index=0)]
    chain.head = 1002
    assert step(pending, receipts, chain, rec).status == "reorged"
    assert step(pending, receipts, chain, rec).status == "confirming"
    assert rec["match"]["block"] == 1001


def test_lagging_endpoint_below_match_block_is_not_a_reorg():
    pending, receipts, chain, rec = matched()
    chain.head = 990                           # another endpoint, behind
    out = step(pending, receipts, chain, rec)
    assert out.status == "confirming" and out.confirmations == 0
    assert rec["state"] == "confirming"


def test_outage_while_confirming_keeps_match():
    pending, receipts, chain, rec = matched()
    chain.down = True
    out = step(pending, receipts, chain, rec)
    assert out.error == "network_unavailable"
    assert rec["state"] == "confirming" and rec["match"] is not None


def test_cancelled_or_expired_ui_status_does_not_stop_confirmation():
    pending, receipts, chain, rec = matched()
    rec["ui_status"] = "cancelled"
    chain.head = 1100
    assert step(pending, receipts, chain, rec, now=4000.0).status == "paid"
