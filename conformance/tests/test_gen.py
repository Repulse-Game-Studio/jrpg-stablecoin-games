"""Vector writer, staleness checker and the gen_vectors CLI."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

from scfg_conformance import vectors

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def fake_sources(monkeypatch):
    sources = {
        "tier1/alpha.json": lambda: {"fn": "alpha", "cases": [{"in": 1, "out": "é"}]},
        "tier1/beta.json": lambda: {"fn": "beta", "cases": []},
    }
    monkeypatch.setattr(vectors, "SOURCES", sources)
    monkeypatch.setattr(vectors, "load_all", lambda: None)
    return sources


def _snapshot(root: Path) -> dict:
    return {
        p.relative_to(root).as_posix(): p.read_bytes()
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


def test_generation_is_deterministic(tmp_path, fake_sources):
    a, b = tmp_path / "a", tmp_path / "b"
    pa = vectors.write_all(a)
    vectors.write_all(b)
    assert [p.relative_to(a).as_posix() for p in pa] == ["tier1/alpha.json", "tier1/beta.json"]
    assert _snapshot(a) == _snapshot(b)
    assert set(_snapshot(a)) == set(fake_sources)


def test_check_clean_after_write(tmp_path, fake_sources):
    vectors.write_all(tmp_path)
    assert vectors.check(tmp_path) == []


def test_check_reports_stale_missing_extra(tmp_path, fake_sources):
    vectors.write_all(tmp_path)
    (tmp_path / "tier1/alpha.json").write_text("{}\n", encoding="utf-8")
    (tmp_path / "tier1/beta.json").unlink()
    (tmp_path / "tier1/gamma.json").write_text("{}\n", encoding="utf-8")
    (tmp_path / "notes.txt").write_text("ignored", encoding="utf-8")
    assert vectors.check(tmp_path) == [
        "extra: tier1/gamma.json",
        "missing: tier1/beta.json",
        "stale: tier1/alpha.json",
    ]


def test_check_with_empty_sources_tolerates_absent_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(vectors, "SOURCES", {})
    monkeypatch.setattr(vectors, "load_all", lambda: None)
    assert vectors.check(tmp_path / "nope") == []
    assert vectors.check(tmp_path) == []


def test_check_absent_dir_reports_missing(tmp_path, fake_sources):
    assert vectors.check(tmp_path / "nope") == [
        "missing: tier1/alpha.json",
        "missing: tier1/beta.json",
    ]


def _load_cli():
    path = ROOT / "tools" / "gen_vectors.py"
    spec = importlib.util.spec_from_file_location("gen_vectors_cli", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_cli_write_then_check_then_stale(tmp_path, fake_sources, capsys):
    cli = _load_cli()
    out = str(tmp_path / "vectors")
    assert cli.main(["--out", out, "--check"]) == 1  # nothing written yet
    capsys.readouterr()
    assert cli.main(["--out", out]) == 0
    assert "wrote tier1/alpha.json" in capsys.readouterr().out
    assert cli.main(["--out", out, "--check"]) == 0
    assert capsys.readouterr().out == ""
    (tmp_path / "vectors" / "tier1" / "alpha.json").write_text("{}\n", encoding="utf-8")
    assert cli.main(["--out", out, "--check"]) == 1
    captured = capsys.readouterr()
    assert "stale: tier1/alpha.json" in captured.out
    assert "out of date" in captured.err and "delete" in captured.err


def test_cli_runs_as_script(tmp_path):
    script = str(ROOT / "tools" / "gen_vectors.py")
    r = subprocess.run([sys.executable, script, "--out", str(tmp_path), "--check"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode in (0, 1), r.stdout + r.stderr  # starts and exits cleanly
    assert "Traceback" not in r.stderr


def test_tier2_generation_is_deterministic(tmp_path, monkeypatch):
    """Tier 2 scenarios regenerate byte-identically, in-process and in a fresh interpreter
    with a different hash seed (set and dict ordering must never leak into a vector)."""
    from scfg_conformance import tier2

    assert tier2.SOURCES
    monkeypatch.setattr(vectors, "SOURCES", dict(tier2.SOURCES))
    a, b = tmp_path / "a", tmp_path / "b"
    vectors.write_all(a)
    vectors.write_all(b)
    assert _snapshot(a) == _snapshot(b)
    assert all(rel.startswith("tier2/") for rel in _snapshot(a))
    for seed in ("1", "2"):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_vectors.py"), "--check"],
                           capture_output=True, text=True, cwd=ROOT,
                           env={**os.environ, "PYTHONHASHSEED": seed})
        assert r.returncode == 0, r.stdout + r.stderr
