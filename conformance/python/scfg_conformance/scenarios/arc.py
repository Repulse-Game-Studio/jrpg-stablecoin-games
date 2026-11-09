"""Arc: USDC is the native gas coin, so its payments are read from the system emitter (18
decimals) and floored to the 6-decimal units the rest of the core uses."""

from __future__ import annotations

from scfg_core.chains import ARC_SYSTEM_EMITTER, ARC_USDC

from ..scenario import scenario
from .evm import covers
from .fixtures import CHAPTER2, DEV_EVM, PAYER_EVM, tx_hash

NATIVE = 10**12     # 18-decimal wei per 6-decimal unit
UNITS = 4_993_742   # 4.99 + code 37 + nonce 42, for restore and stored receipts


def _arc(s, **config):
    s.config(wallet=DEV_EVM, chains=["arc"], **config)
    s.item(**CHAPTER2)
    return s.evm("arc")


def _emitter(node, value, *, tx, block, log_index=0, sender=PAYER_EVM):
    """A native send: a Transfer at the system emitter with an 18-decimal value."""
    return node.add_transfer(ARC_USDC, sender, DEV_EVM, value, block=block, tx_hash=tx,
                             log_index=log_index, address=ARC_SYSTEM_EMITTER)


def _erc20_transfer(node, units, *, tx, block, log_index=0, native=None):
    """What one ERC-20 transfer() on Arc logs: the emitter's Transfer, then the interface's
    own 6-decimal Transfer at the 0x3600 contract."""
    _emitter(node, units * NATIVE if native is None else native, tx=tx, block=block,
             log_index=log_index)
    node.add_transfer(ARC_USDC, PAYER_EVM, DEV_EVM, units, block=block, tx_hash=tx,
                      log_index=log_index + 1)


def _outcome(s, rec):
    """The outcome of `rec`'s session in one tick."""
    [out] = [o for o in s.tick() if o.session_id == rec["id"]]
    return out


def _pending(s, rec):
    return s.steps[-1]["snapshot"]["pending"][rec["id"]]


@scenario("evm/arc_native_send", covers=(
    covers("arc_logs", "parse_transfer_floors_native_value", "arc_native_send_detected_by_scan",
           "arc_native_send_with_dust_matches_session")))
def _native_send(s):
    """A plain native send logs only at the system emitter. The scan reads that emitter, an
    18-decimal value floors to 6-decimal units (sub-unit dust is ignored), one confirmation is
    final, and the receipt carries the 6-decimal amount."""
    node = _arc(s)
    exact = s.start_session("unlock", "arc", item="chapter2")
    dusty = s.start_session("unlock", "arc", item="chapter2")
    assert exact["units"] != dusty["units"]
    _emitter(node, exact["units"] * NATIVE, tx=tx_hash(1), block=998, log_index=3)
    _emitter(node, dusty["units"] * NATIVE + NATIVE - 1, tx=tx_hash(2), block=999)
    outs = {o.session_id: o for o in s.tick()}
    assert {o.status for o in outs.values()} == {"paid"}, outs
    assert str(outs[exact["id"]].result.amount) == f"{exact['units'] / 10**6:.6f}"
    assert str(outs[dusty["id"]].result.amount) == f"{dusty['units'] / 10**6:.6f}"
    assert {c["params"][0]["address"].lower() for c in s.steps[-1]["calls"]
            if c["method"] == "eth_getLogs"} == {ARC_SYSTEM_EMITTER.lower()}
    receipts = s.steps[-1]["snapshot"]["receipts"]
    assert set(receipts) == {f"arc:{tx_hash(1)}:3", f"arc:{tx_hash(2)}:0"}
    assert receipts[f"arc:{tx_hash(1)}:3"]["amount"] == f"{exact['units'] / 10**6:.6f}"


@scenario("evm/arc_erc20_transfer_counts_once", covers=(
    covers("arc_logs", "parse_transfer_ignores_interface_log_on_arc",
           "arc_transfer_counts_once_in_scan", "arc_interface_log_alone_is_not_seen_by_scan")))
def _erc20_counts_once(s):
    """An ERC-20 transfer() logs twice (emitter, then the 0x3600 interface): the payment counts
    once, as the emitter's log, so the receipt is keyed by the emitter's log index. The
    interface's log on its own is never a payment."""
    node = _arc(s)
    paid = s.start_session("unlock", "arc", item="chapter2")
    _erc20_transfer(node, paid["units"], tx=tx_hash(1), block=998, log_index=5)
    [out] = s.tick()
    assert out.status == "paid" and out.result.tx_hash == tx_hash(1), out
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [f"arc:{tx_hash(1)}:5"]
    lonely = s.start_session("unlock", "arc", item="chapter2")
    node.mine(1)
    node.add_transfer(ARC_USDC, PAYER_EVM, DEV_EVM, lonely["units"], block=node.head,
                      tx_hash=tx_hash(2), log_index=1)
    [out] = s.tick()
    assert out.status == "waiting" and _pending(s, lonely)["match"] is None


@scenario("evm/arc_dust_floors_to_zero", covers=covers(
    "arc_logs", "parse_transfer_ignores_sub_micro_value"))
