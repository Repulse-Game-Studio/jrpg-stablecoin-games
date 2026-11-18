"""tools/run_unity_e2e.py's pure parts (the run itself needs a Unity editor and a local chain)."""

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("run_unity_e2e", ROOT / "tools" / "run_unity_e2e.py")
e2e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e2e)


def test_problems_are_the_players_reported_lines_only():
    lines = [
        '[UnityMemory] Configuration Parameters - Can be set up in boot.config\n',
        'SCFG-REPLY {"ok": true}\n',
        "SCFG-PROBLEM Error: scfg: call SCFG.Configure(...) first\n",
        "SCFG-QUITTING\n",
        "SCFG-PROBLEM Warning: something\n",
    ]
    assert e2e.problems(lines) == ["Error: scfg: call SCFG.Configure(...) first", "Warning: something"]


def test_the_runner_reuses_the_godot_families():
    assert set(e2e.scenario.FAMILIES) == {"evm", "arc", "solana"}
    assert e2e.EXECUTABLE == "scfg-e2e.x86_64"
