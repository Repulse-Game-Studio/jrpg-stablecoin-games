"""Program-derived addresses and associated token accounts (classic SPL Token program)."""

from __future__ import annotations

import hashlib

from .base58 import b58encode, decode_fixed
from .ed25519 import is_on_curve

TOKEN_PROGRAM_ID = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
ASSOCIATED_TOKEN_PROGRAM_ID = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
MAX_SEED_LEN = 32
MAX_SEEDS = 16
_MARKER = b"ProgramDerivedAddress"


def create_program_address(seeds: list[bytes], program_id: bytes) -> bytes | None:
    if len(seeds) > MAX_SEEDS or any(len(s) > MAX_SEED_LEN for s in seeds):
        raise ValueError("too many seeds or a seed longer than 32 bytes")
    digest = hashlib.sha256(b"".join(seeds) + program_id + _MARKER).digest()
    return None if is_on_curve(digest) else digest


def find_program_address(seeds: list[bytes], program_id: bytes) -> tuple[bytes, int]:
    for bump in range(255, -1, -1):
        address = create_program_address(list(seeds) + [bytes([bump])], program_id)
        if address is not None:
            return address, bump
    raise ValueError("no viable bump seed")


def associated_token_address(owner: str, mint: str) -> str:
    """The developer's (or player's) token account for `mint`, as wallets create it."""
    seeds = [decode_fixed(owner, 32), decode_fixed(TOKEN_PROGRAM_ID, 32), decode_fixed(mint, 32)]
    address, _ = find_program_address(seeds, decode_fixed(ASSOCIATED_TOKEN_PROGRAM_ID, 32))
    return b58encode(address)
