"""tests/unreal_showcase: the Unreal showcase's headless boot test, the twin of tests/godot_showcase.
Every test here is marked `unreal_showcase`, which pyproject.toml registers and its addopts deselect
unless the `-m` expression names it, so a plain `pytest` never builds or
starts Unreal:

    uv run pytest -m unreal_showcase

The test runs `tools/run_unreal_e2e.py --showcase`, which needs UE 5.8 (its folder in $UE_ROOT),
xvfb-run and Mesa's lavapipe, and no chain; a missing one skips the test
(SCFG_REQUIRE_UNREAL=1 fails instead)."""

from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config, items):
    """Marks this directory's items `unreal_showcase`, so pyproject's addopts deselect them unless `-m`
    names the marker. tryfirst: before pytest's own `-m` deselection reads the marks."""
    for item in items:
        if HERE in Path(str(item.fspath)).resolve().parents:
            item.add_marker("unreal_showcase")
