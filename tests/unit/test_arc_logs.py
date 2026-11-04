"""Arc: USDC is the native gas coin, so its transfers are read from the system emitter.

A plain native send logs a Transfer only at the system emitter (18 decimals). An ERC-20
transfer() on the 0x3600 interface logs there too, plus its own 6-decimal Transfer.
Chain.parse_transfer keeps exactly one of those per payment: the emitter's, floored to
6-decimal units and stamped with the canonical USDC address.
"""

from decimal import Decimal

from scfg_core.chains import ARC_SYSTEM_EMITTER, ARC_USDC, MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.evm import TransferLog
from scfg_core.models import Item
from scfg_core.receipts import add_receipt, make_receipt, receipt_key
from scfg_core.restore import lookup_evm
from scfg_core.reverify import check_evm_receipt
from scfg_core.sessions import create_session, set_hint
from scfg_core.watcher import advance

from fakes import FakeChain, drive, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
TX = "0x" + "ab" * 32
ARC = MAINNET["arc"]
NATIVE = 10**12  # native (18-decimal) wei per 6-decimal unit


def emitter_log(value, **kw):
    kw.setdefault("block", 100)
    kw.setdefault("tx_hash", TX)
    return transfer_log(ARC_SYSTEM_EMITTER, PAYER, DEV, value, **kw)


def test_parse_transfer_floors_native_value():
    t = ARC.parse_transfer(emitter_log(1_230_017_900_000_000_000, log_index=4))
    assert t.value == 1_230_017
    assert t.token == ARC_USDC
    assert (t.sender, t.recipient, t.log_index) == (PAYER, DEV, 4)


def test_parse_transfer_ignores_sub_micro_value():
    # Less than one 6-decimal unit floors to nothing: never a payment.
    assert ARC.parse_transfer(emitter_log(NATIVE - 1)) is None


def test_parse_transfer_ignores_interface_log_on_arc():
    # The 0x3600 log of an ERC-20 transfer() duplicates the emitter's: never counted.
    log = transfer_log(ARC_USDC, PAYER, DEV, 1_230_017, block=100, tx_hash=TX)
    assert ARC.parse_transfer(log) is None


def test_parse_transfer_ignores_unknown_contract():
    other = "0x1111111111111111111111111111111111111111"
    log = transfer_log(other, PAYER, DEV, 5, block=100, tx_hash=TX)
    assert ARC.parse_transfer(log) is None
    assert MAINNET["base"].parse_transfer(log) is None


def test_parse_transfer_plain_chain_unchanged():
    usdc = MAINNET["base"].tokens["USDC"].address
    t = MAINNET["base"].parse_transfer(
        transfer_log(usdc, PAYER, DEV, 4_993_742, block=7, tx_hash=TX, log_index=2))
    assert (t.token, t.value, t.log_index, t.block_number) == (usdc, 4_993_742, 2, 7)


def test_parse_transfer_skips_non_transfer_logs():
    log = emitter_log(5 * NATIVE)
    log["topics"] = log["topics"][:2]
    assert ARC.parse_transfer(log) is None


# --- the watcher, restore and re-verification on Arc ---------------------------------

ITEMS = {"chapter2": Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))}
UNITS = 4_993_742  # 4.99 + code 37 + nonce 42


def arc_transfer_tx(units, *, block=998, tx_hash=TX, log_index=5, native=None):
    """The two logs one ERC-20 transfer() on Arc produces: emitter first, then 0x3600."""
    return [
        emitter_log(native if native is not None else units * NATIVE, block=block,
                    tx_hash=tx_hash, log_index=log_index),
        transfer_log(ARC_USDC, PAYER, DEV, units, block=block, tx_hash=tx_hash,
                     log_index=log_index + 1),
    ]


def session(head=1000):
    pending, receipts, rpc = {}, {}, FakeChain(head=head)
    rec = create_session(pending, kind="unlock", item="chapter2", title="Chapter 2", token="USDC",
                         chain="arc", price="4.99", code=37, nonce=42, start_block=995, now=0.0)
    assert rec["units"] == UNITS
    return pending, receipts, rpc, rec


def step(pending, receipts, rpc, rec, now=10.0):
    return drive(advance(pending, receipts, rec["id"], rpc=rpc, chain=ARC, wallet=DEV,
                         items=ITEMS, now=now))


def test_arc_native_send_detected_by_scan():
    pending, receipts, rpc, rec = session()
    rpc.logs.append(emitter_log(UNITS * NATIVE, block=998, log_index=3))   # wallet "Send"
    out = step(pending, receipts, rpc, rec)
    assert rpc.last_get_logs[0] == ARC_SYSTEM_EMITTER
    assert out.status == "paid"      # one confirmation is final on Arc
    assert out.result.amount == Decimal("4.993742")
    assert list(receipts) == [receipt_key("arc", TX, 3)]


