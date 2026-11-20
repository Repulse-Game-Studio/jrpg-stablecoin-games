"""What the Unreal tools share (run_unreal_build.py, run_unreal_tests.py, run_unreal_e2e.py and
package_unreal.py): the engine's root, the dev project, the DDC graph without zen, the plugin's
test-only modules, and the two ways a process group is ended."""

from __future__ import annotations

import os
import signal
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "unreal" / "Project" / "ScfgDev" / "ScfgDev.uproject"
# The engine's root directory comes from --engine or this variable; there is no default.
ENGINE_ENV = "UE_ROOT"
ENGINE_HELP = "the Unreal Engine's root directory (default $UE_ROOT; one of the two is needed)"
NO_ENGINE = "no Unreal Engine given: set UE_ROOT to the engine's root directory or pass --engine"
# The plugin's test-only modules (Scfg.uplugin: UncookedOnly): never linked into a game, and left
# out of the release zips.
TEST_MODULES = ("ScfgCoreTests", "ScfgTests")


def engine_root(given: str | os.PathLike | None, environ=None) -> Path | None:
    """The engine's root directory: `given` (--engine), else $UE_ROOT; None when neither is set
    (an empty value counts as unset)."""
    environ = os.environ if environ is None else environ
    value = given or environ.get(ENGINE_ENV)
    return Path(value) if value else None


def ddc_graph(engine: Path) -> str:
    """BaseEngine.ini's DDC graph without ZenLocal: the Installed variant on an installed engine
    (Engine/Build/InstalledBuild.txt), whose default graph is Installed too."""
    installed = (engine / "Engine" / "Build" / "InstalledBuild.txt").is_file()
    return "InstalledNoZenLocalFallback" if installed else "NoZenLocalFallback"


def kill_group_now(pgid: int) -> None:
    """SIGKILL every process of the group, whether or not its leader is still alive."""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass   # the whole group is gone already


def group_alive(pgid: int) -> bool:
    """The process group has a process left (one of another user's counts too)."""
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def terminate_group(pgid: int, grace: float = 5.0) -> None:
    """SIGTERM the process group, then SIGKILL what is left after `grace` seconds."""
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if not group_alive(pgid):
            return
        time.sleep(0.1)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass
