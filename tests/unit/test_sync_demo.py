import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def sync(tmp_path):
    spec = importlib.util.spec_from_file_location("sync_demo", ROOT / "tools" / "sync_demo.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_godot_copies_the_addon_without_tests_or_caches(sync, tmp_path):
    src = tmp_path / "godot" / "addons" / "scfg"
    (src / "core").mkdir(parents=True)
    (src / "tests").mkdir()
    (src / ".godot").mkdir()
    (src / "scfg.gd").write_text("extends Node\n")
    (src / "scfg.gd.uid").write_text("uid://abc\n")
    (src / "core" / "engine.gd").write_text("class_name E\n")
    (src / "tests" / "t.gd").write_text("x\n")
    (src / ".godot" / "cache").write_text("x\n")
    dst = tmp_path / "godot" / "demo" / "addons" / "scfg"
    dst.mkdir(parents=True)
    (dst / "stale.gd").write_text("old\n")

    sync.sync_godot(src=src, dst=dst)

    assert (dst / "scfg.gd").read_text() == "extends Node\n"
    assert (dst / "core" / "engine.gd").exists()
    assert not (dst / "tests").exists()
    assert not (dst / ".godot").exists()
    assert not (dst / "scfg.gd.uid").exists()
    assert not (dst / "stale.gd").exists()


def test_renpy_copies_plugin_and_packages_without_caches(sync, tmp_path):
    src = tmp_path / "plugin" / "game"
    for name in ("scfg", "python-packages"):
        (src / name / "__pycache__").mkdir(parents=True)
        (src / name / "a.py").write_text(name)
        (src / name / "b.rpyc").write_text("x")
    dst = tmp_path / "demo" / "game"

    sync.sync_renpy(src=src, dst=dst)

    assert (dst / "scfg" / "a.py").read_text() == "scfg"
    assert (dst / "python-packages" / "a.py").exists()
    assert not (dst / "scfg" / "b.rpyc").exists()
    assert not (dst / "scfg" / "__pycache__").exists()


def test_default_engine_is_renpy(sync):
    assert sync.parse_args([]).engine == "renpy"
    assert sync.parse_args(["--engine", "godot"]).engine == "godot"
