"""Build the drop-in release zip: dist/scfg-renpy-<version>.zip

The zip mirrors a Ren'Py project, so developers unzip it straight into their
project folder:

    game/scfg/...                      (.rpy files + browser checkout page)
    game/python-packages/scfg_core/... (engine-independent core)
    scfg-README.md                     (developer guide, joined from docs/developers/)
    scfg-LICENSE.txt                   (MIT license)

Usage: uv run python tools/package.py
"""

from __future__ import annotations

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


def developer_guide():
    """Join the docs/developers/ pages, in website/sidebars.json order, into one file.

    Each page's headings move down one level, plus one per level of nesting in the
    sidebar, and links between pages become anchors in the joined file. A link to a
    page outside docs/developers/ fails.
    """
    sidebar = json.loads(SIDEBARS.read_text(encoding="utf-8"))["docs"]
    depth = {p: level for p, level in sidebar_pages(sidebar) if p.startswith("developers/")}
    pages = list(depth)
    # The first page is the guide's intro and the rest sit under it; count levels from them.
    base = min((depth[p] for p in pages[1:]), default=1) - 1
    depth = {p: max(1, level - base) for p, level in depth.items()}
    texts = {p: (DOCS / p).read_text(encoding="utf-8") for p in pages}
    tops = {p: "#" + slug(texts[p].splitlines()[0]) for p in pages}
    tops[pages[0]] = "#" + slug(GUIDE_TITLE)

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

    out = [GUIDE_TITLE]
    for page in pages:
        lines = texts[page].rstrip("\n").split("\n")
        if page == pages[0]:
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


def main() -> int:
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    target = dist / f"scfg-renpy-{__version__}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, arcname in files_to_ship():
            zf.write(path, arcname)
            print(f"  {arcname}")
        zf.writestr("scfg-README.md", developer_guide())
        print("  scfg-README.md")
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
