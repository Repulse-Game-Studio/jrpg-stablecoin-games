"""Vector registry, writer and staleness checker.

SOURCES maps a path relative to the output directory (for example "tier1/keccak.json")
to a producer returning the JSON-able document for that file.
"""

from __future__ import annotations

import importlib
from pathlib import Path
from typing import Callable

from . import canonical

SOURCES: dict[str, Callable[[], dict]] = {}

# Modules that register vector files. Each exposes a module-level `SOURCES` dict with the
# same shape as above. Later tasks append their module here.
MODULES = ["scfg_conformance.tier1", "scfg_conformance.tier1_config", "scfg_conformance.tier2"]


def load_all() -> None:
    """Import every vector module and merge its SOURCES into the registry (idempotent)."""
    for name in MODULES:
        SOURCES.update(importlib.import_module(name).SOURCES)


def write_all(out_dir: Path) -> list[Path]:
    """Write every registered source under out_dir; return the paths written, sorted."""
    out_dir = Path(out_dir)
    written = []
    for rel in sorted(SOURCES):
        path = out_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(canonical.dumps(SOURCES[rel]()).encode("utf-8"))
        written.append(path)
    return written


def check(out_dir: Path) -> list[str]:
    """Compare out_dir with what the sources would produce.

    Returns "missing: <rel>", "stale: <rel>" and "extra: <rel>" lines, sorted. Only *.json
    files count as candidates for "extra". An absent directory counts as empty.
    """
    out_dir = Path(out_dir)
    problems = []
    for rel in sorted(SOURCES):
        path = out_dir / rel
        if not path.is_file():
            problems.append(f"missing: {rel}")
        elif path.read_bytes() != canonical.dumps(SOURCES[rel]()).encode("utf-8"):
            problems.append(f"stale: {rel}")
    if out_dir.is_dir():
        for path in out_dir.rglob("*.json"):
            rel = path.relative_to(out_dir).as_posix()
            if path.is_file() and rel not in SOURCES:
                problems.append(f"extra: {rel}")
    return sorted(problems)
