#!/usr/bin/env python3
"""Check that the Unity package's CHANGELOG.md is a copy of the repository's CHANGELOG.md.

    uv run python tools/check_unity_changelog.py          # exit 1 and say why, else 0
    uv run python tools/check_unity_changelog.py --write  # (re)write the copy, then check it

The Package Manager shows the CHANGELOG.md at a package's root (its "Changelog" link). The release
tarball gets the repository's file (tools/package.py --engine unity writes it in); a package
installed from a git URL is the tracked folder, so the folder keeps a byte-for-byte copy, and this
check keeps that copy current. Its .meta is checked by tools/check_unity_meta.py.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "CHANGELOG.md"
PACKAGE = ROOT / "unity" / "Packages" / "com.repulse.scfg"
NAME = "CHANGELOG.md"


def problems(package: Path = PACKAGE, source: Path = SOURCE) -> list[str]:
    copy = package / NAME
    if not copy.is_file():
        return [f"{NAME} is missing: uv run python tools/check_unity_changelog.py --write"]
    if copy.read_bytes() != source.read_bytes():
        return [f"{NAME} is not a copy of the repository's {NAME}: "
                "uv run python tools/check_unity_changelog.py --write"]
    return []


def write(package: Path = PACKAGE, source: Path = SOURCE) -> Path:
    target = package / NAME
    target.write_bytes(source.read_bytes())
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check that the Unity package's CHANGELOG.md is a copy of the repository's."
    )
    parser.add_argument("--write", action="store_true", help="(re)write the copy first")
    args = parser.parse_args(argv)
    if args.write:
        print(f"wrote {write()}")
    found = problems()
    for p in found:
        print(p)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
