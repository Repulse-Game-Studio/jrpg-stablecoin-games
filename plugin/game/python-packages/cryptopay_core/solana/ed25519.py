"""Is a 32-byte value a valid ed25519 public key (a point on the curve)?

Solana uses this to tell wallet addresses (on the curve) from program-derived
addresses such as token accounts (deliberately off the curve). This mirrors
curve25519-dalek's CompressedEdwardsY::decompress, which Solana uses: y is read
little-endian with the sign bit masked and reduced mod p (non-canonical encodings
accepted), and the point exists iff (y^2 - 1) / (d*y^2 + 1) is a square mod p.
"""

from __future__ import annotations

P = 2**255 - 19
D = (-121665 * pow(121666, P - 2, P)) % P


def is_on_curve(point: bytes) -> bool:
    if len(point) != 32:
        return False
    y = (int.from_bytes(point, "little") & ((1 << 255) - 1)) % P
    yy = y * y % P
    u = (yy - 1) % P
    v = (D * yy + 1) % P  # never 0: d is not a square mod p
    x2 = u * pow(v, P - 2, P) % P
    return x2 == 0 or pow(x2, (P - 1) // 2, P) == 1
