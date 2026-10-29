"""Payment links (EIP-681) and QR code geometry.

The QR matrix comes from the vendored Project Nayuki generator (_qrcodegen.py).
Rendering is left to Ren'Py: ``qr_rects`` merges each row's dark modules into
horizontal runs, so the screen draws a few hundred rectangles instead of thousands
of single squares.
"""

from __future__ import annotations

from ._qrcodegen import QrCode


def eip681_transfer_uri(*, token_address: str, chain_id: int, recipient: str, units: int) -> str:
    """ERC-20 transfer request that mobile wallets understand when scanned or opened."""
    return f"ethereum:{token_address}@{chain_id}/transfer?address={recipient}&uint256={units}"


def qr_matrix(text: str) -> list[list[bool]]:
    """Square matrix of modules, True = dark. Medium error correction."""
    qr = QrCode.encode_text(text, QrCode.Ecc.MEDIUM)
    n = qr.get_size()
    return [[qr.get_module(x, y) for x in range(n)] for y in range(n)]


def qr_rects(text: str, border: int = 2) -> tuple[int, list[tuple[int, int, int]]]:
    """(size in modules including the quiet-zone border, [(x, y, width), ...] dark runs)."""
    matrix = qr_matrix(text)
    n = len(matrix)
    runs = []
    for y, row in enumerate(matrix):
        x = 0
        while x < n:
            if row[x]:
                start = x
                while x < n and row[x]:
                    x += 1
                runs.append((start + border, y + border, x - start))
            else:
                x += 1
    return n + 2 * border, runs
