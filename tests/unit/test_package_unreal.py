"""tools/package_unreal.py: the Unreal release zips (tools/package.py --engine unreal).

The zips are built from a copy of unreal/Plugins/Scfg whose Scfg.uplugin VersionName is set to the
core's version (until the release bump the two differ, and the real tree is refused), with build
output planted in it to be stripped.
"""

import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import package_unreal  # noqa: E402

VERSION = package_unreal.__version__
ENGINES = ("5.6", "5.7", "5.8")
GENERATED = {"Scfg.uplugin", "Config/FilterPlugin.ini", "README.md", "LICENSE", "NOTICES.txt"}
BUILD_OUTPUT = ("Binaries", "Intermediate", "Saved", "DerivedDataCache")


def plugin_copy(dest, version=VERSION, core_version=None):
    """A copy of the plugin with its VersionName set to `version` and scfg::VERSION (Version.cpp)
    to `core_version` (default: `version`), and build output in it (at the plugin's root and in a
    module folder, as UBT leaves it) and a hidden file."""
    plugin = dest / "Scfg"
    shutil.copytree(package_unreal.PLUGIN, plugin,
                    ignore=shutil.ignore_patterns(*BUILD_OUTPUT))
    descriptor = plugin / "Scfg.uplugin"
    text = descriptor.read_text(encoding="utf-8")
    text = re.sub(r'"VersionName": "[^"]*"', f'"VersionName": "{version}"', text)
    descriptor.write_text(text, encoding="utf-8")
    constant = plugin / "Source" / "ScfgCore" / "Private" / "Version.cpp"
    text = constant.read_text(encoding="utf-8")
    text, count = re.subn(r'(VERSION = )"[^"]*"', rf'\g<1>"{core_version or version}"', text)
    assert count == 1
    constant.write_text(text, encoding="utf-8")
    for where in (plugin, plugin / "Source" / "Scfg"):
        for folder in BUILD_OUTPUT:
            (where / folder / "Linux").mkdir(parents=True)
            (where / folder / "Linux" / "junk.o").write_bytes(b"\0")
    (plugin / "Resources" / "Web" / ".DS_Store").write_bytes(b"\0")
    return plugin


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("unreal-zips")
    plugin = plugin_copy(tmp)
    targets = package_unreal.build_unreal_zips(tmp / "dist", plugin)
    return plugin, targets


def read_zip(target):
    with zipfile.ZipFile(target) as zf:
        return {name: zf.read(name) for name in zf.namelist()}


def test_one_zip_per_engine_version_named_after_the_core_version(built):
    _plugin, targets = built
    assert [t.name for t in targets] == [f"scfg-unreal-{VERSION}-ue{e}.zip" for e in ENGINES]


def test_each_zip_holds_the_plugin_folder_scfg_in_fabs_source_layout(built):
    _plugin, targets = built
    for target in targets:
        names = list(read_zip(target))
        assert all(n.startswith("Scfg/") for n in names), names
        rel = [n[len("Scfg/"):] for n in names]
        tops = {r.split("/")[0] for r in rel}
        assert tops == {"Scfg.uplugin", "Source", "Resources", "Config", "README.md", "LICENSE",
                        "NOTICES.txt"}, tops
        modules = {r.split("/")[1] for r in rel if r.startswith("Source/")}
        assert modules == {"ScfgCore", "Scfg", "ScfgEditor"}, modules
        for name in ("Source/ScfgCore/ScfgCore.Build.cs", "Source/ScfgCore/Public/Scfg/Engine.h",
                     "Source/Scfg/Scfg.Build.cs", "Source/Scfg/Public/ScfgSubsystem.h",
                     "Source/ScfgEditor/ScfgEditor.Build.cs", "Resources/Web/index.html",
                     "Resources/Web/app.js", "Config/FilterPlugin.ini"):
            assert name in rel, name


def test_the_zips_ship_the_page_the_server_serves_and_not_bridge_js(built):
    """Resources/Web/ in the zip is what FScfgLocalServer serves and Scfg.Build.cs stages: the
    page, app.js, evm.js and solana.js. bridge.js (the web exports' bridge, kept in the tree by
    sync_web_page.py) is never served or staged, so it does not ship (final review F10)."""
    plugin, targets = built
    assert (plugin / "Resources" / "Web" / "bridge.js").is_file()
    for target in targets:
        web = sorted(n for n in read_zip(target) if n.startswith("Scfg/Resources/Web/"))
        assert web == [f"Scfg/Resources/Web/{f}"
                       for f in ("app.js", "evm.js", "index.html", "solana.js")], web


