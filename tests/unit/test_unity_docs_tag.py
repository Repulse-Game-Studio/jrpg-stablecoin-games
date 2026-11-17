"""The release tag the Unity docs pin in the git install URL (`...com.repulse.scfg#v0.3.0`).

The Unity package first ships in release 0.3.0, while the core may still be on an earlier version: the tag is right
when it is `v` + the semver of the core's version, or of a later one (the release the package will ship in). It has
to move when the core's version passes it, and every page has to name the same tag.
"""

import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "package.py"
spec = importlib.util.spec_from_file_location("package", TOOL)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

PAGES = [
    *sorted((ROOT / "docs" / "developers" / "unity").glob("*.md")),
    *sorted((ROOT / "unity" / "Packages" / "com.repulse.scfg").rglob("*.md")),
]
TAG = re.compile(r"com\.repulse\.scfg#v(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)\b|`#v(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)`")


def order(version):
    """A semver as a sortable key: 0.3.0-rc.1 < 0.3.0 < 0.3.1."""
    core, _, pre = version.partition("-")
    numbers = tuple(int(n) for n in core.split("."))
    if not pre:
        return (*numbers, 1, ())
    return (*numbers, 0, tuple(int(p) if p.isdigit() else p for p in pre.split(".")))


def tags():
    found = {}
    for page in PAGES:
        for m in TAG.finditer(page.read_text(encoding="utf-8")):
            found.setdefault(m.group(1) or m.group(2), []).append(page.relative_to(ROOT).as_posix())
    return found


def test_order_of_versions():
    assert order("0.3.0-rc.1") < order("0.3.0") < order("0.3.1") < order("0.10.0")
    assert order("0.3.0-alpha.2") < order("0.3.0-beta.1") < order("0.3.0-rc.1")


def test_the_install_page_pins_a_release_tag():
    found = tags()
    assert any("docs/developers/unity/install.md" in pages for pages in found.values()), found


def test_every_unity_page_names_the_same_tag():
    assert len(tags()) == 1, tags()


def test_the_tag_is_the_core_version_or_a_later_one():
    core = package.semver(package.__version__)
    for tag, pages in tags().items():
        assert order(tag) >= order(core), f"v{tag} in {pages} is older than the core's version {core}: pin v{core}"
