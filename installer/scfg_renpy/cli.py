"""Copy the Stablecoin for Games plugin into a Ren'Py project, or update the copy there.

Ren'Py loads code only from a game's own game/ folder, so pip cannot install the plugin
into a game by itself. The package carries the plugin files and this command puts them
where Ren'Py looks:

    game/scfg/                         (screens, API, browser checkout page)
    game/python-packages/scfg_core/    (the engine)

Python 3.9+, standard library only.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

DOCS_URL = "https://jrpg.com/stablecoinforgames/docs/"
# Relative to a project's game/ folder.
PARTS = (Path("scfg"), Path("python-packages") / "scfg_core")
_IGNORE = shutil.ignore_patterns("__pycache__", "*.pyc", "*.rpyc", "*.rpymc")
_VERSION = re.compile(r'^__version__\s*=\s*["\']([^"\']+)["\']', re.M)


class InstallError(Exception):
    """A problem the developer can fix; printed without a traceback."""


def bundled_game_dir() -> Path:
    """The plugin's game/ folder: inside the installed package, or in the repository
    when run from a checkout."""
    here = Path(__file__).resolve().parent
    for candidate in (here / "plugin" / "game", here.parents[1] / "plugin" / "game"):
        if (candidate / "scfg").is_dir():
            return candidate
    raise InstallError("the plugin files are missing from this installation; reinstall scfg-renpy")


def read_version(game_dir: Path) -> str | None:
    init = game_dir / "python-packages" / "scfg_core" / "__init__.py"
    if not init.is_file():
        return None
    match = _VERSION.search(init.read_text(encoding="utf-8"))
    return match.group(1) if match else None


def find_game_dir(path: Path) -> Path:
    """The game/ folder of the Ren'Py project at `path` (the project folder or game/ itself)."""
    path = path.expanduser().resolve()
    if not path.is_dir():
        raise InstallError(f"{path} is not a folder")
    game = path if path.name == "game" else path / "game"
    if game.is_dir() and _has_renpy_files(game):
        return game
    raise InstallError(
        f"{path} doesn't look like a Ren'Py project: expected a game/ folder with .rpy files. "
        "Pass your project folder, the one that contains game/.")


def _has_renpy_files(game: Path) -> bool:
    return any(game.rglob("*.rpy")) or any(game.rglob("*.rpyc"))


def is_installed(game_dir: Path) -> bool:
    return any((game_dir / part).exists() for part in PARTS)


def copy_plugin(source_game: Path, game_dir: Path) -> None:
    for part in PARTS:
        target = game_dir / part
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source_game / part, target, ignore=_IGNORE)


def remove_plugin(game_dir: Path) -> None:
    for part in PARTS:
        target = game_dir / part
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()


def install(project: Path, source_game: Path | None = None) -> str:
    source_game = source_game or bundled_game_dir()
    game_dir = find_game_dir(project)
    if is_installed(game_dir):
        current = read_version(game_dir) or "unknown version"
        raise InstallError(
            f"the plugin is already in {game_dir} ({current}). "
            "Use `scfg-renpy update` to replace it.")
    copy_plugin(source_game, game_dir)
    return f"Installed Stablecoin for Games {read_version(game_dir)} into {game_dir}"


def update(project: Path, source_game: Path | None = None) -> str:
    source_game = source_game or bundled_game_dir()
    game_dir = find_game_dir(project)
    old = read_version(game_dir) if is_installed(game_dir) else None
    # Remove first, so files dropped in the new version don't linger.
    remove_plugin(game_dir)
    copy_plugin(source_game, game_dir)
    new = read_version(game_dir)
    if old is None:
        return f"Installed Stablecoin for Games {new} into {game_dir}"
    return f"Updated Stablecoin for Games {old} -> {new} in {game_dir}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="scfg-renpy",
        description="Add the Stablecoin for Games plugin to a Ren'Py project, or update it.")
    parser.add_argument("--version", action="version",
                        version=f"scfg-renpy {read_version(_safe_bundled()) or 'unknown'}")
    sub = parser.add_subparsers(dest="command", required=True)
    for name, text in (("install", "copy the plugin into a project"),
                       ("update", "replace the plugin in a project with this version")):
        cmd = sub.add_parser(name, help=text, description=text)
        cmd.add_argument("project", nargs="?", default=".",
                         help="your Ren'Py project folder (default: the current folder)")
    args = parser.parse_args(argv)

    try:
        action = install if args.command == "install" else update
        print(action(Path(args.project)))
    except InstallError as e:
        print(f"scfg-renpy: {e}", file=sys.stderr)
        return 1
    if args.command == "install":
        print(f"Next: add scfg.configure(...) in any .rpy file. Guide: {DOCS_URL}")
    return 0


def _safe_bundled() -> Path:
    try:
        return bundled_game_dir()
    except InstallError:
        return Path()
