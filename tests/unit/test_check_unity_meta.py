"""tools/check_unity_meta.py: every file and folder of the Unity package has its .meta, and no .meta is orphaned."""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_unity_meta", ROOT / "tools" / "check_unity_meta.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def _meta(path: Path, guid: str) -> None:
    path.with_name(path.name + ".meta").write_text(f"fileFormatVersion: 2\nguid: {guid}\n")


def test_the_package_in_the_repo_is_complete():
    assert check.problems() == []


def test_missing_orphan_duplicate_and_skipped(tmp_path):
    pkg = tmp_path / "com.example.pkg"
    (pkg / "Runtime").mkdir(parents=True)
    _meta(pkg, "0" * 32)  # the package folder itself is not checked: only what is inside it
    _meta(pkg / "Runtime", "1" * 32)
    (pkg / "Runtime" / "A.cs").write_text("")
    _meta(pkg / "Runtime" / "A.cs", "2" * 32)
    (pkg / "Runtime" / "B.cs").write_text("")  # no .meta
    (pkg / "Runtime" / "Gone.cs.meta").write_text("guid: " + "3" * 32 + "\n")  # orphan
    (pkg / "Runtime" / "C.cs").write_text("")
    _meta(pkg / "Runtime" / "C.cs", "2" * 32)  # duplicate guid
    (pkg / "Samples~").mkdir()
    (pkg / "Samples~" / "D.cs").write_text("")  # skipped, as Unity skips it
    (pkg / ".hidden").write_text("")  # skipped
    found = check.problems(tmp_path)
    assert any(p.startswith("missing .meta:") and p.endswith("B.cs") for p in found)
    assert any(p.startswith("orphan .meta:") and p.endswith("Gone.cs.meta") for p in found)
    assert any(p.startswith("duplicate guid") for p in found)
    assert not any("Samples~" in p or ".hidden" in p for p in found)
    assert len(found) == 3
