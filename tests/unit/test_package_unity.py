"""package.py --engine unity: what the tarball ships and what unity_problems checks in a copy of
the package (test_package.py has the rest)."""

import importlib.util
import shutil
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "package.py"
spec = importlib.util.spec_from_file_location("package_unity", TOOL)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

WEB = Path("Runtime") / "Unity" / "Resources" / "ScfgWeb"


def copy_package(tmp_path):
    copy = tmp_path / "Packages" / "com.repulse.scfg"
    shutil.copytree(package.UNITY_PACKAGE, copy)
    return copy


def page_problems(found):
    return [p for p in found if p.startswith("checkout page copy:")]


def test_editor_backups_are_not_shipped_but_the_tilde_folders_are(tmp_path):
    copy = copy_package(tmp_path)
    (copy / "Runtime" / "Unity" / "SCFG.cs~").write_text("backup")
    (copy / "Samples~" / "Showcase" / "ScfgShowcase.cs~").write_text("backup")
    names = [name for _, name in package.unity_files_to_ship(copy)]
    assert not [n for n in names if n.endswith("~")]
    assert "package/Samples~/Showcase/ScfgShowcase.cs" in names
    assert "package/Documentation~/index.md" in names


def test_the_page_copy_is_checked_in_the_package_given(tmp_path):
    copy = copy_package(tmp_path)
    assert page_problems(package.unity_problems(copy)) == []
    (copy / WEB / "bridge.js.bytes").write_bytes(b"drift")
    found = page_problems(package.unity_problems(copy))
    assert len(found) == 1 and "drifted" in found[0] and str(copy / WEB) in found[0], found


def version_problems(found):
    return [p for p in found if "PackageVersion.Text" in p]


def test_the_csharp_version_constant_must_be_the_cores(tmp_path):
    copy = copy_package(tmp_path)
    assert package.unity_version_constant(copy) == package.semver(package.__version__)
    assert version_problems(package.unity_problems(copy)) == []
    source = copy / "Runtime" / "Core" / "PackageVersion.cs"
    source.write_text(source.read_text().replace(package.semver(package.__version__), "9.9.9"))
    found = version_problems(package.unity_problems(copy))
    assert len(found) == 1 and "9.9.9" in found[0], found
    source.unlink()
    assert "None" in version_problems(package.unity_problems(copy))[0]


def notices_problems(found):
    return [p for p in found if package.UNITY_NOTICES_NAME in p]


def test_the_notices_file_is_current_and_shipped(tmp_path):
    copy = copy_package(tmp_path)
    assert notices_problems(package.unity_problems(copy)) == []
    text = (copy / package.UNITY_NOTICES_NAME).read_text(encoding="utf-8")
    assert "Copyright (c) Project Nayuki. (MIT License)" in text and "CC0" in text
    names = [name for _, name in package.unity_files_to_ship(copy)]
    assert "package/" + package.UNITY_NOTICES_NAME in names
    assert "package/" + package.UNITY_NOTICES_NAME + ".meta" in names
    (copy / package.UNITY_NOTICES_NAME).write_text(text.replace("Nayuki", "Someone"), encoding="utf-8")
    assert len(notices_problems(package.unity_problems(copy))) == 1


def test_the_changelog_copy_must_be_the_repositorys(tmp_path):
    copy = copy_package(tmp_path)
    assert not [p for p in package.unity_problems(copy) if "CHANGELOG" in p]
    (copy / "CHANGELOG.md").write_text("# Changelog\n\n## 0.0.1\n")
    found = [p for p in package.unity_problems(copy) if "CHANGELOG" in p]
    assert found == ["CHANGELOG.md is not a copy of the repository's CHANGELOG.md: "
                     "uv run python tools/check_unity_changelog.py --write"]
    (copy / "CHANGELOG.md").unlink()
    (copy / "CHANGELOG.md.meta").unlink()
    found = [p for p in package.unity_problems(copy) if "CHANGELOG" in p]
    assert "CHANGELOG.md is missing: uv run python tools/check_unity_changelog.py --write" in found
