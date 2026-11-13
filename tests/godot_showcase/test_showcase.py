"""The Godot showcase (godot/showcase/) boots and its chain-free buttons work.

A temporary copy of the showcase gets the addon (tools/sync_demo.py's copy) and a test-only
autoload (smoke_driver.gd) that presses Export, Clear, Restore and a Tip, closes both screens
with ui_cancel, and prints the event log. Needs $SCFG_GODOT (skips without it; with
SCFG_REQUIRE_GD=1, as in CI, it fails instead).
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SHOWCASE = ROOT / "godot" / "showcase"
DRIVER = Path(__file__).with_name("smoke_driver.gd")
TIMEOUT_S = 180


def _godot() -> str:
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_GD") == "1" else pytest.skip
    exe = os.environ.get("SCFG_GODOT", "")
    if not exe:
        give_up("SCFG_GODOT is not set (the path of a Godot 4.3+ binary)")
    if not (os.path.isfile(exe) and os.access(exe, os.X_OK)):
        give_up(f"SCFG_GODOT={exe} is not an executable file")
    return exe


def _sync_godot():
    spec = importlib.util.spec_from_file_location("sync_demo", ROOT / "tools" / "sync_demo.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.sync_godot


@pytest.fixture
def project(tmp_path: Path) -> Path:
    copy = tmp_path / "showcase"
    shutil.copytree(SHOWCASE, copy, ignore=shutil.ignore_patterns(".godot", "addons"))
    _sync_godot()(projects=(copy,))
    shutil.copy(DRIVER, copy / "smoke_driver.gd")
    (copy / "override.cfg").write_text('[autoload]\n\nSmokeDriver="*res://smoke_driver.gd"\n')
    return copy


def _run(godot: str, project: Path, *args: str, home: Path) -> subprocess.CompletedProcess:
    env = {**os.environ, "HOME": str(home), "XDG_DATA_HOME": str(home / "data")}
    return subprocess.run(
        [godot, "--headless", "--path", str(project), *args],
        capture_output=True, text=True, timeout=TIMEOUT_S, env=env,
    )


def test_the_showcase_boots_and_its_buttons_log_their_results(project, tmp_path):
    godot = _godot()
    home = tmp_path / "home"
    home.mkdir()
    imported = _run(godot, project, "--import", home=home)
    assert imported.returncode == 0, imported.stdout + imported.stderr

    run = _run(godot, project, home=home)
    out = run.stdout + run.stderr

    assert run.returncode == 0 and "SHOWCASE-OK" in out, out
    for bad in ("SCRIPT ERROR", "Parse Error", "ERROR:"):
        assert bad not in out, out
    log = out.split("SHOWCASE-LOG-BEGIN", 1)[1].split("SHOWCASE-LOG-END", 1)[0]
    for line in (
        "Ready. Build: desktop",
        "export_receipts() wrote ",
        "clear_local_data(): receipts forgotten",
        "restore result: cancelled",
        'pay("tip", { "amount": "1.00", "token": "USDC" })',
        "tip result: cancelled",
    ):
        assert line in log, log
