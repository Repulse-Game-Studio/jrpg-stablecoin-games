"""The shared localhost checkout server security suite: every test runs against the Python server
(scfg_core/local_server.py), the GDScript one (godot/addons/scfg/local_server.gd) and the Node one
(rpgmaker/src/mz/local_server.ts, RPG Maker MZ's desktop page).

The `godot` half (marked `godot_server`) needs a Godot 4.3+ binary in SCFG_GODOT and an imported
project (`$SCFG_GODOT --headless --path godot --import`). Without one it skips; with
SCFG_REQUIRE_GD=1 it fails instead. CI's godot-core job runs only this half, once per Godot
version: `pytest tests/server_security -m godot_server`.

The `node` half needs Node 22.18+ on PATH and rpgmaker/node_modules (`cd rpgmaker && npm ci`).
Without them it skips; with SCFG_REQUIRE_JS=1 it fails instead. The node half is marked
`node_server` and deselected by default; CI's rpgmaker-ui job runs it: `pytest -m node_server`.
"""
import os
from pathlib import Path

import pytest

from server_handles import (
    GodotDriver,
    GodotServer,
    NodeDriver,
    NodeServer,
    PythonServer,
    godot_binary,
    node_binary,
)


def _engine_errors(text):
    """The lines of Godot's stderr that report an error: a GDScript error (a freed callback
    called) or a failed engine check (a closed socket read) reaches nothing else."""
    return [line for line in text.splitlines()
            if "SCRIPT ERROR" in line or line.lstrip().startswith("ERROR:")]


def _node_errors(text):
    """Every line Node wrote to stderr: the driver writes nothing there, so a line is an uncaught
    exception or a warning from a socket handler."""
    return [line for line in text.splitlines() if line.strip()]


@pytest.fixture(scope="session")
def godot_driver():
    driver = GodotDriver(godot_binary())
    yield driver
    driver.close()
    errors = _engine_errors(driver.stderr_text())
    assert not errors, "the Godot server driver logged errors:\n" + "\n".join(errors)


@pytest.fixture(scope="session")
def node_driver():
    driver = NodeDriver(node_binary())
    yield driver
    driver.close()
    errors = _node_errors(driver.stderr_text())
    assert not errors, "the Node server driver logged errors:\n" + "\n".join(errors)


@pytest.fixture(params=["python", pytest.param("godot", marks=pytest.mark.godot_server),
                        pytest.param("node", marks=pytest.mark.node_server)])
def make_server(request, monkeypatch):
    """make_server(**config) -> a started server handle (see server_handles.py)."""
    made = []
    driver, handle_class, errors_in = None, None, None
    if request.param == "godot":
        driver, handle_class, errors_in = request.getfixturevalue("godot_driver"), GodotServer, _engine_errors
    elif request.param == "node":
        driver, handle_class, errors_in = request.getfixturevalue("node_driver"), NodeServer, _node_errors
    seen = len(driver.stderr_text()) if driver is not None else 0

    def make(**config):
        if driver is None:
            handle = PythonServer(config, monkeypatch)
        else:
            handle = handle_class(config, driver)
        made.append(handle)
        return handle

    yield make
    for handle in made:
        handle.close()
    if driver is not None:
        errors = errors_in(driver.stderr_text()[seen:])
        assert not errors, f"the {request.param} server driver logged errors during this test:\n" + "\n".join(errors)


@pytest.fixture
def server(make_server):
    return make_server()


def _explicit_markexpr(config):
    """Whether `-m` was given on the command line (not only by pyproject's addopts)."""
    return any(a == "-m" or a.startswith("-m") for a in config.invocation_params.args)


def pytest_configure(config):
    """The default `-m` (pyproject addopts) deselects `node_server` tests, which have their own CI
    job. Naming this directory (or a file in it) on the command line, or SCFG_REQUIRE_JS=1,
    runs them; an explicit `-m` from the command line is left alone."""
    term = " and not node_server"
    if term not in (config.option.markexpr or "") or _explicit_markexpr(config):
        return
    here = Path(__file__).resolve().parent
    named = False
    for arg in config.args:
        path = Path(config.invocation_params.dir, arg.split("::")[0]).resolve()
        if path == here or here in path.parents:
            named = True
    if named or os.environ.get("SCFG_REQUIRE_JS") == "1":
        config.option.markexpr = config.option.markexpr.replace(term, "")


def pytest_terminal_summary(terminalreporter, config):
    """A run that ran nothing because the default `-m` deselected it says how to select the
    node_server tests; not after --collect-only or a `-m` of the caller's own."""
    if config.option.collectonly or _explicit_markexpr(config):
        return
    if config.option.markexpr and "not node_server" in config.option.markexpr:
        stats = terminalreporter.stats
        if stats.get("deselected") and not any(
            stats.get(k) for k in ("passed", "failed", "skipped", "error")
        ):
            terminalreporter.write_line(
                "hint: the node_server tests are deselected by default; run them with "
                "`pytest -m node_server`", yellow=True)
