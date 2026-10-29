"""The payment watcher: advances one pending session per call.

    awaiting --(hinted tx verified | exact amount found by scan)--> confirming
    confirming --(N confirmations, block hash unchanged)--> paid (receipt stored)
    confirming --(block hash changed: reorg)--> awaiting
    awaiting --(hinted tx pays less than min_price)--> underpaid
    awaiting --(24 h, nothing found)--> closed

``advance`` is pure logic over plain dicts plus an RPC object, so it is fully
testable with a fake chain. State is only mutated after RPC calls succeed, so a
network outage never corrupts a session. advance() is cooperative (see coio): call it
with yield from.
"""

from __future__ import annotations

from collections.abc import MutableMapping
from dataclasses import dataclass
from decimal import Decimal

from .amounts import units_to_decimal
from .chains import Chain
from .evm import TRANSFER_TOPIC, TransferLog, address_topic, parse_transfer_log
from .models import Item, PaymentResult
from .receipts import add_receipt, make_receipt, receipt_key
from .rpc import RpcError, RpcUnavailable
from .sessions import HINT_TIMEOUT_S, clear_hint, is_past_horizon, remove_session

SCAN_STEP_BLOCKS = 10_000  # upper bound on blocks scanned per advance()
MAX_SCAN_CALLS = 5  # eth_getLogs calls per advance(), to be gentle with public RPCs


@dataclass
class StepOutcome:
    session_id: str
    status: str  # missing | waiting | confirming | reorged | paid | underpaid | closed | expired
    confirmations: int = 0
    required: int = 0
    result: PaymentResult | None = None
    # network_unavailable | tx_reverted | tx_not_a_payment | tx_not_landed (Solana)
    # | internal_error (Engine.tick: the session's step crashed)
    error: str | None = None
    # The hint a tx_reverted / tx_not_a_payment / tx_not_landed verdict is about (Solana),
    # so the screen can tell it from a newer hint reported meanwhile.
    hint_tx: str | None = None


def min_amount(rec: dict, items: dict[str, Item]) -> Decimal:
    if rec["kind"] == "unlock":
        item = items.get(rec["item"])
        return item.min_price if item else Decimal(rec["price"])
    return Decimal("0.000001")


def _match_dict(t: TransferLog) -> dict:
    return {"tx_hash": t.tx_hash, "log_index": t.log_index, "block": t.block_number,
            "block_hash": t.block_hash, "value": t.value, "payer": t.sender,
            "recipient": t.recipient}


def result_from_match(rec: dict, status: str, chain_key: str, m: dict) -> PaymentResult:
    return PaymentResult(status=status, kind=rec["kind"], item=rec["item"],
                         amount=units_to_decimal(m["value"]), token=rec["token"], chain=chain_key,
                         tx_hash=m["tx_hash"], payer=m["payer"], session_id=rec["id"])


def _qualifies(t: TransferLog | None, rec: dict, chain: Chain, wallet: str,
               receipts: MutableMapping) -> bool:
    return (t is not None
            and t.token == chain.tokens[rec["token"]].address
            and t.recipient == wallet
            and t.block_number >= rec["start_block"]
            and receipt_key(chain.key, t.tx_hash, t.log_index) not in receipts)


def _verify_hint(rec, rpc, chain, wallet, items, receipts, now, out) -> str | None:
    """Check the tx hash reported by the browser page or pasted by the player.

    Returns "matched", "underpaid", or None (keep waiting / hint discarded).
    """
    receipt = yield from rpc.get_transaction_receipt(rec["hint_tx"])
    if receipt is None:  # not mined yet (or unknown): give it up to 30 minutes
        if now - rec["hint_at"] >= HINT_TIMEOUT_S:
            clear_hint(rec)
        return None
    if receipt.get("status") == "0x0":
        out.hint_tx = rec["hint_tx"]  # the verdict is about this hint
        clear_hint(rec)
        out.error = "tx_reverted"
        return None
    candidates = [t for t in (parse_transfer_log(lg) for lg in receipt.get("logs") or [])
                  if _qualifies(t, rec, chain, wallet, receipts) and t.value > 0]
    if not candidates:
        out.hint_tx = rec["hint_tx"]  # the verdict is about this hint
        clear_hint(rec)
        out.error = "tx_not_a_payment"
        return None
    best = max(candidates, key=lambda t: t.value)
    rec["match"] = _match_dict(best)
    if units_to_decimal(best.value) < min_amount(rec, items):
        return "underpaid"
    return "matched"