def _dust_floors_to_zero(s):
    """A native send below one 6-decimal unit floors to nothing: it never matches in the scan
    and a hint naming it is not a payment."""
    node = _arc(s)
    rec = s.start_session("unlock", "arc", item="chapter2")
    _emitter(node, NATIVE - 1, tx=tx_hash(1), block=998)
    [out] = s.tick()
    assert out.status == "waiting"
    s.report_hint(rec["id"], tx_hash(1))
    [out] = s.tick()
    assert out.error == "tx_not_a_payment" and out.hint_tx == tx_hash(1), out


@scenario("evm/arc_ignored_logs", covers=(
    covers("arc_logs", "parse_transfer_ignores_unknown_contract",
           "parse_transfer_skips_non_transfer_logs")))
def _ignored_logs(s):
    """Logs of an unknown contract (on Arc and on a plain chain) and a log at the emitter that
    is not a three-topic Transfer are not payments."""
    s.config(wallet=DEV_EVM, chains=["arc", "base"])
    s.item(**CHAPTER2)
    arc, base = s.evm("arc"), s.evm("base")
    other = "0x1111111111111111111111111111111111111111"
    for node, chain, n in ((arc, "arc", 1), (base, "base", 2)):
        rec = s.start_session("unlock", chain, item="chapter2")
        node.add_transfer(other, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(n))
        s.report_hint(rec["id"], tx_hash(n))
        out = _outcome(s, rec)
        assert out.error == "tx_not_a_payment", out
    rec = s.start_session("unlock", "arc", item="chapter2")
    log = _emitter(arc, rec["units"] * NATIVE, tx=tx_hash(3), block=998)
    log["topics"] = log["topics"][:2]
    s.report_hint(rec["id"], tx_hash(3))
    out = _outcome(s, rec)
    assert out.error == "tx_not_a_payment", out


@scenario("evm/arc_hint", covers=(
    covers("arc_logs", "arc_hint_verifies_from_emitter_log",
           "arc_hint_underpaid_uses_floored_value",
           "arc_hint_with_only_interface_log_is_not_a_payment")))
def _arc_hint(s):
    """A hinted Arc transaction is judged by the emitter's log: an exchange-style amount above
    the price is paid, a value just under the minimum floors below it and is underpaid, and a
    transaction with only the interface's log is not a payment."""
    node = _arc(s)
    rec = s.start_session("unlock", "arc", item="chapter2")
    _erc20_transfer(node, 4_994_742, tx=tx_hash(1), block=998, log_index=5)
    s.report_hint(rec["id"], tx_hash(1))
    [out] = s.tick()
    assert out.status == "paid" and str(out.result.amount) == "4.994742", out
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [f"arc:{tx_hash(1)}:5"]
    rec = s.start_session("unlock", "arc", item="chapter2")
    _erc20_transfer(node, 0, tx=tx_hash(2), block=998, log_index=5,
                    native=4_989_999 * NATIVE + NATIVE - 1)
    s.report_hint(rec["id"], tx_hash(2))
    [out] = s.tick()
    assert out.status == "underpaid" and str(out.result.amount) == "4.989999", out
    rec = s.start_session("unlock", "arc", item="chapter2")
    node.add_transfer(ARC_USDC, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(3),
                      log_index=6)
    s.report_hint(rec["id"], tx_hash(3))
    [out] = s.tick()
    assert out.error == "tx_not_a_payment", out


@scenario("evm/arc_restore_and_reverify", covers=(
    covers("arc_logs", "arc_restore_returns_one_candidate", "reverify_keeps_arc_receipt",
           "reverify_drops_arc_receipt_with_wrong_amount",
           "reverify_keeps_legacy_custom_arc_receipt",
           "reverify_drops_legacy_arc_receipt_with_wrong_amount",
           "reverify_drops_arc_receipt_with_no_log_at_all")))
def _arc_restore_and_reverify(s):
    """Restore finds one candidate in a two-log transfer. Re-verification keeps a receipt whose
    emitter log matches (even with dust), keeps a legacy receipt stored at the interface's log
    (index emitter + 1) when the amount matches, and drops receipts whose amount is wrong or
    that have no log at all."""
    node = _arc(s)

    def receipt(n, units, log_index):
        return {"chain": "arc", "tx_hash": tx_hash(n), "log_index": log_index, "block": 900,
                "kind": "unlock", "item": "chapter2", "amount": f"{units / 10**6:.6f}",
                "token": "USDC", "payer": PAYER_EVM, "recipient": DEV_EVM, "source": "payment"}

    stored = [receipt(1, UNITS, 5), receipt(2, UNITS + 1, 5), receipt(3, UNITS, 6),
              receipt(4, UNITS + 1, 6), receipt(5, UNITS, 5)]
    s.initial(receipts={f"arc:{r['tx_hash']}:{r['log_index']}": r for r in stored})
    _erc20_transfer(node, UNITS, tx=tx_hash(1), block=900, log_index=5, native=UNITS * NATIVE + 123)
    for n in (2, 3, 4):
        _erc20_transfer(node, UNITS, tx=tx_hash(n), block=900, log_index=5)
    removed = s.reverify()
    assert removed == sorted(f"arc:{tx_hash(n)}:{i}" for n, i in ((2, 5), (4, 6), (5, 5)))
    assert set(s.steps[-1]["snapshot"]["receipts"]) == {
        f"arc:{tx_hash(1)}:5", f"arc:{tx_hash(3)}:6"}
    _erc20_transfer(node, UNITS, tx=tx_hash(6), block=998, log_index=5)
    found = s.restore_lookup(tx_hash(6))
    assert [(c.chain, c.token, c.transfer.value, c.transfer.log_index, c.kind, c.item)
            for c in found] == [("arc", "USDC", UNITS, 5, "unlock", "chapter2")]