def test_arc_native_send_with_dust_matches_session():
    pending, receipts, rpc, rec = session()
    rpc.logs.append(emitter_log(UNITS * NATIVE + NATIVE - 1, block=998))
    assert step(pending, receipts, rpc, rec).status == "paid"


def test_arc_transfer_counts_once_in_scan():
    pending, receipts, rpc, rec = session()
    rpc.logs.extend(arc_transfer_tx(UNITS))
    assert step(pending, receipts, rpc, rec).status == "paid"
    assert list(receipts) == [receipt_key("arc", TX, 5)]


def test_arc_interface_log_alone_is_not_seen_by_scan():
    pending, receipts, rpc, rec = session()
    rpc.logs.append(arc_transfer_tx(UNITS)[1])
    assert step(pending, receipts, rpc, rec).status == "waiting"


def hinted(logs):
    pending, receipts, rpc, rec = session()
    rpc.receipts[TX] = {"status": "0x1", "logs": logs}
    set_hint(pending, rec["id"], TX, now=1.0)
    return pending, receipts, rpc, rec


def test_arc_hint_verifies_from_emitter_log():
    pending, receipts, rpc, rec = hinted(arc_transfer_tx(UNITS + 1000))  # exchange-style amount
    out = step(pending, receipts, rpc, rec)
    assert out.status == "paid"
    assert out.result.amount == Decimal("4.994742")
    assert list(receipts) == [receipt_key("arc", TX, 5)]


def test_arc_hint_underpaid_uses_floored_value():
    pending, receipts, rpc, rec = hinted(
        arc_transfer_tx(0, native=4_989_999 * NATIVE + NATIVE - 1))
    out = step(pending, receipts, rpc, rec)
    assert out.status == "underpaid"
    assert out.result.amount == Decimal("4.989999")


def test_arc_hint_with_only_interface_log_is_not_a_payment():
    pending, receipts, rpc, rec = hinted(arc_transfer_tx(UNITS)[1:])
    out = step(pending, receipts, rpc, rec)
    assert out.error == "tx_not_a_payment"


def test_arc_restore_returns_one_candidate():
    config = build_config(DEV, chains=["arc"])
    add_item(config, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    rpc = FakeChain()
    rpc.receipts[TX] = {"status": "0x1", "logs": arc_transfer_tx(UNITS)}
    out = drive(lookup_evm(TX, config=config, chain=ARC, rpc=rpc, receipts={}))
    assert len(out) == 1
    c = out[0]
    assert (c.token, c.transfer.value, c.transfer.log_index, c.kind, c.item) == (
        "USDC", UNITS, 5, "unlock", "chapter2")


def arc_receipt(value=UNITS, log_index=5, block=998):
    store = {}
    t = TransferLog(ARC_USDC, PAYER, DEV, value, TX, log_index, block, "0x" + "11" * 32)
    add_receipt(store, make_receipt(chain="arc", transfer=t, kind="unlock", item="chapter2",
                                    token="USDC", source="payment"))
    return next(iter(store.values()))


def test_reverify_keeps_arc_receipt():
    rpc = FakeChain(head=1000)
    rpc.logs.extend(arc_transfer_tx(UNITS, native=UNITS * NATIVE + 123))
    assert drive(check_evm_receipt(arc_receipt(), rpc=rpc, chain=ARC, wallets=(DEV,))) == "keep"
    assert rpc.last_get_logs[0] == ARC_SYSTEM_EMITTER


def test_reverify_drops_arc_receipt_with_wrong_amount():
    rpc = FakeChain(head=1000)
    rpc.logs.extend(arc_transfer_tx(UNITS))
    r = arc_receipt(value=UNITS + 1)
    assert drive(check_evm_receipt(r, rpc=rpc, chain=ARC, wallets=(DEV,))) == "drop"


def test_reverify_keeps_legacy_custom_arc_receipt():
    # A game that added Arc itself with add_chain("arc", ...) before it was built in stored
    # receipts at the 0x3600 log (index emitter + 1). They must survive the update.
    rpc = FakeChain(head=1000)
    rpc.logs.extend(arc_transfer_tx(UNITS))
    r = arc_receipt(log_index=6)
    assert drive(check_evm_receipt(r, rpc=rpc, chain=ARC, wallets=(DEV,))) == "keep"


def test_reverify_drops_legacy_arc_receipt_with_wrong_amount():
    rpc = FakeChain(head=1000)
    rpc.logs.extend(arc_transfer_tx(UNITS))
    r = arc_receipt(value=UNITS + 1, log_index=6)
    assert drive(check_evm_receipt(r, rpc=rpc, chain=ARC, wallets=(DEV,))) == "drop"


def test_reverify_drops_arc_receipt_with_no_log_at_all():
    rpc = FakeChain(head=1000)
    assert drive(check_evm_receipt(arc_receipt(), rpc=rpc, chain=ARC, wallets=(DEV,))) == "drop"