def test_zips_hold_no_test_module_core_tests_or_build_output(built):
    """The test modules and their .uplugin entries are stripped (phase 2 R2-6 R7, phase 4 P4R-1):
    ScfgTests' store hook must not reach a developer's project. No Binaries/ or Intermediate/
    (Fab 4.3.7.3.a), nothing of unreal/core-tests/, no hidden files."""
    _plugin, targets = built
    for target in targets:
        files = read_zip(target)
        for name in files:
            parts = name.split("/")
            assert not {"ScfgTests", "ScfgCoreTests", "core-tests"} & set(parts), name
            assert not set(BUILD_OUTPUT) & set(parts), name
            assert not any(p.startswith(".") for p in parts), name
        uplugin = files["Scfg/Scfg.uplugin"].decode()
        assert "ScfgTests" not in uplugin and "ScfgCoreTests" not in uplugin
        assert "Tests" not in uplugin


def test_each_zips_uplugin_declares_its_engine_version_and_the_core_version(built):
    plugin, targets = built
    source = json.loads((plugin / "Scfg.uplugin").read_text(encoding="utf-8"))
    for engine, target in zip(ENGINES, targets):
        packaged = json.loads(read_zip(target)["Scfg/Scfg.uplugin"])
        assert packaged["EngineVersion"] == f"{engine}.0"
        assert packaged["VersionName"] == VERSION
        assert [m["Name"] for m in packaged["Modules"]] == ["ScfgCore", "Scfg", "ScfgEditor"]
        kept = [m for m in source["Modules"] if m["Name"] in ("ScfgCore", "Scfg", "ScfgEditor")]
        assert packaged["Modules"] == kept
        rest = {k: v for k, v in packaged.items() if k not in ("EngineVersion", "Modules")}
        assert rest == {k: v for k, v in source.items() if k not in ("EngineVersion", "Modules")}


def test_the_zips_differ_only_in_the_uplugin(built):
    _plugin, targets = built
    first, *others = [read_zip(t) for t in targets]
    for other in others:
        assert other.keys() == first.keys()
        assert [n for n in first if first[n] != other[n]] == ["Scfg/Scfg.uplugin"]


def test_every_source_and_resource_file_ships_byte_for_byte(built):
    """The packager copies files as they are (it does not care about their headers; the copyright
    lines of task 5.3 pass through unchanged), and leaves none of the shipped folders out."""
    plugin, targets = built
    expected = {}
    for folder in ("Source/ScfgCore", "Source/Scfg", "Source/ScfgEditor", "Resources"):
        for path in (plugin / folder).rglob("*"):
            rel = path.relative_to(plugin)
            if rel.as_posix() == "Resources/Web/bridge.js":
                continue
            if path.is_file() and not set(BUILD_OUTPUT) & set(rel.parts) and path.name[0] != ".":
                expected["Scfg/" + rel.as_posix()] = path.read_bytes()
    assert len(expected) > 150
    for target in targets:
        files = read_zip(target)
        shipped = {n: d for n, d in files.items() if n[len("Scfg/"):] not in GENERATED}
        assert shipped == expected


def test_readme_license_notices_and_the_filter_that_keeps_them(built):
    _plugin, targets = built
    files = read_zip(targets[-1])
    assert files["Scfg/README.md"].decode() == package_unreal.guide()
    assert files["Scfg/README.md"].decode().startswith(
        "# Stablecoin for Games: Unreal Developer Guide\n")
    assert files["Scfg/LICENSE"] == (ROOT / "LICENSE").read_bytes()
    notices = files["Scfg/NOTICES.txt"].decode()
    assert "Copyright (c) Project Nayuki. (MIT License)" in notices
    assert "Permission is hereby granted" in notices and "noninfringement" in notices
    assert "//" not in notices.replace("https://", "").replace("http://", "")
    assert "XKCP" in notices and "CC0" in notices
    rules = files["Scfg/Config/FilterPlugin.ini"].decode().splitlines()
    assert rules[0] == "[FilterPlugin]"
    assert [r for r in rules[1:] if not r.startswith(";")] == ["/README.md", "/LICENSE",
                                                               "/NOTICES.txt"]


