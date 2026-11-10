"""The Godot desktop e2e runner (tools/run_godot_e2e.py), without Godot or a chain."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_godot_e2e as e2e  # noqa: E402


def test_refuses_to_run_over_an_existing_override_cfg(tmp_path, monkeypatch, capsys):
    """The run writes godot/demo/override.cfg and deletes it afterwards: a developer's own file
    there must stop the run before anything happens, and stay as it was."""
    override = tmp_path / "override.cfg"
    override.write_text("; mine\n[display]\n", encoding="utf-8")
    monkeypatch.setattr(e2e, "OVERRIDE", override)
    monkeypatch.setattr(e2e.shutil, "which", lambda name: "/usr/bin/" + name)

    def must_not_run(*a, **kw):
        raise AssertionError("the run started")

    monkeypatch.setattr(e2e, "sync_godot", must_not_run)
    monkeypatch.setattr(e2e.tempfile, "mkdtemp", must_not_run)
    assert e2e.run("godot", "evm") == 2
    assert override.read_text(encoding="utf-8") == "; mine\n[display]\n", "left as it was"
    out = capsys.readouterr().out
    assert str(override) in out and "override.cfg" in out, out
