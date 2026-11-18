"""The Unreal showcase (unreal/Project/ScfgDev, ScfgShowcase.h) boots and its chain-free buttons
work, as tests/godot_showcase checks for Godot's (see conftest.py: marked unreal_showcase,
deselected by default).

`tools/run_unreal_e2e.py --showcase` builds the editor target, starts the showcase as a game on an
Xvfb of its own (xvfb-run -a, lavapipe) with the e2e driver's control socket, and lets it configure
itself from the project settings (Config/DefaultGame.ini). It clicks Export receipts, Clear local
data, Notify (off and on), Restore purchases, a tip and an unlock (each sheet closed with Escape on
its first step, before a network is chosen), and checks the readouts and the event log after each;
then it starts the showcase again with -scfg-showcase-code and checks the same shop, configured in
code. Each start must have found the settings' logo and posted no JSON-RPC request (the plugin's
HTTP transport counts them). Any Error or Warning line of the game fails it."""

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
LAVAPIPE = Path("/usr/share/vulkan/icd.d/lvp_icd.json")


def _missing() -> str | None:
    if not (ENGINE / "Engine" / "Binaries" / "Linux" / "UnrealEditor").is_file():
        return f"no UE 5.8 at {ENGINE} (set UE_ROOT)"
    if not shutil.which("xvfb-run"):
        return "xvfb-run not found on PATH"
    if not LAVAPIPE.is_file():
        return "Mesa's lavapipe (mesa-vulkan-drivers) is not installed"
    return None


def test_the_showcase_boots_and_its_chain_free_buttons_work():
    missing = _missing()
    if missing:
        if os.environ.get("SCFG_REQUIRE_UNREAL") == "1":
            pytest.fail(missing)
        pytest.skip(missing)
    run = subprocess.run([sys.executable, str(TOOL), "--showcase"], cwd=ROOT,
                         capture_output=True, text=True, timeout=3 * 3600)
    out = run.stdout
    assert run.returncode == 0 and "Unreal showcase tour PASSED" in out, out[-6000:] + run.stderr[-3000:]
    for line in (
        "1. configured from the project settings",
        "shop: 11 buttons; Configured from: the project settings (Config/DefaultGame.ini); "
        "Supported: yes, Owned: nothing yet, Tips: 0 USDC",
        "Export receipts: wrote scfg-receipts.json (no receipt)",
        "Clear local data: logged",
        "Notify: off, then on",
        "Restore purchases: cancelled",
        "Tip 1.00 USDC: cancelled on the amount step",
        "Unlock Chapter 2: cancelled on the network step",
        "2. configured in code, -scfg-showcase-code",
        "Configured from: code (ConfigureInCode): the same 11 buttons and readouts",
    ):
        assert line in out, out
    # each start: the logo found, and no JSON-RPC request made (FScfgHttpTransport's count)
    assert out.count("Content/Showcase/logo.png was found; 0 JSON-RPC requests") == 2, out
