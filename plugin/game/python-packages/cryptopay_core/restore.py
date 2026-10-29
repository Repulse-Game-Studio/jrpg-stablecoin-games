"""Restoring purchases from a transaction hash: the chain is the ledger.

The player pastes a tx hash. We find the chain it lives on, pick out the USDC/USDT
transfers to the developer, and work out what each one bought from the code
encoded in its amount. Amounts that carry no code (e.g. an exchange sent a round
5.00) are ambiguous, so the player chooses which item it paid for, or keeps it
as a tip.
"""

from __future__ import annotations

from collections.abc import MutableMapping
from dataclasses import dataclass
from typing import Callable

from .amounts import decode, units_to_decimal
from .backends import backend_for
from .chains import Chain
from .config import Config
from .evm import TransferLog, parse_transfer_log
from .receipts import add_receipt, make_receipt, receipt_key
from .rpc import RpcError, RpcUnavailable
from .transfers import Transfer

TIP_CHOICE = "tip"


class RestoreError(Exception):
    """code: invalid_hash | tx_not_found | tx_reverted | tx_not_a_payment | network_unavailable"""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


@dataclass
class RestoreCandidate:
    chain: str
    token: str
    transfer: Transfer | TransferLog
    kind: str | None  # "unlock" | "tip" | None (= the player must choose)
    item: str | None
    choices: tuple[str, ...]  # item ids (+ "tip") offered when kind is None
    already_restored: bool


def classify(config: Config, value: int) -> tuple[str | None, str | None, tuple[str, ...]]:
    """Decide what a transfer of `value` base units paid for."""
    _, code, nonce = decode(value)
    amount = units_to_decimal(value)
    affordable = tuple(i.item_id for i in config.items.values() if i.min_price <= amount)
    if code == 0 and nonce == 0:
        # A whole-cent amount carries no code at all: could be an exchange-rounded
        # purchase or a plain tip. Let the player say which.
        return None, None, affordable + (TIP_CHOICE,)
    if code == 0:
        return "tip", None, ()
    item = config.item_by_code(code)
    if item is not None and amount >= item.min_price:
        return "unlock", item.item_id, ()
    return None, None, affordable + (TIP_CHOICE,)


def lookup_evm(tx_hash: str, *, config: Config, chain: Chain, rpc,
               receipts: MutableMapping) -> list[RestoreCandidate] | None:
    """Candidates for a normalized tx hash on one EVM chain; None if the chain lacks it."""
    receipt = yield from rpc.get_transaction_receipt(tx_hash)
    if receipt is None:
        return None
    if receipt.get("status") == "0x0":
        raise RestoreError("tx_reverted")
    by_address = {t.address: name for name, t in chain.tokens.items()}
    out = []
    for log in receipt.get("logs") or []:
        t = parse_transfer_log(log)
        if (t is None or t.token not in by_address or t.value <= 0
                or t.recipient not in config.accepted_wallets):
            continue
        kind, item, choices = classify(config, t.value)
        out.append(RestoreCandidate(
            chain=chain.key, token=by_address[t.token], transfer=t, kind=kind, item=item,
            choices=choices,
            already_restored=receipt_key(chain.key, t.tx_hash, t.log_index) in receipts))
    if not out:
        raise RestoreError("tx_not_a_payment")
    return out


def lookup(tx_hash: str, *, config: Config, chains: dict[str, Chain],
           rpc_for: Callable[[str], object], receipts: MutableMapping) -> list[RestoreCandidate]:
    network_trouble = False
    understood = False
    for key, chain in chains.items():
        backend = backend_for(chain)
        try:
            tx = backend.normalize_tx(tx_hash)
        except ValueError:
            continue
        understood = True
        try:
            out = yield from backend.lookup(tx, config=config, chain=chain, rpc=rpc_for(key),
                                            receipts=receipts)
        except (RpcUnavailable, RpcError):
            network_trouble = True
            continue
        if out is not None:
            return out
    if not understood:
        raise RestoreError("invalid_hash")
    raise RestoreError("network_unavailable" if network_trouble else "tx_not_found")


def apply(receipts: MutableMapping, candidates: list[RestoreCandidate],
          choices: dict[int, str] | None = None) -> list[dict]:
    """Store receipts for the candidates. `choices` maps candidate index -> item id or "tip"
    for candidates whose kind is None (candidates without a choice are skipped)."""
    choices = choices or {}
    added = []
    for i, c in enumerate(candidates):
        kind, item = c.kind, c.item
        if kind is None:
            choice = choices.get(i)
            if choice is None:
                continue
            if choice not in c.choices:
                raise ValueError(f"{choice!r} is not an allowed choice")
            kind, item = ("tip", None) if choice == TIP_CHOICE else ("unlock", choice)
        r = make_receipt(chain=c.chain, transfer=c.transfer, kind=kind, item=item,
                         token=c.token, source="restore")
        if add_receipt(receipts, r):
            added.append(r)
    return added
