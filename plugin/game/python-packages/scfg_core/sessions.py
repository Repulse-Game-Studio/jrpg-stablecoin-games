"""Pending payment sessions.

A session is one payment attempt, created when the player picks a network. It is
stored as a plain dict (Ren'Py pickles it into ``persistent``) so it survives
restarts and the watcher can keep looking for the payment for up to 24 hours.
"""

from __future__ import annotations

import re
import secrets
from collections.abc import MutableMapping
from decimal import Decimal

from .amounts import encode, parse_amount

SESSION_WINDOW_S = 30 * 60
SCAN_HORIZON_S = 24 * 3600
HINT_TIMEOUT_S = 30 * 60
UI_STATUSES = ("open", "pending", "cancelled", "expired")
_TX_HASH = re.compile(r"^0x[0-9a-fA-F]{64}$")


def new_session_id() -> str:
    return "ps_" + secrets.token_hex(8)


def normalize_tx_hash(tx_hash: str) -> str:
    if not isinstance(tx_hash, str) or not _TX_HASH.match(tx_hash.strip()):
        raise ValueError(f"not a transaction hash: {tx_hash!r}")
    return tx_hash.strip().lower()


def create_session(store: MutableMapping, *, kind: str, item: str | None, title: str,
                   token: str, chain: str, price, code: int, start_block: int, now: float,
                   nonce: int | None = None, extra: dict | None = None) -> dict:
    if kind not in ("unlock", "tip"):
        raise ValueError(f"invalid kind {kind!r}")
    if nonce is None:
        nonce = secrets.randbelow(100)
    p = parse_amount(price)
    sid = new_session_id()
    rec = {
        "id": sid, "kind": kind, "item": item, "title": title, "token": token, "chain": chain,
        "price": str(p), "code": code, "nonce": nonce, "units": encode(p, code, nonce),
        "start_block": start_block, "cursor": start_block, "created_at": now,
        "state": "awaiting", "ui_status": "open",
        "hint_tx": None, "hint_at": None, "match": None,
    }
    for key in extra or {}:
        if key in rec:
            raise ValueError(f"session field {key!r} is reserved")
    rec.update(extra or {})
    store[sid] = rec
    return rec


def get_session(store: MutableMapping, sid: str) -> dict | None:
    return store.get(sid)


def remove_session(store: MutableMapping, sid: str) -> None:
    store.pop(sid, None)


def list_sessions(store: MutableMapping) -> list[dict]:
    return sorted(store.values(), key=lambda r: r["created_at"])


def set_hint(store: MutableMapping, sid: str, tx_hash: str, now: float,
             normalize=normalize_tx_hash) -> None:
    rec = store[sid]
    rec["hint_tx"] = normalize(tx_hash)
    rec["hint_at"] = now


def clear_hint(rec: dict) -> None:
    rec["hint_tx"] = None
    rec["hint_at"] = None


def set_ui_status(store: MutableMapping, sid: str, status: str) -> None:
    if status not in UI_STATUSES:
        raise ValueError(f"invalid ui status {status!r}")
    store[sid]["ui_status"] = status


def is_past_window(rec: dict, now: float) -> bool:
    return now - rec["created_at"] >= SESSION_WINDOW_S


def is_past_horizon(rec: dict, now: float) -> bool:
    return now - rec["created_at"] >= SCAN_HORIZON_S


def session_price(rec: dict) -> Decimal:
    return Decimal(rec["price"])
