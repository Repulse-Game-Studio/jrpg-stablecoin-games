"""Generate the white, tintable UI images used by the cryptopay screens.

The screens tint these at runtime (TintMatrix), so one set of images serves every
accent color. Drawn at 2x and anti-aliased by 4x4 supersampling. Stdlib only.

    plugin/game/cryptopay/ui/fill.png   rounded rectangle, filled
    plugin/game/cryptopay/ui/line.png   rounded rectangle, outline
    plugin/game/cryptopay/ui/dot.png    circle

Usage: uv run python tools/make_ui_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "plugin" / "game" / "cryptopay" / "ui"
SIZE = 64        # 2x: the screens show these at half size on a 1280x720 game
RADIUS = 20
LINE = 3
SAMPLES = 4


def inside_rounded(x: float, y: float, size: float, radius: float) -> bool:
    cx = min(max(x, radius), size - radius)
    cy = min(max(y, radius), size - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2 and 0 <= x <= size and 0 <= y <= size


def coverage(shape, size: int) -> list[list[float]]:
    rows = []
    step = 1.0 / SAMPLES
    for py in range(size):
        row = []
        for px in range(size):
            hits = sum(shape(px + (i + 0.5) * step, py + (j + 0.5) * step)
                       for i in range(SAMPLES) for j in range(SAMPLES))
            row.append(hits / SAMPLES ** 2)
        rows.append(row)
    return rows


def write_png(path: Path, alpha: list[list[float]]) -> None:
    height, width = len(alpha), len(alpha[0])
    raw = b"".join(b"\x00" + b"".join(bytes((255, 255, 255, round(a * 255))) for a in row)
                   for row in alpha)

    def chunk(kind: bytes, data: bytes) -> bytes:
        crc = struct.pack(">I", zlib.crc32(kind + data))
        return struct.pack(">I", len(data)) + kind + data + crc

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    fill = coverage(lambda x, y: inside_rounded(x, y, SIZE, RADIUS), SIZE)
    inner_size, inner_radius = SIZE - 2 * LINE, RADIUS - LINE
    inner = coverage(lambda x, y: inside_rounded(x - LINE, y - LINE, inner_size, inner_radius),
                     SIZE)
    line = [[max(0.0, f - i) for f, i in zip(fr, ir)] for fr, ir in zip(fill, inner)]
    dot = coverage(lambda x, y: (x - 12) ** 2 + (y - 12) ** 2 <= 11 ** 2, 24)
    for name, alpha in (("fill.png", fill), ("line.png", line), ("dot.png", dot)):
        write_png(OUT / name, alpha)
        print(f"wrote {OUT / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
