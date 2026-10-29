"""Launch-time re-verification of stored receipts against the chain.

Each receipt is checked by its chain family's backend:

- EVM: one eth_getLogs call scoped to the receipt's own block. Block-scoped lookups
  keep working years later, even on nodes that no longer index old transaction hashes.
- Solana: getTransaction on the receipt's signature, asked of each endpoint in turn;
  a missing transaction only counts when an endpoint whose history reaches back to the
  receipt's slot says so.

Only a *definitive* mismatch removes a receipt; network trouble never does.
"""

from __future__ import annotations

from collections.abc import Mapping, MutableMapping, Sequence
from decimal import Decimal, InvalidOperation
from typing import Callable

from .amounts import decimal_to_units
from .backends import backend_for
from .chains import Chain
from .evm import TRANSFER_TOPIC, address_topic, parse_transfer_log
from .rpc import RpcError, RpcUnavailable

FUTURE_TOLERANCE_BLOCKS = 10_000


def reverify_receipts(receipts: MutableMapping, *, rpc_for: Callable[[str], object],
                      chains: dict[str, Chain],
                      wallets: Sequence[str] | Mapping[str, Sequence[str]]) -> list[str]:
    """Remove receipts that the chain contradicts. Returns the removed keys.

    `wallets` is either a sequence (the developer's wallets, for every family) or a mapping
    family -> sequence. A family with no wallets keeps its receipts: they cannot be judged.
    """
    removed: list[str] = []

    def drop(key: str) -> None:
        receipts.pop(key, None)
        removed.append(key)

    for key, r in list(receipts.items()):
        try:
            chain = chains.get(r["chain"])
            # Malformed receipts are dropped here; the backend then reads the same fields.
            r["tx_hash"].lower(), int(r["log_index"]), int(r["block"])
            decimal_to_units(Decimal(r["amount"]))
            token_name = r["token"]
        except (KeyError, TypeError, ValueError, AttributeError, InvalidOperation):
            drop(key)
            continue
        if chain is None:
            continue  # chain unknown to this build: cannot judge, keep
        token = chain.tokens.get(token_name)
        if token is None:
            drop(key)
            continue
        family_wallets = (wallets.get(chain.family, ()) if isinstance(wallets, Mapping)
                          else wallets)
        if not family_wallets:
            continue  # no wallet of this family configured any more: cannot judge, keep
        verdict = yield from backend_for(chain).reverify(
            r, rpc=rpc_for(chain.key), chain=chain, wallets=tuple(family_wallets))
        if verdict == "drop":
            drop(key)
    return removed


def check_evm_receipt(r: dict, *, rpc, chain: Chain, wallets: Sequence[str]) -> str:
    """Judge one stored EVM receipt: "drop" if the chain contradicts it, else "keep"."""
    tx_hash, log_index, block = r["tx_hash"].lower(), int(r["log_index"]), int(r["block"])
    units = decimal_to_units(Decimal(r["amount"]))
    token = chain.tokens[r["token"]]
    try:
        head = yield from rpc.block_number()
        if block > head:
            # probably a lagging endpoint: keep, unless the block is impossibly far ahead
            return "drop" if block > head + FUTURE_TOLERANCE_BLOCKS else "keep"
        logs = yield from rpc.get_logs(
            token.address, [TRANSFER_TOPIC, None, [address_topic(w) for w in wallets]],
            block, block)
    except (RpcUnavailable, RpcError):
        return "keep"
    genuine = any(
        t is not None and t.tx_hash == tx_hash and t.log_index == log_index
        and t.value == units and t.recipient in wallets and t.token == token.address
        for t in (parse_transfer_log(lg) for lg in logs)
    )
    return "keep" if genuine else "drop"
