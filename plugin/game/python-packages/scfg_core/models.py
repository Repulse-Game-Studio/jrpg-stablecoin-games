"""Value objects shared with the developer's script."""

from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal


@dataclass(frozen=True)
class Item:
    """A paid unlock declared with scfg.add_item()."""

    item_id: str
    code: int
    price: Decimal
    token: str
    title: str
    min_price: Decimal


@dataclass
class PaymentResult:
    """What pay(), callbacks and on_result receive.

    status: "paid" | "already_owned" | "cancelled" | "expired" | "underpaid" | "failed" | "pending"
    """

    status: str
    kind: str
    item: str | None = None
    amount: Decimal | None = None
    token: str | None = None
    chain: str | None = None
    tx_hash: str | None = None
    payer: str | None = None
    session_id: str | None = None
    error: str | None = None


@dataclass
class RestoreResult:
    """What restore() returns. status: "restored" | "nothing_found" | "cancelled" | "failed"."""

    status: str
    restored: list[dict] = field(default_factory=list)
    error: str | None = None
