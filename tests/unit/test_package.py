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


# --- the Godot release zip -------------------------------------------------------------------


def godot_names():
    return [arcname for _path, arcname in package.godot_files_to_ship()]


def test_godot_zip_has_addons_scfg_at_its_root():
    names = godot_names()
    assert "addons/scfg/plugin.cfg" in names
    assert "addons/scfg/scfg.gd" in names
    assert "addons/scfg/core/engine.gd" in names
    assert "addons/scfg/ui/sheet.tscn" in names
    assert "addons/scfg/web/app.js" in names
    assert "addons/scfg/translations/scfg.pot" in names
    assert "scfg-LICENSE.txt" in names
    assert not any(n.startswith("godot/") for n in names)


def test_godot_zip_leaves_out_tests_demo_and_editor_cache():
    for name in godot_names():
        parts = name.split("/")
        assert name.startswith("addons/scfg/") or name == "scfg-LICENSE.txt", name
        assert not name.endswith(".uid"), name
        assert not {".godot", "tests", "demo", "e2e", "__pycache__"} & set(parts), name
        assert "e2e_driver" not in name, name


def test_godot_zip_carries_the_vendored_notice_and_a_readme():
    notice = package.godot_notices()
    assert "Project Nayuki" in notice
    assert "Permission is hereby granted" in notice
    assert "XKCP" in notice
    guide = package.godot_guide()
    assert guide.startswith("# Stablecoin for Games: Godot Developer Guide")
    anchors = {package.slug(h) for h in headings(guide)}
    for target in re.findall(r"\]\(([^)\s]+)\)", guide):
        assert ".md" not in target, target
        if target.startswith("#"):
            assert target[1:] in anchors, target


def test_renpy_developer_guide_does_not_include_the_godot_pages():
    guide = package.developer_guide()
    assert "Godot" not in guide.splitlines()[0]
    assert "Scfg.configure" not in guide
    assert "res://" not in guide


def test_build_godot_zip_writes_the_versioned_archive(tmp_path):
    target = package.build_godot_zip(tmp_path)
    assert target.name == f"scfg-godot-{package.__version__}.zip"
    import zipfile

    with zipfile.ZipFile(target) as zf:
        names = zf.namelist()
    assert "scfg-README.md" in names and "scfg-NOTICES.txt" in names
    assert "addons/scfg/plugin.cfg" in names
    assert not any(n.endswith(".uid") or "/tests/" in n or ".godot" in n for n in names)
