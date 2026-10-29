"""Receipts: verified on-chain transfers to the developer, stored in ``persistent``.

There is deliberately no "owned" flag anywhere. Ownership is always recomputed from
receipts, and receipts are re-checked against the chain at every launch.
"""

from __future__ import annotations

from collections.abc import MutableMapping
from decimal import Decimal, InvalidOperation

from .amounts import units_to_decimal
from .evm import TransferLog
from .models import Item
from .transfers import Transfer


def receipt_key(chain: str, tx_hash: str, log_index: int) -> str:
    # EVM hashes are hex (case-insensitive); Solana signatures are base58 (case matters).
    tx = tx_hash.lower() if tx_hash.startswith("0x") else tx_hash
    return f"{chain}:{tx}:{log_index}"


def make_receipt(*, chain: str, transfer: Transfer | TransferLog, kind: str, item: str | None,
                 token: str, source: str) -> dict:
    if kind not in ("unlock", "tip"):
        raise ValueError(f"invalid kind {kind!r}")
    if source not in ("payment", "restore"):
        raise ValueError(f"invalid source {source!r}")
    return {
        "chain": chain, "tx_hash": transfer.tx_hash, "log_index": transfer.log_index,
        "block": transfer.block_number, "kind": kind, "item": item,
        "amount": str(units_to_decimal(transfer.value)), "token": token,
        "payer": transfer.sender, "recipient": transfer.recipient, "source": source,
    }


def add_receipt(store: MutableMapping, receipt: dict) -> bool:
    key = receipt_key(receipt["chain"], receipt["tx_hash"], receipt["log_index"])
    if key in store:
        return False
    store[key] = receipt
    return True


def remove_receipt(store: MutableMapping, key: str) -> None:
    store.pop(key, None)


def compute_ownership(store: MutableMapping, items: dict[str, Item]) -> tuple[set[str], Decimal]:
    """(owned item ids, total tipped). Malformed entries are skipped, never fatal."""
    owned: set[str] = set()
    tips = Decimal(0)
    for r in store.values():
        try:
            kind = r["kind"]
            amount = Decimal(r["amount"])
        except (KeyError, TypeError, InvalidOperation):
            continue
        if not amount.is_finite():
            continue
        if kind == "unlock":
            item = items.get(r.get("item"))
            if item is not None and amount >= item.min_price:
                owned.add(item.item_id)
        elif kind == "tip":
            tips += amount
    return owned, tips