def test_every_path_is_at_most_170_characters(built):
    _plugin, targets = built
    for target in targets:
        assert all(len(n) <= 170 for n in read_zip(target))
    assert package_unreal.path_problems(["S" * 170]) == []
    assert package_unreal.path_problems(["S" * 171]) == [
        f"{'S' * 171} is longer than 170 characters (171)"]


def test_a_path_over_170_characters_is_refused(tmp_path):
    plugin = plugin_copy(tmp_path)
    prefix = len("Scfg/Source/Scfg/Private/")
    (plugin / "Source" / "Scfg" / "Private" / ("L" * (171 - prefix - 2) + ".h")).write_text("")
    with pytest.raises(ValueError, match="Fab allows paths of 170 characters at most"):
        package_unreal.build_unreal_zips(tmp_path / "dist", plugin)
    assert not (tmp_path / "dist").exists() or not any((tmp_path / "dist").iterdir())


def test_a_uplugin_version_that_differs_from_the_core_is_refused(tmp_path):
    plugin = plugin_copy(tmp_path, version="9.9.9")
    with pytest.raises(ValueError, match=f"VersionName 9.9.9 != scfg_core {re.escape(VERSION)}"):
        package_unreal.build_unreal_zips(tmp_path / "dist", plugin)
    assert not (tmp_path / "dist").exists()


def test_a_core_version_constant_that_differs_from_the_uplugin_is_refused(tmp_path):
    """scfg::VERSION (Private/Version.cpp, exported by the core) ships in the zip too: it must say
    what the .uplugin says (final review F4)."""
    plugin = plugin_copy(tmp_path, core_version="9.9.8")
    with pytest.raises(ValueError,
                       match=f"scfg::VERSION 9.9.8 [(]Version.cpp[)] != Scfg.uplugin VersionName "
                             f"{re.escape(VERSION)}"):
        package_unreal.build_unreal_zips(tmp_path / "dist", plugin)
    assert not (tmp_path / "dist").exists()


def test_a_version_cpp_without_the_constant_is_refused(tmp_path):
    plugin = plugin_copy(tmp_path)
    (plugin / "Source" / "ScfgCore" / "Private" / "Version.cpp").write_text("// nothing\n")
    with pytest.raises(ValueError, match="no scfg::VERSION literal in Source/ScfgCore/Private/Version.cpp"):
        package_unreal.build_unreal_zips(tmp_path / "dist", plugin)


def test_the_real_plugins_version_constant_matches_its_uplugin():
    """In the tree (before and after the release bump) the two versions the zip carries agree."""
    assert package_unreal.core_constant_version() == package_unreal.descriptor()["VersionName"]


def test_main_names_the_version_mismatch_and_fails(tmp_path, capsys):
    plugin = plugin_copy(tmp_path, version="9.9.9")
    assert package_unreal.main(tmp_path / "dist", plugin) == 1
    err = capsys.readouterr().err
    assert f"Scfg.uplugin VersionName 9.9.9 != scfg_core {VERSION}" in err


def test_a_module_the_packager_does_not_know_is_refused(tmp_path):
    plugin = plugin_copy(tmp_path)
    (plugin / "Source" / "ScfgExtra").mkdir()
    with pytest.raises(ValueError, match="Source/ScfgExtra is neither a shipped nor a test module"):
        package_unreal.build_unreal_zips(tmp_path / "dist", plugin)


def test_the_real_plugin_has_only_modules_the_packager_knows():
    """Every module of unreal/Plugins/Scfg is shipped or stripped (the version is the release
    bump's: checked separately)."""
    version = package_unreal.descriptor()["VersionName"]
    assert package_unreal.problems(version=version) == []


def test_the_zips_are_reproducible(tmp_path, built):
    plugin, targets = built
    again = package_unreal.build_unreal_zips(tmp_path / "again", plugin)
    for first, second in zip(targets, again):
        assert first.read_bytes() == second.read_bytes()
        with zipfile.ZipFile(first) as zf:
            infos = zf.infolist()
        assert [i.filename for i in infos] == sorted(i.filename for i in infos)
        assert {i.date_time for i in infos} == {(1980, 1, 1, 0, 0, 0)}
