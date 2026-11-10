"""The shared localhost checkout server security suite: every test runs against the Python server
(scfg_core/local_server.py) and the GDScript one (godot/addons/scfg/local_server.gd).

The `godot` half needs a Godot 4.3+ binary in SCFG_GODOT and an imported project
(`$SCFG_GODOT --headless --path godot --import`). Without one it skips; with SCFG_REQUIRE_GD=1
it fails instead.
"""
import pytest

from server_handles import GodotDriver, GodotServer, PythonServer, godot_binary


def _engine_errors(text):
    """The lines of Godot's stderr that report an error: a GDScript error (a freed callback
    called) or a failed engine check (a closed socket read) reaches nothing else."""
    return [line for line in text.splitlines()
            if "SCRIPT ERROR" in line or line.lstrip().startswith("ERROR:")]


@pytest.fixture(scope="session")
def godot_driver():
    driver = GodotDriver(godot_binary())
    yield driver
    driver.close()
    errors = _engine_errors(driver.stderr_text())
    assert not errors, "the Godot server driver logged errors:\n" + "\n".join(errors)


@pytest.fixture(params=["python", "godot"])
def make_server(request, monkeypatch):
    """make_server(**config) -> a started server handle (see server_handles.py)."""
    made = []
    driver = request.getfixturevalue("godot_driver") if request.param == "godot" else None
    seen = len(driver.stderr_text()) if driver is not None else 0

    def make(**config):
        if driver is None:
            handle = PythonServer(config, monkeypatch)
        else:
            handle = GodotServer(config, driver)
        made.append(handle)
        return handle

    yield make
    for handle in made:
        handle.close()
    if driver is not None:
        errors = _engine_errors(driver.stderr_text()[seen:])
        assert not errors, "Godot logged errors during this test:\n" + "\n".join(errors)


@pytest.fixture
def server(make_server):
    return make_server()
