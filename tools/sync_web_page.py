#!/usr/bin/env python3
"""Copy (or --check) the shared browser checkout page into each engine's addon.

plugin/game/scfg/web/ is the single source; every target gets a byte-for-byte copy. The Unity
package's copy (a Resources folder) names each file `<name>.bytes`, so Unity imports it as a
TextAsset, and keeps the `.meta` files Unity writes next to them (their GUIDs must stay stable):
sync never removes a `.meta`, and --check ignores them (tools/check_unity_meta.py checks that each
copy has one).

Only regular files are ever written, and never through a link. A symlink in a target (or a
target directory that is a symlink) is never followed: sync removes a symlinked entry before
copying and refuses a symlinked target directory or any subdirectory. A copy that is a hard link
(it shares its bytes with another file, the source say) is never written in place: each copy is
written to a new file that then replaces the entry. --check reports all of them.
"""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "plugin" / "game" / "scfg" / "web"
# RPG Maker MZ is no target: rpgmaker/build.mjs embeds the page in SCFG.js at build time.
UNITY_TARGET = (ROOT / "unity" / "Packages" / "com.repulse.scfg" / "Runtime" / "Unity" / "Resources"
                / "ScfgWeb")
TARGETS = [ROOT / "godot" / "addons" / "scfg" / "web", UNITY_TARGET]
# Targets whose copies carry a suffix (Unity imports only known extensions as TextAssets) and
# Unity .meta files.
UNITY_TARGETS = {UNITY_TARGET}
UNITY_SUFFIX = ".bytes"


def _unity(target: Path) -> bool:
    """A Unity target: one of UNITY_TARGETS, or the same folder in another copy of the package
    (package.py checks the package it is given)."""
    return target in UNITY_TARGETS or target.parts[-4:] == UNITY_TARGET.parts[-4:]


def _suffix(target: Path) -> str:
    return UNITY_SUFFIX if _unity(target) else ""


def _kept(target: Path, name: str) -> bool:
    """An entry of `target` that is not a copy and is left alone (a Unity .meta)."""
    return _unity(target) and name.endswith(".meta")


def _files(directory: Path) -> dict:
    """The regular files (never symlinks) directly in `directory`, by name."""
    if directory.is_symlink() or not directory.is_dir():
        return {}
    return {p.name: p for p in sorted(directory.iterdir())
            if stat.S_ISREG(p.lstat().st_mode)}


def _label(path: Path) -> str:
    return path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)


def _entry_problems(want: dict, target: Path) -> list:
    """Every entry of `target` that is not a regular file: a symlink, a directory or another
    kind of file (a directory where a page file belongs is "not a file")."""
    problems = []
    if target.is_symlink():
        return [f"symlink: {_label(target)} (the target directory must be a real directory)"]
    if not target.is_dir():
        return [f"not a directory: {_label(target)}"] if target.exists() else []
    for p in sorted(target.iterdir()):
        if _kept(target, p.name) and stat.S_ISREG(p.lstat().st_mode):
            continue
        mode = p.lstat().st_mode
        if stat.S_ISLNK(mode):
            problems.append(f"symlink: {_label(p)}")
        elif stat.S_ISDIR(mode):
            problems.append(f"{'not a file' if p.name in want else 'subdirectory'}: {_label(p)}")
        elif not stat.S_ISREG(mode):
            problems.append(f"not a file: {_label(p)}")
        elif p.lstat().st_nlink > 1:
            problems.append(f"hard link: {_label(p)} (shares its bytes with another file)")
    return problems


def check(src: Path, target: Path) -> list:
    want = {name + _suffix(target): path for name, path in _files(src).items()}
    problems = _entry_problems(want, target)
    if problems and target.is_symlink():
        return problems
    have = _files(target)
    present = {p.name for p in target.iterdir()} if target.is_dir() else set()
    label = _label(target)
    for name, path in want.items():
        if name not in present:
            problems.append(f"missing: {label}/{name}")
        elif name in have and have[name].read_bytes() != path.read_bytes():
            problems.append(f"drifted: {label}/{name}")
    for name in have:
        if name not in want and not _kept(target, name):
            problems.append(f"extra: {label}/{name}")
    return problems


def sync(src: Path, target: Path) -> list:
    """Make `target` an exact copy of `src`. Returns the problems that stopped it (nothing is
    written then): a symlinked or non-directory target, or a subdirectory in it."""
    if target.is_symlink() or (target.exists() and not target.is_dir()):
        return _entry_problems({}, target)
    target.mkdir(parents=True, exist_ok=True)
    want = {name + _suffix(target): path for name, path in _files(src).items()}
    refused = [p for p in _entry_problems(want, target)
               if p.startswith(("not a file", "subdirectory"))]
    if refused:
        return refused
    for p in sorted(target.iterdir()):
        if _kept(target, p.name) and stat.S_ISREG(p.lstat().st_mode):
            continue
        if not stat.S_ISREG(p.lstat().st_mode) or p.name not in want:
            p.unlink()  # a symlink is removed itself, never followed
    for name, path in want.items():
        # A new file replacing the entry: writing in place would go through a hard link.
        # (no stale temp file: every entry not in `want` was unlinked above)
        tmp = target / f".{name}.sync-tmp"
        shutil.copyfile(path, tmp)
        os.replace(tmp, target / name)
    return []


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail if any copy has drifted, is missing or is extra, or a target "
                             "holds a symlink or a directory")
    args = parser.parse_args(argv)
    if args.check:
        problems = [p for target in TARGETS for p in check(SRC, target)]
        if not _files(SRC):
            problems.append(f"source has no files: {SRC}")
        for problem in problems:
            print(problem)
        if problems:
            print("checkout page copies out of date: run python tools/sync_web_page.py "
                  "(edit plugin/game/scfg/web/ only, never the copies)", file=sys.stderr)
            return 1
        return 0
    status = 0
    for target in TARGETS:
        refused = sync(SRC, target)
        for problem in refused:
            print(f"refused: {problem} (remove it by hand)", file=sys.stderr)
        if refused:
            status = 1
        else:
            print(f"synced {SRC} -> {target}")
    return status


if __name__ == "__main__":
    sys.exit(main())
