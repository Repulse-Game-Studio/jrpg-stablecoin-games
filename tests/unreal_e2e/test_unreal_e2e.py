"""tools/run_unreal_e2e.py for each chain family (see conftest.py: marked unreal_e2e, deselected by
default)."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "run_unreal_e2e.py"
ENGINE = Path(os.environ.get("UE_ROOT", "/mnt/files/UnrealEngine"))
NODES = {"evm": "anvil", "arc": "anvil", "solana": "solana-test-validator"}


def _missing(family: str) -> str | None:
    if not (ENGINE / "Engine" / "Binaries" / "Linux" / "UnrealEditor").is_file():
        return f"no UE 5.8 at {ENGINE} (set UE_ROOT)"
    for tool in ("xvfb-run", NODES[family]):
        if not shutil.which(tool):
            return f"{tool} not found on PATH"
    if not Path("/usr/share/vulkan/icd.d/lvp_icd.json").is_file():
        return "Mesa's lavapipe (mesa-vulkan-drivers) is not installed"
    return None


@pytest.mark.parametrize("family", sorted(NODES))
def test_the_showcase_pays_on_a_local_chain(family):
    missing = _missing(family)
    if missing:
        if os.environ.get("SCFG_REQUIRE_UNREAL") == "1":
            pytest.fail(missing)
        pytest.skip(missing)
    cmd = [sys.executable, str(TOOL), "--family", family]
    if os.environ.get("SCFG_UNREAL_PACKAGED") == "1":  # an environment variable, not an option:
        cmd.append("--packaged")  # this directory's conftest is not loaded for `pytest -m ...`
    run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=3 * 3600)
    assert run.returncode == 0, run.stdout[-6000:] + run.stderr[-3000:]
    assert f"Unreal e2e ({family}," in run.stdout and "PASSED" in run.stdout
