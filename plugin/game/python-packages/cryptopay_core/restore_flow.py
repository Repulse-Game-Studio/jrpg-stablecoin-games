"""State behind the restore screen, independent of Ren'Py.

input -> checking -> (choose ->) saving -> done
"""

from __future__ import annotations

from collections.abc import Callable

from .backends import normalize_any_tx
from .models import RestoreResult
from .restore import RestoreError

MESSAGES = {
    "invalid_hash": "That doesn't look like a transaction hash or signature.",
    "tx_not_found": "Transaction not found. Check the hash, or try again later.",
    "tx_reverted": "That transaction failed on-chain; nothing was paid.",
    "tx_not_a_payment": "This transaction isn't a payment to this game.",
    "network_unavailable": "Can't reach the network. Try again in a moment.",
    "paste_blocked": "Your browser blocked pasting. Type the hash instead.",
    "failed": "Something went wrong. Please try again.",
}


class RestoreController:
    def __init__(self, *, submit: Callable):
        self.submit = submit
        self.state = "input"
        self.tx_input = ""
        self.error: str | None = None
        self.candidates: list = []
        self.choices: dict[int, str] = {}
        self.added: list[dict] = []
        self._future = None
        self._phase: str | None = None

    def message(self) -> str | None:
        return MESSAGES.get(self.error) if self.error else None

    def pending_choices(self) -> list[int]:
        return [i for i, c in enumerate(self.candidates)
                if c.kind is None and not c.already_restored]

    def check(self, text: str | None = None) -> bool:
        try:
            tx = normalize_any_tx(self.tx_input if text is None else text)
        except ValueError:
            self.error = "invalid_hash"
            return False
        self.error = None
        self.state = "checking"
        self._phase = "lookup"
        self._future = self.submit(lambda e: e.restore_lookup_co(tx))
        return True

    def choose(self, index: int, choice: str) -> None:
        self.choices[index] = choice

    @property
    def can_confirm(self) -> bool:
        """Every candidate that needs the player's choice has one."""
        return all(i in self.choices for i in self.pending_choices())

    def confirm(self) -> None:
        if self.state not in ("checking", "choose") or not self.can_confirm:
            return
        cands = [c for c in self.candidates if not c.already_restored]
        index_map = {self.candidates.index(c): i for i, c in enumerate(cands)}
        choices = {index_map[i]: v for i, v in self.choices.items() if i in index_map}
        self.state = "saving"
        self._phase = "apply"
        self._future = self.submit(lambda e: e.restore_apply(cands, choices))

    def poll(self) -> None:
        fut = self._future
        if fut is None or not fut.done():
            return
        self._future = None
        exc = fut.exception()
        if exc is not None:
            self.state = "input"
            self.error = exc.code if isinstance(exc, RestoreError) else "failed"
            return
        if self._phase == "lookup":
            self.candidates = fut.result()
            if all(c.already_restored for c in self.candidates):
                self.state = "done"
            elif self.pending_choices():
                self.state = "choose"
            else:
                self.confirm()
        else:
            self.added = fut.result()
            self.state = "done"

    def result(self) -> RestoreResult:
        if self.state == "done":
            return RestoreResult(status="restored" if self.added else "nothing_found",
                                 restored=list(self.added))
        return RestoreResult(status="cancelled", error=self.error)