# D13: GDScript's int is signed 64-bit, so an 18-decimal emitter value overflows from 9.22 USDC.
BIG_ITEM = {"item_id": "chapter9", "code": 37, "price": "19.99", "token": "USDC",
            "title": "Chapter 9"}
BIG_UNITS = 19_993_742   # 19.99 + code 37 + nonce 42


def _big_arc(s):
    s.config(wallet=DEV_EVM, chains=["arc"])
    s.item(**BIG_ITEM)
    return s.evm("arc")


def _assert_big(node):
    """The recorded emitter data must not fit a signed 64-bit integer."""
    top = max(int(lg["data"], 16) for lg in node.logs)
    assert top > 2**63, top


@scenario("evm/arc_large_scan", covers=covers(
    "arc_logs", "arc_native_send_detected_by_scan", "arc_native_send_with_dust_matches_session"))
def _large_scan(s):
    """The scan reads native sends above 2**63 in 18-decimal wei (a 19.99 USDC item): exact and
    with sub-unit dust, both pay, and the receipts carry the floored 6-decimal amount."""
    node = _big_arc(s)
    exact = s.start_session("unlock", "arc", item="chapter9")
    dusty = s.start_session("unlock", "arc", item="chapter9")
    _emitter(node, exact["units"] * NATIVE, tx=tx_hash(1), block=998, log_index=3)
    _emitter(node, dusty["units"] * NATIVE + NATIVE - 1, tx=tx_hash(2), block=999)
    _assert_big(node)
    assert exact["units"] > 2**63 // NATIVE and dusty["units"] > 2**63 // NATIVE
    outs = {o.session_id: o for o in s.tick()}
    assert {o.status for o in outs.values()} == {"paid"}, outs
    receipts = s.steps[-1]["snapshot"]["receipts"]
    assert set(receipts) == {f"arc:{tx_hash(1)}:3", f"arc:{tx_hash(2)}:0"}
    assert receipts[f"arc:{tx_hash(1)}:3"]["amount"] == f"{exact['units'] / 10**6:.6f}"
    assert receipts[f"arc:{tx_hash(2)}:0"]["amount"] == f"{dusty['units'] / 10**6:.6f}"


@scenario("evm/arc_large_hint", covers=covers(
    "arc_logs", "arc_hint_verifies_from_emitter_log", "arc_hint_underpaid_uses_floored_value"))
def _large_hint(s):
    """Hinted Arc transactions above 2**63 in 18-decimal wei: a transfer() that pays is paid,
    and one whose dusty value floors a unit under the price is underpaid."""
    node = _big_arc(s)
    rec = s.start_session("unlock", "arc", item="chapter9")
    _erc20_transfer(node, rec["units"] + 1000, tx=tx_hash(1), block=998, log_index=5)
    s.report_hint(rec["id"], tx_hash(1))
    [out] = s.tick()
    assert out.status == "paid", out
    assert str(out.result.amount) == f"{(rec['units'] + 1000) / 10**6:.6f}", out
    rec = s.start_session("unlock", "arc", item="chapter9")
    _erc20_transfer(node, 0, tx=tx_hash(2), block=998, log_index=5,
                    native=(19_989_999 * NATIVE) + NATIVE - 1)
    _assert_big(node)
    s.report_hint(rec["id"], tx_hash(2))
    [out] = s.tick()
    assert out.status == "underpaid" and str(out.result.amount) == "19.989999", out


@scenario("evm/arc_large_restore", covers=covers("arc_logs", "arc_restore_returns_one_candidate"))
def _large_restore(s):
    """Restore finds one candidate in a two-log transfer whose emitter value is above 2**63,
    applying it stores a receipt, and re-verification keeps that receipt."""
    node = _big_arc(s)
    _erc20_transfer(node, BIG_UNITS, tx=tx_hash(1), block=900, log_index=5)
    _assert_big(node)
    found = s.restore_lookup(tx_hash(1))
    assert [(c.chain, c.token, c.transfer.value, c.transfer.log_index, c.kind, c.item)
            for c in found] == [("arc", "USDC", BIG_UNITS, 5, "unlock", "chapter9")]
    s.restore_apply(len(s.steps) - 1, {})
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [f"arc:{tx_hash(1)}:5"]
    assert s.reverify() == []
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [f"arc:{tx_hash(1)}:5"]
