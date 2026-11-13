"""The Godot web e2e runner's export helpers (tools/run_godot_web_e2e.py), without Godot."""

import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_godot_web_e2e as web  # noqa: E402

PROJECT = """config_version=5

[autoload]

Scfg="*res://addons/scfg/scfg.gd"

[display]
"""


def test_godot_version_reads_tag_and_templates_dir(monkeypatch):
    def fake_run(cmd, **kw):
        return subprocess.CompletedProcess(cmd, 0, stdout="4.7.2.stable.official.abc123\n")

    monkeypatch.setattr(web.subprocess, "run", fake_run)
    assert web.godot_version("godot") == ("4.7.2-stable", "4.7.2.stable")


def test_godot_version_refuses_other_output(monkeypatch):
    monkeypatch.setattr(web.subprocess, "run", lambda cmd, **kw: subprocess.CompletedProcess(
        cmd, 0, stdout="not godot\n"))
    with pytest.raises(web.E2EError):
        web.godot_version("godot")


PRESETS = """[preset.0]

name="Web"
include_filter="addons/scfg/web/*"
exclude_filter="e2e/*"
export_path=""
"""


def test_demo_copy_adds_the_driver_autoload_after_scfg(tmp_path, monkeypatch):
    demo = tmp_path / "demo"
    demo.mkdir()
    (demo / "project.godot").write_text(PROJECT)
    (demo / "export_presets.cfg").write_text(PRESETS)
    (demo / "override.cfg").write_text("[autoload]\n")
    (demo / "main.gd.uid").write_text("uid://x")
    monkeypatch.setattr(web, "DEMO", demo)
    copy = web.demo_copy(tmp_path / "work")
    text = (copy / "project.godot").read_text()
    assert ('Scfg="*res://addons/scfg/scfg.gd"\n'
            'ScfgE2EDriver="*res://e2e/e2e_driver.gd"\n') in text
    assert not (copy / "override.cfg").exists() and not (copy / "main.gd.uid").exists()
    # the demo's own export leaves the driver out; the e2e export needs it
    assert 'exclude_filter=""\n' in (copy / "export_presets.cfg").read_text()
    assert (demo / "project.godot").read_text() == PROJECT  # the demo itself is untouched
    assert (demo / "export_presets.cfg").read_text() == PRESETS


def test_the_demo_web_preset_leaves_the_driver_out():
    presets = (ROOT / "godot" / "demo" / "export_presets.cfg").read_text()
    assert 'exclude_filter="e2e/*"\n' in presets


def test_demo_copy_refuses_a_preset_without_the_driver_filter(tmp_path, monkeypatch):
    demo = tmp_path / "demo"
    demo.mkdir()
    (demo / "project.godot").write_text(PROJECT)
    (demo / "export_presets.cfg").write_text(PRESETS.replace('"e2e/*"', '""'))
    monkeypatch.setattr(web, "DEMO", demo)
    with pytest.raises(web.E2EError, match="exclude_filter"):
        web.demo_copy(tmp_path / "work")


def test_demo_copy_refuses_an_unexpected_project(tmp_path, monkeypatch):
    demo = tmp_path / "demo"
    demo.mkdir()
    (demo / "project.godot").write_text("config_version=5\n")
    (demo / "export_presets.cfg").write_text(PRESETS)
    monkeypatch.setattr(web, "DEMO", demo)
    with pytest.raises(web.E2EError, match="Scfg autoload"):
        web.demo_copy(tmp_path / "work")


def test_link_templates_points_the_run_at_the_installed_ones(tmp_path):
    installed = tmp_path / "data" / "godot" / "export_templates" / "4.3.stable"
    installed.mkdir(parents=True)
    (installed / "web_nothreads_release.zip").write_bytes(b"zip")
    run_data = tmp_path / "home" / "data"
    web.link_templates(tmp_path / "data", "4.3.stable", "4.3-stable", run_data)
    linked = run_data / "godot" / "export_templates" / "4.3.stable"
    assert (linked / "web_nothreads_release.zip").read_bytes() == b"zip"


def test_link_templates_says_how_to_install_missing_ones(tmp_path):
    with pytest.raises(web.E2EError) as e:
        web.link_templates(tmp_path / "none", "4.3.stable", "4.3-stable", tmp_path / "home")
    assert "fetch_godot.py --version 4.3-stable --templates --templates-dir" in str(e.value)


def test_amount_of_a_pending_session():
    assert web.amount_of({"units": 1_000_042}) == "1.000042"
    assert web.amount_of({"units": 12_500_000.0}) == "12.500000"


def test_refusal_problems_skip_only_the_audio_teardown_page_error():
    """Step 0: after the demo refused its config, only Godot's own web teardown (quitting while
    the page still starts its audio) may throw in the page; any other page error, engine error or
    a _ready that went on is a problem."""
    reason = "e2e config window.scfgE2EConfig: not a string but number"
    teardown = ("[pageerror] Failed to construct 'AudioWorkletNode': parameter 1 is not of type "
                "'BaseAudioContext'.")
    logs = ["Godot Engine v4.3", "USER ERROR: " + reason, "   at: push_error (x.cpp:1)",
            "TypeError: Failed to construct 'AudioWorkletNode': parameter 1 ...", teardown]
    assert web.refusal_problems(logs, reason) == []
    other = "[pageerror] ReferenceError: x is not defined"
    went_on = "USER ERROR: scfg: call Scfg.configure(...) first"
    assert web.refusal_problems(logs + [other, went_on, "USER WARNING: w"], reason) == [
        other, went_on, "USER WARNING: w"]
    assert web.refusal_problems(["Scfg.configure(...) first"], reason) == ["Scfg.configure(...) first"]
