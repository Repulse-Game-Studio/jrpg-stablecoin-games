"""The interface every chain family implements (see backends/evm.py, backends/solana.py)."""

from __future__ import annotations

from collections.abc import MutableMapping, Sequence
from typing import Any, Protocol


class Backend(Protocol):
    """start, advance, reverify, lookup, check_ready (and Solana's build_payment) are
    cooperative generators (see coio); normalize_tx and make_rpc are plain."""

    family: str

    def make_rpc(self, chain) -> Any:
        """The RPC object handed back to every other method for this chain."""

    def normalize_tx(self, text: str) -> str:
        """Validate a transaction id in this family's format. Raises ValueError."""

    def start(self, *, rpc, chain, wallet: str, token: str) -> dict:
        """Fields for a new session: must contain "start_block"; the rest is merged in."""

    def advance(self, pending: MutableMapping, receipts: MutableMapping, sid: str, *, rpc,
                chain, wallet: str, items: dict, now: float):
        """Move one pending session forward; returns a watcher.StepOutcome."""

    def reverify(self, receipt: dict, *, rpc, chain, wallets: Sequence[str]) -> str:
        """"keep" or "drop". Network trouble must answer "keep"."""

    def lookup(self, tx: str, *, config, chain, rpc, receipts: MutableMapping):
        """Restore candidates for tx on this chain, or None if the chain doesn't know it.
        Raises restore.RestoreError("tx_reverted" | "tx_not_a_payment")."""

    def check_ready(self, *, rpc, chain, wallet: str) -> dict[str, bool]:
        """token name -> can the developer receive it on this chain."""
