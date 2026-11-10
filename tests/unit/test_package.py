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


# --- the RPG Maker MZ release zip --------------------------------------------------------------


@pytest.fixture
def fake_plugin(tmp_path):
    """A stand-in for the built SCFG.js, so the layout tests need no Node."""
    path = tmp_path / "built" / "SCFG.js"
    path.parent.mkdir()
    path.write_text("/*:\n * @target MZ\n */\n(() => {})();\n", encoding="utf-8")
    return path


def build_rpgmaker(tmp_path, plugin):
    import zipfile

    target = package.build_rpgmaker_zip(tmp_path / "dist", plugin=plugin)
    with zipfile.ZipFile(target) as zf:
        return target, zf.namelist(), {n: zf.read(n) for n in zf.namelist()}


def test_rpgmaker_zip_has_the_plugin_and_img_scfg_at_its_root(tmp_path, fake_plugin):
    target, names, contents = build_rpgmaker(tmp_path, fake_plugin)
    assert target.name == f"scfg-rpgmaker-mz-{package.__version__}.zip"
    assert sorted(names) == [
        "img/scfg/README.txt",
        "js/plugins/SCFG.js",
        "scfg-LICENSE.txt",
        "scfg-NOTICES.txt",
        "scfg-README.md",
    ]
    assert contents["js/plugins/SCFG.js"] == fake_plugin.read_bytes()
    assert "logo" in contents["img/scfg/README.txt"].decode().lower()


def test_rpgmaker_zip_refuses_a_plugin_version_that_differs_from_the_core(tmp_path, fake_plugin, monkeypatch):
    monkeypatch.setattr(package, "__version__", "9.9.9")
    with pytest.raises(ValueError, match="0.1.4|!= scfg_core 9.9.9"):
        package.build_rpgmaker_zip(tmp_path / "dist", plugin=fake_plugin)
    assert not (tmp_path / "dist" / "scfg-rpgmaker-mz-9.9.9.zip").exists()


def test_rpgmaker_plugin_version_is_read_from_the_source():
    assert package.rpgmaker_plugin_version() == package.__version__


def test_rpgmaker_zip_carries_the_vendored_notices():
    notice = package.rpgmaker_notices()
    assert "Project Nayuki" in notice
    assert "Permission is hereby granted" in notice
    assert "XKCP" in notice
    assert "js/plugins/SCFG.js" in notice


def test_rpgmaker_guide_is_joined_from_its_docs_and_says_it_is_unverified():
    guide = package.rpgmaker_guide()
    assert guide.startswith("# Stablecoin for Games: RPG Maker MZ Developer Guide")
    assert "not yet on real RPG Maker MZ" in guide
    anchors = {package.slug(h) for h in headings(guide)}
    for target in re.findall(r"\]\(([^)\s]+)\)", guide):
        assert ".md" not in target, target
        if target.startswith("#"):
            assert target[1:] in anchors, target


def test_the_other_guides_do_not_include_the_rpgmaker_pages():
    assert "SCFG.pay" not in package.developer_guide()
    assert "RPG Maker" not in package.godot_guide()
    assert "Scfg.configure" not in package.rpgmaker_guide()


def test_rpgmaker_zip_builds_the_real_plugin(tmp_path):
    import shutil
    import zipfile

    if not (package.ROOT / "rpgmaker" / "node_modules" / "esbuild").exists() or not shutil.which("npm"):
        pytest.skip("needs npm and rpgmaker/node_modules (cd rpgmaker && npm ci)")
    target = package.build_rpgmaker_zip(tmp_path)
    with zipfile.ZipFile(target) as zf:
        plugin = zf.read("js/plugins/SCFG.js").decode()
    assert plugin.startswith("/*:\n * @target MZ")
    assert "@plugindesc Stablecoin for Games (SCFG)" in plugin
    assert f'VERSION = "{package.__version__}"' in plugin


def test_rpgmaker_api_page_lists_every_text_id_with_its_english():
    import shutil

    if not shutil.which("node"):
        pytest.skip("needs node")
    spec = importlib.util.spec_from_file_location("check_text_ids", TOOL.with_name("check_text_ids.py"))
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    page = (package.DOCS / "developers" / "rpgmaker" / "api.md").read_text(encoding="utf-8")
    for text_id, english in check.default_texts().items():
        assert f"| `{text_id}` | {english.replace('|', chr(92) + '|')} |" in page, text_id
