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
    """Marks this directory's items `unreal_showcase`, then deselects them unless the `-m` expression names
    the marker: pyproject's addopts do that for the default run, but a `-m` on the command line
    replaces theirs, and a negative one (`-m "not integration"`) must not start Unreal either.
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