def _scan(rec, rpc, chain, wallet, receipts, head: int) -> bool:
    """Look for a transfer of exactly the session's encoded amount since the cursor."""
    if rec["cursor"] > head:  # this endpoint is behind the one that set start_block
        return False
    span = getattr(rpc, "log_range", None)
    step = min(SCAN_STEP_BLOCKS, span * MAX_SCAN_CALLS) if span else SCAN_STEP_BLOCKS
    to = min(head, rec["cursor"] + step - 1)
    token = chain.tokens[rec["token"]].address
    logs = yield from rpc.get_logs(token, [TRANSFER_TOPIC, None, address_topic(wallet)],
                                   rec["cursor"], to)
    found = sorted(
        (t for t in (parse_transfer_log(lg) for lg in logs)
         if _qualifies(t, rec, chain, wallet, receipts) and t.value == rec["units"]),
        key=lambda t: (t.block_number, t.log_index))
    rec["cursor"] = to + 1
    if found:
        rec["match"] = _match_dict(found[0])
        return True
    return False


def advance(pending: MutableMapping, receipts: MutableMapping, sid: str, *, rpc, chain: Chain,
            wallet: str, items: dict[str, Item], now: float) -> StepOutcome:
    rec = pending.get(sid)
    out = StepOutcome(session_id=sid, status="missing", required=chain.confirmations)
    if rec is None:
        return out
    try:
        head: int | None = None
        if rec["state"] == "awaiting":
            out.status = "waiting"
            if is_past_horizon(rec, now):
                remove_session(pending, sid)
                out.status = "closed"
                return out
            outcome = None
            if rec["hint_tx"]:
                outcome = yield from _verify_hint(rec, rpc, chain, wallet, items, receipts, now,
                                                  out)
            if outcome is None and not rec["hint_tx"]:
                head = yield from rpc.block_number()
                found = yield from _scan(rec, rpc, chain, wallet, receipts, head)
                outcome = "matched" if found else None
            if outcome == "underpaid":
                remove_session(pending, sid)
                out.status = "underpaid"
                out.result = result_from_match(rec, "underpaid", chain.key, rec["match"])
                return out
            if outcome != "matched":
                return out
            rec["state"] = "confirming"

        out.status = "confirming"
        m = rec["match"]
        if head is None:
            head = yield from rpc.block_number()
        if head < m["block"]:  # lagging endpoint, not a reorg
            return out
        current = yield from rpc.get_block_hash(m["block"])
        if current is None or current.lower() != m["block_hash"]:
            rec["state"] = "awaiting"
            rec["match"] = None
            rec["cursor"] = min(rec["cursor"], m["block"])
            out.status = "reorged"
            return out
        out.confirmations = head - m["block"] + 1
        if out.confirmations < chain.confirmations:
            return out
        transfer = TransferLog(
            chain.tokens[rec["token"]].address, m["payer"], m["recipient"], m["value"],
            m["tx_hash"], m["log_index"], m["block"], m["block_hash"])
        add_receipt(receipts, make_receipt(chain=chain.key, transfer=transfer, kind=rec["kind"],
                                           item=rec["item"], token=rec["token"], source="payment"))
        remove_session(pending, sid)
        out.status = "paid"
        out.result = result_from_match(rec, "paid", chain.key, m)
        return out
    except (RpcUnavailable, RpcError):
        out.error = "network_unavailable"
        return out
