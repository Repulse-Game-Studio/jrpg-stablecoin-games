"""tools/check_unity_samples.py without Unity: package.json's samples and the .meta files."""

import importlib.util
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "check_unity_samples", ROOT / "tools" / "check_unity_samples.py"
)
samples = importlib.util.module_from_spec(spec)
spec.loader.exec_module(samples)


def test_the_packages_samples_are_listed_and_have_their_meta_files():
    assert samples.problems() == []
    assert [s["path"] for s in samples.listed()] == ["Samples~/Showcase", "Samples~/Demo"]


def test_each_sample_has_a_scene_the_check_opens():
    for name in ("Showcase", "Demo"):
        assert (samples.SAMPLES / name / f"{name}.unity").is_file()
    check = (ROOT / "unity" / "Project" / "Assets" / "Maintenance" / "Editor" / "ScfgSamplesCheck.cs")
    text = check.read_text()
    assert '("Showcase", "Scfg.Samples.Showcase.ScfgShowcase")' in text
    assert '("Demo", "Scfg.Samples.Demo.ScfgDemo")' in text


def test_problems_finds_unlisted_folders_missing_meta_and_bad_entries(tmp_path):
    pkg = tmp_path / "pkg"
    shutil.copytree(samples.PACKAGE / "Samples~", pkg / "Samples~")
    manifest = {"samples": [
        {"displayName": "Showcase", "description": "d", "path": "Samples~/Showcase"},
        {"displayName": "Gone", "description": "", "path": "Samples~/Gone"},
    ]}
    (pkg / "package.json").write_text(json.dumps(manifest))
    (pkg / "Samples~" / "Demo" / "ScfgDemo.cs.meta").unlink()
    (pkg / "Samples~" / "Showcase" / "Orphan.txt.meta").write_text("guid: x\n")
    found = samples.problems(pkg / "Samples~", pkg / "package.json")
    assert "Samples~/Demo is not listed in package.json's samples" in found
    assert "package.json lists Samples~/Gone, which does not exist" in found
    assert any("without description" in p for p in found)
    assert "missing .meta: Samples~/Demo/ScfgDemo.cs" in found
    assert "orphan .meta: Samples~/Showcase/Orphan.txt.meta" in found
