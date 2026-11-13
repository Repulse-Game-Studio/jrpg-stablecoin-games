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


def test_godot_zip_ships_addons_scfg_with_its_license_inside():
    names = godot_names()
    assert "addons/scfg/plugin.cfg" in names
    assert "addons/scfg/scfg.gd" in names
    assert "addons/scfg/core/engine.gd" in names
    assert "addons/scfg/ui/sheet.tscn" in names
    assert "addons/scfg/web/app.js" in names
    assert "addons/scfg/translations/scfg.pot" in names
    assert "addons/scfg/LICENSE" in names
    assert not any(n.startswith("godot/") for n in names)


def test_godot_zip_leaves_out_tests_demo_and_editor_cache():
    for name in godot_names():
        parts = name.split("/")
        assert name.startswith("addons/scfg/"), name
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


def test_godot_guide_opens_with_the_landing_page_and_keeps_its_sections():
    """docs/developers/godot/README.md (the sidebar category's landing page) is the guide's
    intro; Install and the API reference stay top-level sections under the title."""
    guide = package.godot_guide()
    lines = guide.splitlines()
    assert lines[0] == "# Stablecoin for Games: Godot Developer Guide"
    assert "For Godot developers" not in guide  # the README's own title is replaced
    assert "[Install](#install-godot)" in guide and "[API reference](#api-reference-godot)" in guide
    tops = [h for h in lines if h.startswith("## ")]
    assert tops[:2] == ["## Install (Godot)", "## API reference (Godot)"], tops


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
        readme = zf.read(f"scfg-godot-{package.__version__}/addons/scfg/README.md").decode()
    assert readme == package.godot_guide()
    assert not any(n.endswith(".uid") or "/tests/" in n or ".godot" in n for n in names)


def test_godot_zip_installs_to_res_addons_scfg_from_the_asset_library(tmp_path):
    """One wrapper folder, addons/scfg/ under it and nothing else: the editor's installer strips
    the single top-level folder, so the addon lands in res://addons/scfg/ (which the addon
    hardcodes). A zip whose only top-level entry is addons/ would install to res://scfg/ on
    Godot 4.3 to 4.6, and files beside addons/ would land in the user's project root."""
    import zipfile

    target = package.build_godot_zip(tmp_path)
    top = f"scfg-godot-{package.__version__}/"
    with zipfile.ZipFile(target) as zf:
        names = zf.namelist()
    assert all(n.startswith(top + "addons/scfg/") for n in names), names
    for doc in ("README.md", "LICENSE", "NOTICES.txt", "plugin.cfg"):
        assert top + "addons/scfg/" + doc in names, doc
    assert len(names) == len(set(names))


# --- the RPG Maker MZ release zip --------------------------------------------------------------


def plugin_text(version):
    """What a built SCFG.js starts with (its header showing `version`), then an empty IIFE."""
    return ("/*:\n * @target MZ\n"
            f" * @plugindesc Stablecoin for Games (SCFG): payments v{version}\n"
            " * @author Repulse Game Studio\n *\n"
            f" * @help\n * Stablecoin for Games (SCFG) for RPG Maker MZ, v{version}.\n *\n"
            " * More help.\n */\n(() => {})();\n")


@pytest.fixture
def fake_plugin(tmp_path):
    """A stand-in for the built SCFG.js, so the layout tests need no Node."""
    path = tmp_path / "built" / "SCFG.js"
    path.parent.mkdir()
    path.write_text(plugin_text(package.__version__), encoding="utf-8")
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
    with pytest.raises(ValueError, match="0.2.0|!= scfg_core 9.9.9"):
        package.build_rpgmaker_zip(tmp_path / "dist", plugin=fake_plugin)
    assert not (tmp_path / "dist" / "scfg-rpgmaker-mz-9.9.9.zip").exists()


@pytest.mark.parametrize("where", ["plugindesc", "help"])
def test_rpgmaker_zip_refuses_a_header_that_shows_another_version(tmp_path, where):
    path = tmp_path / "SCFG.js"
    text = plugin_text(package.__version__)
    line = "@plugindesc" if where == "plugindesc" else "for RPG Maker MZ,"
    lines = [ln.replace(f"v{package.__version__}", "v9.9.9") if line in ln else ln for ln in text.splitlines()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="header"):
        package.build_rpgmaker_zip(tmp_path / "dist", plugin=path)
    assert not (tmp_path / "dist").exists() or not list((tmp_path / "dist").iterdir())


def test_rpgmaker_header_version_check_reads_only_the_header():
    v = package.__version__
    assert package.rpgmaker_header_problems(plugin_text(v), v) == []
    no_header = f"(() => {{ /* @plugindesc x v{v} */ }})();\n"
    assert package.rpgmaker_header_problems(no_header, v) != []
    # the version elsewhere in the bundle (VERSION = "...") does not count for the header
    bare = plugin_text("0.0.0") + f'const VERSION = "{v}";\n'
    assert len(package.rpgmaker_header_problems(bare, v)) == 2


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
    assert "not yet in real RPG Maker MZ" in guide
    assert guide.count("# For RPG Maker MZ developers") == 0  # the README is the intro, under the title
    anchors = {package.slug(h) for h in headings(guide)}
    for target in re.findall(r"\]\(([^)\s]+)\)", guide):
        if target.startswith(("http://", "https://")):
            continue  # a page on the web, such as the plugin's README on GitHub
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
    assert package.rpgmaker_header_problems(plugin, package.__version__) == []
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
