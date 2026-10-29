"""Base58 (Bitcoin alphabet), as used for Solana addresses and signatures."""

from __future__ import annotations

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


def decode_fixed(text: str, length: int) -> bytes:
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
