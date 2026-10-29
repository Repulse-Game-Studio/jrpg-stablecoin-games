"""Solana Pay transfer-request links (https://docs.solanapay.com/spec)."""

from __future__ import annotations

from urllib.parse import quote

from ..amounts import format_units


def solana_pay_uri(*, recipient: str, units: int, mint: str, reference: str, label: str,
                   message: str) -> str:
    """The amount always carries 6 decimals, so the item code in it survives."""
    params = [("amount", format_units(units)), ("spl-token", mint), ("reference", reference),
              ("label", label), ("message", message)]
    query = "&".join(f"{k}={quote(v, safe='')}" for k, v in params if v)
    return f"solana:{recipient}?{query}"
