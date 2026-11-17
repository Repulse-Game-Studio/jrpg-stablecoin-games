"""tests/unreal_e2e: the Unreal desktop e2e (tools/run_unreal_e2e.py) as pytest tests, one per chain
family. Every test here is marked `unreal_e2e` (registered here: pyproject.toml is not touched) and
deselected unless the `-m` expression names it, so a plain `pytest` never builds or starts Unreal:

    uv run pytest -m unreal_e2e [-k solana]           # SCFG_UNREAL_PACKAGED=1: the packaged mode

Each test runs the tool, which needs UE 5.8 ($UE_ROOT or /mnt/files/UnrealEngine), xvfb-run, Mesa's
lavapipe, Playwright's Chromium and anvil / solana-test-validator; a missing one skips the test
(SCFG_REQUIRE_UNREAL=1 fails instead)."""

from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "unreal_e2e: the Unreal desktop e2e under xvfb-run (needs UE 5.8, Xvfb, lavapipe, "
        "a chain node); deselected unless -m names it: run with -m unreal_e2e")


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config, items):
    """Marks this directory's items `unreal_e2e` (here, not at import time, for the reason given in
    tests/server_security/conftest.py), then deselects them unless `-m` names the marker.
    tryfirst: before pytest's own `-m` deselection reads the marks."""
    for item in items:
        if HERE in Path(str(item.fspath)).resolve().parents:
            item.add_marker("unreal_e2e")
    if "unreal_e2e" in (config.option.markexpr or ""):
        return
    dropped = [item for item in items if item.get_closest_marker("unreal_e2e") is not None]
    if dropped:
        config.hook.pytest_deselected(items=dropped)
        items[:] = [item for item in items if item.get_closest_marker("unreal_e2e") is None]
