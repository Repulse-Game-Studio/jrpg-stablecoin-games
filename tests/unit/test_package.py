import importlib.util
import re
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "package.py"
spec = importlib.util.spec_from_file_location("package", TOOL)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def headings(text):
    fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            fence = not fence
        elif not fence and line.startswith("#"):
            yield line


def test_developer_guide_links_resolve():
    guide = package.developer_guide()
    anchors = {package.slug(h) for h in headings(guide)}
    links = re.findall(r"\]\(([^)\s]+)\)", guide)
    assert links
    for target in links:
        assert not target.endswith(".md") and ".md#" not in target, target
        if target.startswith("#"):
            assert target[1:] in anchors, target


def test_developer_guide_has_every_page_in_order():
    guide = package.developer_guide()
    tops = [h for h in headings(guide) if h.startswith("## ")]
    assert tops[0] == "## Install"
    assert tops[-1] == "## How it works"
    assert "### Solana" in guide and "### Web builds" in guide


def test_link_outside_developer_guide_fails(tmp_path, monkeypatch):
    (tmp_path / "developers").mkdir()
    sidebars = tmp_path / "sidebars.json"
    sidebars.write_text('{"docs": ["developers/README"]}')
    (tmp_path / "developers" / "README.md").write_text("# Guide\n\nSee [paying](../players/paying.md).\n")
    monkeypatch.setattr(package, "DOCS", tmp_path)
    monkeypatch.setattr(package, "SIDEBARS", sidebars)
    with pytest.raises(ValueError, match="leaves the developer guide"):
        package.developer_guide()
