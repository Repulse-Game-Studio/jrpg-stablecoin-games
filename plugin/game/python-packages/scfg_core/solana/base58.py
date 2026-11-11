"""Base58 (Bitcoin alphabet), as used for Solana addresses and signatures."""

from __future__ import annotations

import math

ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
_INDEX = {c: i for i, c in enumerate(ALPHABET)}


def b58encode(data: bytes) -> str:
    n = int.from_bytes(data, "big")
    out = []
    while n:
        n, r = divmod(n, 58)
        out.append(ALPHABET[r])
    zeros = len(data) - len(data.lstrip(b"\0"))
    return "1" * zeros + "".join(reversed(out))


def b58decode(text: str) -> bytes:
    if not isinstance(text, str) or not text:
        raise ValueError(f"not base58: {text!r}")
    n = 0
    for c in text:
        if c not in _INDEX:
            raise ValueError(f"invalid base58 character {c!r}")
        n = n * 58 + _INDEX[c]
    zeros = len(text) - len(text.lstrip("1"))
    body = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\0" * zeros + body


def max_encoded_length(length: int) -> int:
    """The longest base58 text of `length` bytes: 44 characters for 32, 88 for 64."""
    return math.ceil(length * math.log(256) / math.log(58))


def decode_fixed(text: str, length: int) -> bytes:
    """`length` bytes from base58 `text`. Decoding is quadratic in the text's length, so text
    longer than any encoding of `length` bytes is refused before it is decoded. Bad input is a
    ValueError, a `length` that is not an int (a bool, a float, a str, ...) included."""
    if isinstance(length, bool) or not isinstance(length, int):
        raise ValueError(f"not a byte count: {length!r}")
    try:
        limit = max_encoded_length(length)
    except OverflowError:  # beyond a float: no text is that long
        raise ValueError(f"not a byte count: {length!r}") from None
    if isinstance(text, str) and len(text) > limit:
        raise ValueError(f"too long for {length} bytes: {len(text)} characters")
    raw = b58decode(text)
    if len(raw) != length:
        raise ValueError(f"expected {length} bytes, got {len(raw)}: {text!r}")
    return raw


def normalize_pubkey(text) -> str:
    if not isinstance(text, str):
        raise ValueError(f"not an address: {text!r}")
    return b58encode(decode_fixed(text.strip(), 32))


def normalize_signature(text) -> str:
    if not isinstance(text, str):
        raise ValueError(f"not a signature: {text!r}")
    return b58encode(decode_fixed(text.strip(), 64))
