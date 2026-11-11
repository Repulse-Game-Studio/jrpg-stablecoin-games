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
from typing import Callable

from .amounts import decimal_to_units, stored_amount
from .backends import backend_for
from .chains import Chain
from .errors import print_error
from .evm import TRANSFER_TOPIC, address_topic, parse_transfer_log
from .ints import int_from
from .rpc import RpcError, RpcUnavailable

FUTURE_TOLERANCE_BLOCKS = 10_000


def reverify_receipts(receipts: MutableMapping, *, rpc_for: Callable[[str], object],
                      chains: dict[str, Chain],
                      wallets: Sequence[str] | Mapping[str, Sequence[str]],
                      on_error: Callable[[BaseException], None] | None = None) -> list[str]:
    """Remove receipts that the chain contradicts. Returns the removed keys.

    `wallets` is either a sequence (the developer's wallets, for every family) or a mapping
    family -> sequence. A family with no wallets keeps its receipts: they cannot be judged.

    The removed keys come back in the store's iteration order (the order receipts were
    stored in, as loaded), which is deterministic, so every port reports them alike.

    A stored entry whose own fields are invalid is removed, and the reason is reported to
    `on_error` (by default printed): a forged receipt cannot escape verification by making its
    check fail. Invalid fields, checked before any RPC: a missing field; a chain, token or item
    that cannot be a key; a tx hash that is not a str; a log index or block that is not an
    int >= 0 as ints.int_from reads it (Python's int(), and at most 4300 digits on every
    interpreter); an amount that is not a number of base units within the range of
    amounts.stored_amount; and, on a chain this build knows, a tx id that is not
    its family's (an EVM hash: 0x and 64 hex digits; a Solana signature: 64 base58 bytes, at
    most 88 characters) or has surrounding whitespace. A receipt naming a chain this build does
    not know (a removed custom chain, the other network's) is kept, its family fields
    unchecked: a configuration change must not erase purchases.

    A check that fails because of what a node returned is not the receipt's fault: that
    receipt is kept as unverifiable, the failure is reported, and the receipts after it are
    still checked.
    """
    removed: list[str] = []
    report = on_error or print_error

    def drop(key: str) -> None:
        receipts.pop(key, None)
        removed.append(key)

    for key, r in list(receipts.items()):
        problem = _invalid_fields(r, chains)
        if problem is not None:
            drop(key)
            report(ValueError(f"stored receipt {key!r} is invalid and was removed: {problem!r}"))
            continue
        try:
            verdict = yield from _judge(r, rpc_for=rpc_for, chains=chains, wallets=wallets)
        except Exception as e:
            report(e)  # node data the check could not read: unverifiable, kept
            continue
        if verdict == "drop":
            drop(key)
    return removed


def _invalid_fields(r, chains: dict[str, Chain]) -> BaseException | None:
    """Why the stored entry `r` is not a receipt the checks can read, or None. The backends
    then read the same fields. On a chain this build knows, the tx id must also be one of its
    family's (an EVM hash: 0x and 64 hex digits; a Solana signature: 64 base58 bytes), as
    stored, with no surrounding whitespace."""
    try:
        hash(r["chain"]), hash(r["token"]), hash(r.get("item"))
        tx = r["tx_hash"]
        tx.lower()
        if int_from(r["log_index"]) < 0 or int_from(r["block"]) < 0:
            raise ValueError("log index and block must be >= 0")
        decimal_to_units(stored_amount(r["amount"]))
        chain = chains.get(r["chain"])
        if chain is not None:
            if tx != tx.strip():
                raise ValueError(f"tx id with surrounding whitespace: {tx!r}")
            backend_for(chain).normalize_tx(tx)
    except (KeyError, TypeError, ValueError, AttributeError, ArithmeticError) as e:
        return e
    return None


def _judge(r: dict, *, rpc_for, chains, wallets):
    """A well-formed receipt's verdict: "drop" or "keep"."""
    chain = chains.get(r["chain"])
    if chain is None:
        return "keep"  # chain unknown to this build: cannot judge, keep
    if chain.tokens.get(r["token"]) is None:
        return "drop"
    family_wallets = (wallets.get(chain.family, ()) if isinstance(wallets, Mapping)
                      else wallets)
    if not family_wallets:
        return "keep"  # no wallet of this family configured any more: cannot judge, keep
    return (yield from backend_for(chain).reverify(
        r, rpc=rpc_for(chain.key), chain=chain, wallets=tuple(family_wallets)))


def check_evm_receipt(r: dict, *, rpc, chain: Chain, wallets: Sequence[str]) -> str:
    """Judge one stored EVM receipt: "drop" if the chain contradicts it, else "keep"."""
    tx_hash, log_index, block = (r["tx_hash"].lower(), int_from(r["log_index"]),
                                 int_from(r["block"]))
    units = decimal_to_units(stored_amount(r["amount"]))
    token = chain.tokens[r["token"]]
    topics = [TRANSFER_TOPIC, None, [address_topic(w) for w in wallets]]

    def matches(transfers) -> bool:
        return any(
            t is not None and t.tx_hash == tx_hash and t.log_index == log_index
            and t.value == units and t.recipient in wallets and t.token == token.address
            for t in transfers)

    try:
        head = yield from rpc.block_number()
        if block > head:
            # probably a lagging endpoint: keep, unless the block is impossibly far ahead
            return "drop" if block > head + FUTURE_TOLERANCE_BLOCKS else "keep"
        logs = yield from rpc.get_logs(token.log_source, topics, block, block)
        if matches(chain.parse_transfer(lg) for lg in logs):
            return "keep"
        if not token.log_address:
            return "drop"
        # A receipt stored by a game that added this chain itself before it was built in
        # (add_chain("arc", ...)) points at the token contract's own log. That log is still
        # a genuine transfer, so it keeps the purchase.
        logs = yield from rpc.get_logs(token.address, topics, block, block)
    except (RpcUnavailable, RpcError):
        return "keep"
    return "keep" if matches(parse_transfer_log(lg) for lg in logs) else "drop"
