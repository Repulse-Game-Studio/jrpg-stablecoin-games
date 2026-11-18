"""tests/unreal_e2e: the Unreal desktop e2e (tools/run_unreal_e2e.py) as pytest tests, one per chain
family. Every test here is marked `unreal_e2e`, which pyproject.toml registers and its addopts deselect
unless the `-m` expression names it, so a plain `pytest` never builds or
starts Unreal:

    uv run pytest -m unreal_e2e [-k solana]           # SCFG_UNREAL_PACKAGED=1: the packaged mode

Each test runs the tool, which needs UE 5.8 (its folder in $UE_ROOT), xvfb-run, Mesa's lavapipe,
Playwright's Chromium and anvil / solana-test-validator; a missing one skips the test
(SCFG_REQUIRE_UNREAL=1 fails instead)."""

from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config, items):
    """Marks this directory's items `unreal_e2e`, so pyproject's addopts deselect them unless `-m`
    names the marker. tryfirst: before pytest's own `-m` deselection reads the marks."""
    for item in items:
        if HERE in Path(str(item.fspath)).resolve().parents:
            item.add_marker("unreal_e2e")
