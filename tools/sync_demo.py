"""Copy the plugin into a demo game so the demo can run.

Usage:
    uv run python tools/sync_demo.py                  # Ren'Py
    uv run python tools/sync_demo.py --engine godot   # Godot

Ren'Py copies plugin/game/ into examples/demo_game/game/; Godot copies godot/addons/scfg/ into
the addons/scfg/ folder of each Godot project that uses it: godot/demo/ (the demo game the
end-to-end tests play) and godot/showcase/ (a tour of the whole API).
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RENPY_SRC = ROOT / "plugin" / "game"
RENPY_DST = ROOT / "examples" / "demo_game" / "game"
GODOT_SRC = ROOT / "godot" / "addons" / "scfg"
GODOT_PROJECTS = (ROOT / "godot" / "demo", ROOT / "godot" / "showcase")


def _copy(src: Path, target: Path, ignore) -> None:
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(src, target, ignore=ignore)
    print(f"copied {src} -> {target}")


def sync_renpy(src: Path = RENPY_SRC, dst: Path = RENPY_DST) -> None:
    for name in ("scfg", "python-packages"):
        _copy(src / name, dst / name, shutil.ignore_patterns("__pycache__", "*.rpyc"))


def sync_godot(src: Path = GODOT_SRC, projects: tuple[Path, ...] = GODOT_PROJECTS) -> None:
    for project in projects:
        _copy(src, project / "addons" / "scfg", shutil.ignore_patterns("tests", ".godot", "*.uid"))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--engine", choices=("renpy", "godot"), default="renpy")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    engine = parse_args(argv).engine
    (sync_godot if engine == "godot" else sync_renpy)()


if __name__ == "__main__":
    main()
