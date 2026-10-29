"""What the checkout page may tell the game, checked the same way wherever the page runs
(local_server.py on desktop, overlay.py in the web build). A tx hash is only a hint: the
watcher verifies it on-chain before anything is unlocked."""

from __future__ import annotations

from .backends import normalize_any_tx
from .evm import to_checksum_address
from .solana.base58 import normalize_pubkey

BUILD_TIMEOUT_S = 15  # how long a page waits for the game to build a Solana transaction


def parse_tx_message(data) -> tuple[str, str | None]:
    """{"tx_hash", "from"?} -> (normalized tx hash or signature, normalized sender or None).
    Raises ValueError for a missing or malformed hash; a malformed sender is dropped."""
    if not isinstance(data, dict):
        raise ValueError("not an object")
    try:
        tx_hash = normalize_any_tx(data["tx_hash"])
    except (KeyError, TypeError, AttributeError) as e:
        raise ValueError("bad tx hash") from e
    sender = data.get("from")
    try:
        if sender:
            sender = (to_checksum_address(sender) if str(sender).startswith("0x")
                      else normalize_pubkey(sender))
        else:
            sender = None
    except (ValueError, TypeError):
        sender = None
    return tx_hash, sender


def parse_build_message(data) -> str:
    """{"account"} -> the player's Solana account, normalized. Raises ValueError."""
    if not isinstance(data, dict):
        raise ValueError("not an object")
    try:
        return normalize_pubkey(data["account"])
    except (KeyError, TypeError, AttributeError) as e:
        raise ValueError("bad account") from e
