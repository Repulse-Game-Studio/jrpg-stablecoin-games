"""Build the drop-in release zips: dist/scfg-renpy-<version>.zip (default) and
dist/scfg-godot-<version>.zip (--engine godot).

The Ren'Py zip mirrors a Ren'Py project, so developers unzip it straight into their
project folder:

    game/scfg/...                      (.rpy files + browser checkout page)
    game/python-packages/scfg_core/... (engine-independent core)
    scfg-README.md                     (developer guide, joined from docs/developers/)
    scfg-LICENSE.txt                   (MIT license)

The Godot zip unzips into a Godot project folder:

    addons/scfg/...                    (the addon: autoload, sheet, core, checkout page)
    scfg-README.md                     (Godot developer guide, joined from docs/developers/godot/)
    scfg-LICENSE.txt                   (MIT license)
    scfg-NOTICES.txt                   (notices of the third-party code the addon ports)

Usage: uv run python tools/package.py [--engine renpy|godot]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))

from scfg_core import __version__  # noqa: E402

SKIP_PARTS = {"__pycache__"}
SKIP_SUFFIXES = {".rpyc", ".rpymc", ".pyc"}
DOCS = ROOT / "docs"
SIDEBARS = ROOT / "website" / "sidebars.json"
GUIDE_TITLE = "# Stablecoin for Games: Developer Guide"
GODOT_GUIDE_TITLE = "# Stablecoin for Games: Godot Developer Guide"
GODOT_PAGES = "developers/godot/"
GODOT_ADDON = ROOT / "godot" / "addons" / "scfg"
GODOT_SKIP_PARTS = {"__pycache__", ".godot", "tests", "demo", "e2e"}
GODOT_SKIP_SUFFIXES = {".uid", ".pyc"}
LINK = re.compile(r"\]\(([^)\s]+)\)")


def slug(heading):
    """GitHub's anchor for a heading line."""
    text = heading.lstrip("#").strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def sidebar_pages(items, level=1):
    """(page path, nesting level) for docs sidebar items, in sidebar order."""
    for item in items:
        if isinstance(item, str):
            yield item + ".md", level
            continue
        link = item.get("link") or {}
        if link.get("type") == "doc":
            yield link["id"] + ".md", level
        yield from sidebar_pages(item.get("items", []), level + 1)


def developer_guide(prefix="developers/", skip=(GODOT_PAGES,), title=GUIDE_TITLE, intro=True):
    """Join the docs pages under `prefix` (minus those under `skip`), in website/sidebars.json
    order, into one file.

    Each page's headings move down one level, plus one per level of nesting in the
    sidebar, and links between pages become anchors in the joined file. A link to a
    page outside the joined ones fails. With `intro`, the first page is the guide's intro
    (its title line is replaced by `title`); without, every page is a section under `title`.
    """
    sidebar = json.loads(SIDEBARS.read_text(encoding="utf-8"))["docs"]
    depth = {
        p: level
        for p, level in sidebar_pages(sidebar)
        if p.startswith(prefix) and not p.startswith(tuple(skip))
    }
    pages = list(depth)
    # The first page is the guide's intro and the rest sit under it; count levels from them.
    base = min((depth[p] for p in (pages[1:] if intro else pages)), default=1) - 1
    depth = {p: max(1, level - base) for p, level in depth.items()}
    texts = {p: (DOCS / p).read_text(encoding="utf-8") for p in pages}
    tops = {p: "#" + slug(texts[p].splitlines()[0]) for p in pages}
    if intro:
        tops[pages[0]] = "#" + slug(title)

    def relink(page, match):
        target = match.group(1)
        if target.startswith(("http://", "https://", "#")):
            return match.group(0)
        path, _, anchor = target.partition("#")
        resolved = PurePosixPath(page).parent.joinpath(path).as_posix()
        while "/../" in resolved or resolved.startswith("../"):
            resolved = re.sub(r"[^/]+/\.\./", "", resolved, count=1)
        if resolved not in tops:
            raise ValueError(f"{page}: link to {target} leaves the developer guide")
        return "](" + ("#" + anchor if anchor else tops[resolved]) + ")"

    out = [title]
    for page in pages:
        lines = texts[page].rstrip("\n").split("\n")
        if intro and page == pages[0]:
            lines = lines[1:]
        fence = False
        for line in lines:
            if line.startswith("```"):
                fence = not fence
            elif not fence:
                if line.startswith("#"):
                    line = "#" * depth[page] + line
                line = LINK.sub(lambda m, page=page: relink(page, m), line)
            out.append(line)
        out.append("")
    return "\n".join(out)


