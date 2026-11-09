"""The coverage manifest: every Python unit test that the ports must twin is mapped."""

from __future__ import annotations

import json
import textwrap
from pathlib import Path

from scfg_conformance import coverage

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "conformance" / "vectors"
MANIFEST = ROOT / "conformance" / "coverage.json"
TIER1_FILES = ["keccak", "evm_addresses", "solana_base58", "solana_pda", "solana_tx", "amounts",
               "paylinks", "solana_paylinks", "page_messages", "transfers", "config",
               "config_solana", "custom_chains", "chains"]


def test_coverage_manifest():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert coverage.check(manifest, VECTORS) == []
    for name in TIER1_FILES:
        assert f"tests/unit/test_{name}.py" in manifest["files"]


def test_required_files_exist():
    assert len(coverage.REQUIRED_FILES) == 31
    for rel in coverage.REQUIRED_FILES:
        assert (ROOT / rel).is_file(), rel


def test_unlisted_reports_required_files_not_yet_mapped():
    assert coverage.unlisted({"files": list(coverage.REQUIRED_FILES)}) == []
    assert "tests/unit/test_engine.py" in coverage.unlisted({"files": []})


def write(root: Path, rel: str, text: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text), encoding="utf-8")
    return path


SAMPLE = """\
    import pytest

    @pytest.mark.parametrize("n", [1, 2, 3])
    def test_param(n):
        def test_nested():
            pass

    def helper():
        pass

    class TestThing:
        def test_a(self):
            pass

        def helper(self):
            pass

    class Other:
        def test_ignored(self):
            pass

    async def test_async():
        pass
"""


def test_test_ids_by_ast(tmp_path):
    path = write(tmp_path, "tests/unit/test_keccak.py", SAMPLE)
    assert coverage.test_ids(path, root=tmp_path) == [
        "tests/unit/test_keccak.py::test_param",
        "tests/unit/test_keccak.py::TestThing::test_a",
        "tests/unit/test_keccak.py::test_async",
    ]
    ids = coverage.test_ids("tests/unit/test_keccak.py")
    assert ids and all(i.startswith("tests/unit/test_keccak.py::test_") for i in ids)


def _tree(tmp_path):
    write(tmp_path, "tests/unit/test_keccak.py", "def test_a():\n    pass\n\n"
                                                 "def test_b():\n    pass\n")
    vectors = tmp_path / "conformance" / "vectors"
    write(vectors, "tier1/keccak.json", "{}\n")
    write(vectors, "tier2/x/flow.json", json.dumps({
        "tier": 2, "id": "x/flow", "covers": ["tests/unit/test_keccak.py::test_b"]}))
    return vectors


def test_check_accepts_a_consistent_manifest(tmp_path):
    vectors = _tree(tmp_path)
    manifest = {"files": ["tests/unit/test_keccak.py"], "tests": {
        "tests/unit/test_keccak.py::test_a": {"python_only": "wires a Python-only helper"},
        "tests/unit/test_keccak.py::test_b": ["tier1/keccak", "tier2/x/flow"]}}
    assert coverage.check(manifest, vectors, root=tmp_path) == []


def test_check_reports_problems(tmp_path):
    vectors = _tree(tmp_path)
    write(vectors, "tier2/x/other.json", json.dumps({
        "tier": 2, "id": "x/other", "covers": ["tests/unit/test_keccak.py::test_a"]}))
    for bad in ("tier1/Keccak.json", "tier3/keccak.json"):  # files exist; ids are malformed
        write(vectors, bad, "{}\n")
    manifest = {"files": ["tests/unit/test_keccak.py", "tests/unit/test_nope.py"], "tests": {
        "tests/unit/test_keccak.py::test_b": ["tier1/keccak", "tier1/missing",
                                              "tier1/../tier1/keccak", "tier3/keccak",
                                              "tier1/Keccak"],
        "tests/unit/test_keccak.py::test_gone": ["tier1/keccak"],
        "tests/unit/test_keccak.py::test_a": [],
        "tests/unit/test_chains.py::test_x": {"python_only": ""},
    }}
    problems = coverage.check(manifest, vectors, root=tmp_path)
    expected = [
        "not a required file: tests/unit/test_nope.py",
        "unknown test: tests/unit/test_keccak.py::test_gone",
        "unknown test: tests/unit/test_chains.py::test_x",
        "unknown vector tier1/missing for tests/unit/test_keccak.py::test_b",
        "unknown vector tier1/../tier1/keccak for tests/unit/test_keccak.py::test_b",
        "unknown vector tier3/keccak for tests/unit/test_keccak.py::test_b",
        "unknown vector tier1/Keccak for tests/unit/test_keccak.py::test_b",
        "empty mapping for tests/unit/test_keccak.py::test_a",
        "bad mapping for tests/unit/test_chains.py::test_x",
        "tier2/x/flow covers tests/unit/test_keccak.py::test_b but the manifest does not map "
        "it there",
        "tier2/x/other covers tests/unit/test_keccak.py::test_a but the manifest does not map "
        "it there",
    ]
    for line in expected:
        assert line in problems, (line, problems)
    assert problems == sorted(problems)


def test_check_reports_unmapped_tests_and_one_sided_tier2_mappings(tmp_path):
    vectors = _tree(tmp_path)
    write(vectors, "tier2/x/flow.json", json.dumps({"tier": 2, "id": "x/flow", "covers": []}))
    manifest = {"files": ["tests/unit/test_keccak.py"], "tests": {
        "tests/unit/test_keccak.py::test_b": ["tier2/x/flow"]}}
    assert coverage.check(manifest, vectors, root=tmp_path) == [
        "manifest maps tests/unit/test_keccak.py::test_b to tier2/x/flow, whose covers lack it",
        "unmapped: tests/unit/test_keccak.py::test_a",
    ]


def test_manifest_is_complete():
    """Every required unit-test file is listed, and a python_only entry says why."""
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert set(manifest["files"]) == set(coverage.REQUIRED_FILES)
    assert len(manifest["files"]) == len(coverage.REQUIRED_FILES)
    for test, mapping in manifest["tests"].items():
        if isinstance(mapping, dict):
            reason = mapping.get("python_only")
            assert isinstance(reason, str) and len(reason.strip()) > 15, test
