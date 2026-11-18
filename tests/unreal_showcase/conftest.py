"""tests/unreal_showcase: the Unreal showcase's headless boot test, the twin of tests/godot_showcase.
Every test here is marked `unreal_showcase` (registered here, so the marker travels with the
directory) and deselected unless the `-m` expression names it, so a plain `pytest` never builds or
starts Unreal:

    uv run pytest -m unreal_showcase

The test runs `tools/run_unreal_e2e.py --showcase`, which needs UE 5.8 (its folder in $UE_ROOT),
xvfb-run and Mesa's lavapipe, and no chain; a missing one skips the test
(SCFG_REQUIRE_UNREAL=1 fails instead)."""

from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "unreal_showcase: the Unreal showcase's boot test under xvfb-run (needs UE 5.8, "
        "Xvfb, lavapipe); deselected unless -m names it: run with -m unreal_showcase")


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config, items):
    """Marks this directory's items `unreal_showcase` (here, not at import time, for the reason
    given in tests/server_security/conftest.py), then deselects them unless `-m` names the marker.
    tryfirst: before pytest's own `-m` deselection reads the marks."""
    for item in items:
        if HERE in Path(str(item.fspath)).resolve().parents:
            item.add_marker("unreal_showcase")
    if "unreal_showcase" in (config.option.markexpr or ""):
        return
    dropped = [item for item in items if item.get_closest_marker("unreal_showcase") is not None]
    if dropped:
        config.hook.pytest_deselected(items=dropped)
        items[:] = [item for item in items if item.get_closest_marker("unreal_showcase") is None]
