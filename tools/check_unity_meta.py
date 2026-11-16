#!/usr/bin/env python3
"""Check that every file and folder of the Unity package has its `.meta`, and every `.meta` its asset.

    uv run python tools/check_unity_meta.py          # exit 1 and list the problems, else 0

Unity writes a `.meta` (with the asset's GUID) next to each file and folder it imports. A package installed from a
git URL is read-only, so Unity cannot write them there: a missing `.meta` makes the asset unusable (and its GUID, which
other assets reference, would differ per install). An orphan `.meta` (its asset gone) is left behind by a rename.
Unity skips hidden entries (a leading dot) and folders ending in `~` (Samples~, Documentation~), and so does this
check. Each GUID must also be unique.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "unity" / "Packages"
GUID = re.compile(r"^guid: ([0-9a-f]{32})$", re.M)


def _skipped(path: Path, base: Path) -> bool:
    """Unity ignores hidden entries and `~` folders, and everything under them."""
    for part in path.relative_to(base).parts:
        if part.startswith(".") or part.endswith("~"):
            return True
    return False


def problems(base: Path = PACKAGES) -> list[str]:
    out: list[str] = []
    guids: dict[str, Path] = {}
    for package in sorted(p for p in base.iterdir() if p.is_dir()):
        for path in sorted(package.rglob("*")):
            if _skipped(path, package):
                continue
            rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            if path.name.endswith(".meta"):
                asset = path.with_name(path.name[: -len(".meta")])
                if not asset.exists():
                    out.append(f"orphan .meta: {rel}")
                    continue
                m = GUID.search(path.read_text(encoding="utf-8", errors="replace"))
                if not m:
                    out.append(f"no guid: {rel}")
                elif m.group(1) in guids:
                    out.append(f"duplicate guid {m.group(1)}: {rel} and {guids[m.group(1)]}")
                else:
                    guids[m.group(1)] = rel
                continue
            if not path.with_name(path.name + ".meta").exists():
                out.append(f"missing .meta: {rel}")
    return out


def main() -> int:
    found = problems()
    for p in found:
        print(p)
    if found:
        print("open unity/Project in the Unity editor once (it writes the .meta files), then commit them",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
