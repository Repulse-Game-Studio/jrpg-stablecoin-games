"""Keccak-256 as used by Ethereum (original Keccak padding, not NIST SHA3-256).

The permutation and sponge come from the Keccak team's public-domain reference
implementation, vendored in ``_xkcp.py``.
"""

from __future__ import annotations

from ._xkcp import Keccak

_RATE_BITS = 1088
_CAPACITY_BITS = 512
_KECCAK_SUFFIX = 0x01  # NIST SHA3 uses 0x06; Ethereum uses the original Keccak 0x01


def keccak256(data: bytes) -> bytes:
    return bytes(Keccak(_RATE_BITS, _CAPACITY_BITS, bytearray(data), _KECCAK_SUFFIX, 32))
