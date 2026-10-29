"""Copy the plugin into examples/demo_game/game/ so the demo can run.

Usage: uv run python tools/sync_demo.py
"""

from __future__ import annotations

import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "plugin" / "game"
DST = ROOT / "examples" / "demo_game" / "game"

for name in ("cryptopay", "python-packages"):
    target = DST / name
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(SRC / name, target, ignore=shutil.ignore_patterns("__pycache__", "*.rpyc"))
    print(f"copied {SRC / name} -> {target}")
