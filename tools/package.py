"""Build the drop-in release zip: dist/cryptopay-renpy-<version>.zip

The zip mirrors a Ren'Py project, so developers unzip it straight into their
project folder:

    game/cryptopay/...                      (.rpy files + browser checkout page)
    game/python-packages/cryptopay_core/... (engine-independent core)
    cryptopay-README.md                     (the developer guide)
    cryptopay-LICENSE.txt                   (MIT license)

Usage: uv run python tools/package.py
"""

from __future__ import annotations

import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))

from cryptopay_core import __version__  # noqa: E402

SKIP_PARTS = {"__pycache__"}
SKIP_SUFFIXES = {".rpyc", ".rpymc", ".pyc"}


def files_to_ship():
    base = ROOT / "plugin"
    for path in sorted((base / "game").rglob("*")):
        if path.is_dir() or SKIP_PARTS & set(path.parts) or path.suffix in SKIP_SUFFIXES:
            continue
        yield path, path.relative_to(base).as_posix()
    yield ROOT / "docs" / "developer-guide.md", "cryptopay-README.md"
    yield ROOT / "LICENSE", "cryptopay-LICENSE.txt"


def main() -> int:
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    target = dist / f"cryptopay-renpy-{__version__}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, arcname in files_to_ship():
            zf.write(path, arcname)
            print(f"  {arcname}")
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