def files_to_ship():
    base = ROOT / "plugin"
    for path in sorted((base / "game").rglob("*")):
        if path.is_dir() or SKIP_PARTS & set(path.parts) or path.suffix in SKIP_SUFFIXES:
            continue
        yield path, path.relative_to(base).as_posix()
    yield ROOT / "LICENSE", "scfg-LICENSE.txt"


def godot_guide():
    """The Godot developer guide (docs/developers/godot/), joined like the Ren'Py one."""
    return developer_guide(GODOT_PAGES, (), GODOT_GUIDE_TITLE, intro=False)


def godot_notices():
    """The notices of the code the addon vendors or ports, for scfg-NOTICES.txt.

    The Nayuki notice is read from qrcodegen.gd, where the port keeps it, so the two cannot drift.
    """
    lines = (GODOT_ADDON / "core" / "qrcodegen.gd").read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("## The original notice"))
    end = next(i for i, line in enumerate(lines) if not line.startswith("##"))
    qr = "\n".join(line[3:] if line.startswith("## ") else "" for line in lines[start + 2 : end])
    return (
        "Third-party code in addons/scfg\n"
        "===============================\n\n"
        "QR Code generator (addons/scfg/core/qrcodegen.gd)\n"
        "-------------------------------------------------\n"
        "A GDScript port of Project Nayuki's QR Code generator library (Python). The original\n"
        "notice follows.\n\n"
        f"{qr.strip()}\n\n"
        "Keccak-256 (addons/scfg/core/keccak.gd)\n"
        "---------------------------------------\n"
        "A GDScript port of XKCP's CompactFIPS202 sponge (Gilles Van Assche, https://keccak.team/).\n"
        "To the extent possible under law, the implementer has waived all copyright and related\n"
        "or neighboring rights to the source code (CC0,\n"
        "http://creativecommons.org/publicdomain/zero/1.0/).\n"
    )


def godot_files_to_ship():
    """(path, arcname) for the Godot zip's files: addons/scfg/ at the root, plus the LICENSE.

    The tests, the demo and its e2e driver live outside addons/scfg; the editor cache and the
    *.uid files Godot 4.4+ regenerates are left out of it.
    """
    for path in sorted(GODOT_ADDON.rglob("*")):
        rel = path.relative_to(GODOT_ADDON)
        if path.is_dir() or GODOT_SKIP_PARTS & set(rel.parts) or path.suffix in GODOT_SKIP_SUFFIXES:
            continue
        yield path, ("addons/scfg/" + rel.as_posix())
    yield ROOT / "LICENSE", "scfg-LICENSE.txt"


def godot_addon_version():
    text = (GODOT_ADDON / "plugin.cfg").read_text(encoding="utf-8")
    return re.search(r'^version="([^"]+)"', text, re.M).group(1)


def build_godot_zip(dist):
    if godot_addon_version() != __version__:
        raise ValueError(f"plugin.cfg version {godot_addon_version()} != scfg_core {__version__}")
    dist.mkdir(exist_ok=True)
    target = dist / f"scfg-godot-{__version__}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, arcname in godot_files_to_ship():
            zf.write(path, arcname)
            print(f"  {arcname}")
        zf.writestr("scfg-README.md", godot_guide())
        print("  scfg-README.md")
        zf.writestr("scfg-NOTICES.txt", godot_notices())
        print("  scfg-NOTICES.txt")
    return target


def build_renpy_zip(dist):
    dist.mkdir(exist_ok=True)
    target = dist / f"scfg-renpy-{__version__}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, arcname in files_to_ship():
            zf.write(path, arcname)
            print(f"  {arcname}")
        zf.writestr("scfg-README.md", developer_guide())
        print("  scfg-README.md")
    return target


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Build a release zip into dist/.")
    parser.add_argument("--engine", choices=("renpy", "godot"), default="renpy")
    args = parser.parse_args(argv)
    dist = ROOT / "dist"
    build = build_godot_zip if args.engine == "godot" else build_renpy_zip
    print(f"wrote {build(dist)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
