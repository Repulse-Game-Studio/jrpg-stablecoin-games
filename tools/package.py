"""Build the drop-in release zips: dist/scfg-renpy-<version>.zip (default),
dist/scfg-godot-<version>.zip (--engine godot) and dist/scfg-rpgmaker-mz-<version>.zip
(--engine rpgmaker).

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

The RPG Maker MZ zip unzips into an MZ project folder:

    js/plugins/SCFG.js                 (the whole plugin, built by `npm run build` in rpgmaker/)
    img/scfg/README.txt                (where the logo goes)
    scfg-README.md                     (RPG Maker guide, joined from docs/developers/rpgmaker/)
    scfg-LICENSE.txt                   (MIT license)
    scfg-NOTICES.txt                   (notices of the third-party code SCFG.js carries)

Usage: uv run python tools/package.py [--engine renpy|godot|rpgmaker]
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
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
RPGMAKER_PAGES = "developers/rpgmaker/"
RPGMAKER_GUIDE_TITLE = "# Stablecoin for Games: RPG Maker MZ Developer Guide"
RPGMAKER_DIR = ROOT / "rpgmaker"
RPGMAKER_VERSION_SOURCE = RPGMAKER_DIR / "src" / "mz" / "scfg.ts"
RPGMAKER_VENDORED_QR = RPGMAKER_DIR / "src" / "core" / "vendor" / "qrcodegen.js"
RPGMAKER_LOGO_NOTE = """Put your logo here
==================

Copy your logo (a png, jpg or webp file) into this folder, for example img/scfg/logo.png.
Then, in the Plugin Manager, set SCFG's Logo parameter to that file. It is shown on the
payment sheet and on the browser checkout page.

Leave the Logo parameter empty if you have no logo; this folder can then stay as it is.
"""
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


def developer_guide(
    prefix="developers/", skip=(GODOT_PAGES, RPGMAKER_PAGES), title=GUIDE_TITLE, intro=True
):
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


def rpgmaker_guide():
    """The RPG Maker MZ developer guide (docs/developers/rpgmaker/), joined like the others."""
    return developer_guide(RPGMAKER_PAGES, (), RPGMAKER_GUIDE_TITLE, intro=False)


def rpgmaker_plugin_version():
    """The version SCFG.js reports (`SCFG.version`): the VERSION constant of src/mz/scfg.ts.

    The plugin header (the MZ comment blocks) carries no version of its own.
    """
    text = RPGMAKER_VERSION_SOURCE.read_text(encoding="utf-8")
    return re.search(r'^export const VERSION = "([^"]+)"', text, re.M).group(1)


def rpgmaker_notices():
    """The notices of the code SCFG.js vendors or ports, for scfg-NOTICES.txt.

    The Nayuki notice is read from the vendored qrcodegen.js, where it is kept unmodified, so
    the two cannot drift.
    """
    text = RPGMAKER_VENDORED_QR.read_text(encoding="utf-8")
    block = text[text.index("/*") + 2 : text.index("*/")]
    qr = "\n".join(line.strip().removeprefix("*").strip() for line in block.splitlines())
    qr = re.sub(r"\n{3,}", "\n\n", qr).strip()
    return (
        "Third-party code in js/plugins/SCFG.js\n"
        "======================================\n\n"
        "QR Code generator\n"
        "-----------------\n"
        "Project Nayuki's QR Code generator library (TypeScript), compiled to JavaScript and\n"
        "included unmodified. The original notice follows.\n\n"
        f"{qr}\n\n"
        "Keccak-256\n"
        "----------\n"
        "A TypeScript port of the Keccak team's compact reference sponge (XKCP's CompactFIPS202,\n"
        "Gilles Van Assche, https://keccak.team/). To the extent possible under law, the\n"
        "implementer has waived all copyright and related or neighboring rights to the source\n"
        "code (CC0, http://creativecommons.org/publicdomain/zero/1.0/).\n"
    )


def build_rpgmaker_plugin(outdir):
    """Run `npm run build` in rpgmaker/ into `outdir` (after `npm ci`); the plugin file."""
    npm = shutil.which("npm")
    if npm is None:
        raise RuntimeError("npm not found: SCFG.js is built with Node (cd rpgmaker && npm ci)")
    if not (RPGMAKER_DIR / "node_modules" / "esbuild").exists():
        raise RuntimeError("rpgmaker/node_modules is missing: run `cd rpgmaker && npm ci` first")
    command = [npm, "run", "build", "--", "--outdir", str(outdir)]
    subprocess.run(command, cwd=RPGMAKER_DIR, check=True)
    return Path(outdir) / "js" / "plugins" / "SCFG.js"


def build_rpgmaker_zip(dist, plugin=None):
    """dist/scfg-rpgmaker-mz-<version>.zip. `plugin` is a built SCFG.js (default: build one)."""
    if rpgmaker_plugin_version() != __version__:
        raise ValueError(f"SCFG.js version {rpgmaker_plugin_version()} != scfg_core {__version__}")
    with tempfile.TemporaryDirectory(prefix="scfg-rpgmaker-") as tmp:
        if plugin is None:
            plugin = build_rpgmaker_plugin(tmp)
        dist.mkdir(parents=True, exist_ok=True)
        target = dist / f"scfg-rpgmaker-mz-{__version__}.zip"
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(plugin, "js/plugins/SCFG.js")
            print("  js/plugins/SCFG.js")
            zf.writestr("img/scfg/README.txt", RPGMAKER_LOGO_NOTE)
            print("  img/scfg/README.txt")
            zf.write(ROOT / "LICENSE", "scfg-LICENSE.txt")
            print("  scfg-LICENSE.txt")
            zf.writestr("scfg-README.md", rpgmaker_guide())
            print("  scfg-README.md")
            zf.writestr("scfg-NOTICES.txt", rpgmaker_notices())
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
    parser.add_argument("--engine", choices=("renpy", "godot", "rpgmaker"), default="renpy")
    args = parser.parse_args(argv)
    dist = ROOT / "dist"
    builders = {"renpy": build_renpy_zip, "godot": build_godot_zip, "rpgmaker": build_rpgmaker_zip}
    build = builders[args.engine]
    print(f"wrote {build(dist)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
