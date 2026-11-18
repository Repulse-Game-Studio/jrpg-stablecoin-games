"""The four localhost checkout servers behind one test interface, for test_server_security.py.

A server is described by a plain, JSON-able config (so the GDScript, Node and C# ones can get it
over a driver channel) and wrapped in a handle with the same members for every implementation:

    port, secret, url        what the page URL is built from
    calls()                  the callbacks the server made, in order: [["on_tx", [tx, sender]],
                             ["build_tx", [account]], ...]
    jobs()                   every build job handed out: [{"done": bool, "cancelled": bool}]
    stop(), running          as on the server

Config keys (all optional):
    assets        {name: bytes}: what load_asset returns; any other name fails (500)
    session_info  dict
    status        dict: what get_status returns
    logo          None (no load_logo), "broken" (load_logo fails), or (bytes, content type)
    build         None (no build_tx), or {"mode": "ok", "value": dict, "delay_s"?: float},
                  {"mode": "fail"} (the job fails), {"mode": "raise"} (build_tx itself fails)
                  or {"mode": "never"} (the job never finishes)
    build_timeout_s   BUILD_TIMEOUT_S for this server

Beside those three, the C++ one (the Unreal core's): CppDriver runs unreal/core-driver's
scfg_server_driver (built once per session by tests/integration/cpp_driver.py) with the driver
protocol below; each CppServer is a fresh scfg::local_server::CheckoutServer of the Unreal core
(Plugins/Scfg/Source/ScfgCore, LocalServer.h) on POSIX sockets.

PythonServer runs scfg_core.local_server.CheckoutServer in-process. GodotDriver runs
godot/tests/integration/server_driver.gd in one headless Godot per test session; each
GodotServer is a fresh ScfgCheckoutServer in it (the driver stops the previous one first).
NodeDriver runs rpgmaker/test/integration/server_driver.ts in one Node per test session, with the
same protocol; each NodeServer is a fresh CheckoutServer of rpgmaker/src/mz/local_server.ts.
CsDriver runs unity/server-driver (built once per session with `dotnet build -c Release`) in one
.NET process per test session, with the same protocol again; each CsServer is a fresh
Scfg.Desktop.CheckoutServer (unity/Packages/com.repulse.scfg/Runtime/Desktop/LocalServer.cs).
"""
from __future__ import annotations

import base64
import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeout
from pathlib import Path

import pytest

from scfg_core import local_server

ROOT = Path(__file__).resolve().parents[2]
# tests/integration/cpp_driver.py builds the C++ drivers; importing it by its module name (as the
# integration tests do) shares its once-per-session build.
if str(ROOT / "tests" / "integration") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests" / "integration"))
import cpp_driver  # noqa: E402

PROJECT = ROOT / "godot"
DRIVER = "res://tests/integration/server_driver.gd"
RPGMAKER = ROOT / "rpgmaker"
NODE_DRIVER = RPGMAKER / "test" / "integration" / "server_driver.ts"
MIN_NODE = (22, 18)  # runs .ts directly (type stripping), as tests/integration/js_driver.py
CS_PROJECT = ROOT / "unity" / "server-driver"
CS_DLL = CS_PROJECT / "bin" / "Release" / "net8.0" / "Scfg.Server.Driver.dll"
CS_BUILD_TIMEOUT = 600
REPLY_PREFIX = b"SCFG-REPLY "
REPLY_TIMEOUT = 60

DEFAULT_ASSETS = {
    "index.html": b"<html>page</html>",
    "app.js": b"// js",
    "evm.js": b"// evm.js",
    "solana.js": b"// solana.js",
}
DEFAULT_SESSION = {"chain_id": 42161, "amount_units": "4993742"}
DEFAULT_STATUS = {"state": "waiting"}


def godot_binary():
    """$SCFG_GODOT when it names an executable file. Skip otherwise; with SCFG_REQUIRE_GD=1 (CI)
    fail instead. (The same rule as tests/integration/gd_driver.py.)"""
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_GD") == "1" else pytest.skip
    exe = os.environ.get("SCFG_GODOT", "")
    if not exe:
        give_up("SCFG_GODOT is not set (the path of a Godot 4.3+ binary)")
    if not (os.path.isfile(exe) and os.access(exe, os.X_OK)):
        give_up(f"SCFG_GODOT={exe} is not an executable file")
    return exe


def node_binary():
    """`node` on PATH when it can run the TypeScript server (Node 22.18+, rpgmaker/node_modules
    installed). Skip otherwise; with SCFG_REQUIRE_JS=1 (CI) fail instead. (The same rule as
    tests/integration/js_driver.py.)"""
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_JS") == "1" else pytest.skip
    need = "the Node server needs Node {}.{} or newer".format(*MIN_NODE)
    node = shutil.which("node")
    if node is None:
        give_up(f"node not installed ({need})")
    try:
        out = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        out = ""
    m = re.match(r"v(\d+)\.(\d+)", out.strip())
    version = (int(m.group(1)), int(m.group(2))) if m else None
    if version is None or version < MIN_NODE:
        found = "unknown" if version is None else "{}.{}".format(*version)
        give_up(f"node {found} is too old or unreadable ({need})")
    if not (RPGMAKER / "node_modules").is_dir():
        give_up("rpgmaker/node_modules missing (cd rpgmaker && npm ci)")
    return node


def find_dotnet():
    """Path of the `dotnet` host: PATH, then $DOTNET_ROOT/dotnet, then ~/.dotnet/dotnet; else None.
    (The same rule as tests/integration/cs_driver.py.)"""
    found = shutil.which("dotnet")
    if found:
        return found
    for base in (os.environ.get("DOTNET_ROOT"), os.path.expanduser("~/.dotnet")):
        if base:
            candidate = Path(base) / "dotnet"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def _dotnet_env(dotnet):
    return dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1",
                DOTNET_ROOT=os.environ.get("DOTNET_ROOT") or str(Path(dotnet).resolve().parent))


_cs_built = None  # (dotnet, None) or (None, why not): the build's outcome, kept for the session


def _build_cs_driver():
    dotnet = find_dotnet()
    if dotnet is None:
        return None, "dotnet not installed (the C# server needs the .NET 8 SDK)"
    try:
        out = subprocess.run([dotnet, "build", str(CS_PROJECT), "-c", "Release"], cwd=ROOT,
                             env=_dotnet_env(dotnet), capture_output=True, text=True,
                             timeout=CS_BUILD_TIMEOUT)
    except (OSError, subprocess.SubprocessError) as e:
        return None, f"dotnet build could not run: {e}"
    if out.returncode != 0 or not CS_DLL.is_file():
        return None, ("dotnet build unity/server-driver failed:\n"
                      f"{out.stdout[-3000:]}{out.stderr[-1000:]}")
    return dotnet, None


def cs_binary():
    """The `dotnet` host, once unity/server-driver is built (at most once per session). Skip when
    dotnet is missing; with SCFG_REQUIRE_CS=1 (CI) fail instead, and a build that ran and failed
    always fails. (The same rule as tests/integration/cs_driver.py.)"""
    global _cs_built
    if _cs_built is None:
        _cs_built = _build_cs_driver()
    dotnet, why = _cs_built
    if dotnet is None:
        broken = why.startswith("dotnet build")
        give_up = pytest.fail if (broken or os.environ.get("SCFG_REQUIRE_CS") == "1") else pytest.skip
        give_up(why)
    return dotnet


class PythonServer:
    def __init__(self, config, monkeypatch):
        self._calls = []
        self._jobs = []
        assets = config.get("assets", DEFAULT_ASSETS)
        if "build_timeout_s" in config:
            monkeypatch.setattr(local_server, "BUILD_TIMEOUT_S", config["build_timeout_s"])
        logo = config.get("logo")
        if logo is None:
            load_logo = None
        elif logo == "broken":
            def load_logo():
                raise OSError("gone")
        else:
            def load_logo():
                return logo[0], logo[1]
        build = config.get("build")
        build_tx = None if build is None else (lambda account: self._build(build, account))
        self._srv = local_server.CheckoutServer(
            load_asset=lambda name: assets[name],
            session_info=config.get("session_info", DEFAULT_SESSION),
            on_tx=lambda tx, sender: self._calls.append(["on_tx", [tx, sender]]),
            get_status=lambda: config.get("status", DEFAULT_STATUS),
            load_logo=load_logo, build_tx=build_tx)
        self._srv.start()

    def _build(self, build, account):
        self._calls.append(["build_tx", [account]])
        if build["mode"] == "raise":
            raise RuntimeError("submit failed")
        fut = Future()
        self._jobs.append(fut)
        if build["mode"] == "fail":
            fut.set_exception(FutureTimeout())
        elif build["mode"] == "ok":
            if build.get("delay_s"):
                timer = threading.Timer(build["delay_s"], fut.set_result, [build["value"]])
                timer.daemon = True
                timer.start()
            else:
                fut.set_result(build["value"])
        return fut

    port = property(lambda self: self._srv.port)
    secret = property(lambda self: self._srv.secret)
    url = property(lambda self: self._srv.url)
    running = property(lambda self: self._srv.running)

    def calls(self):
        return list(self._calls)

    def jobs(self):
        return [{"done": f.done(), "cancelled": f.cancelled()} for f in self._jobs]

    def stop(self):
        self._srv.stop()

    def close(self):
        self._srv.stop()


class LineDriver:
    """A driver process: JSON commands on stdin, replies on stdout as lines starting with
    REPLY_PREFIX (anything else there is kept as noise, e.g. Godot's banner)."""

    hint = ""  # added to the "no reply" failure

    def __init__(self, argv, cwd, env=None):
        self._stderr = tempfile.TemporaryFile()
        self._noise = []
        self._out = b""
        self._proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=self._stderr)

    def call(self, op, **args):
        line = json.dumps({"op": op, **args}).encode()  # ASCII: Godot 4.3 reads stdin as Latin-1
        self._proc.stdin.write(line + b"\n")
        self._proc.stdin.flush()
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            out = self._read_line(deadline)
            if out is None:
                raise AssertionError(
                    f"server driver gave no reply to {op}{self.hint}; stdout:\n"
                    + "".join(self._noise) + f"stderr:\n{self.stderr_text()}")
            if out.startswith(REPLY_PREFIX):
                break
            self._noise.append(out.decode(errors="replace"))
        reply = json.loads(out[len(REPLY_PREFIX):])
        if "error" in reply:
            raise AssertionError(f"server driver refused {op}: {reply['error']}")
        return reply["ok"]

    def _read_line(self, deadline):
        fd = self._proc.stdout.fileno()
        while b"\n" not in self._out:
            ready, _, _ = select.select([fd], [], [], max(deadline - time.monotonic(), 0))
            chunk = os.read(fd, 1 << 16) if ready else b""
            if not chunk:
                return None
            self._out += chunk
        line, _, self._out = self._out.partition(b"\n")
        return line + b"\n"

    @property
    def alive(self):
        return self._proc.poll() is None

    def stderr_text(self):
        """Everything the driver wrote to stderr so far (all of it, once closed)."""
        if self._stderr.closed:
            return self._final_stderr
        self._stderr.seek(0)
        return self._stderr.read().decode(errors="replace")

    def close(self):
        try:
            try:
                self._proc.stdin.close()
            except OSError:
                pass
            try:
                self._proc.wait(10)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        finally:
            self._proc.stdout.close()
            self._final_stderr = self.stderr_text()
            self._stderr.close()


class GodotDriver(LineDriver):
    """One headless Godot running server_driver.gd."""

    hint = (" (has the project been imported? "
            "`$SCFG_GODOT --headless --path godot --import`)")

    def __init__(self, exe):
        super().__init__([exe, "--headless", "--path", str(PROJECT), "--script", DRIVER], ROOT)


class NodeDriver(LineDriver):
    """One Node running rpgmaker/test/integration/server_driver.ts."""

    def __init__(self, node):
        super().__init__([node, str(NODE_DRIVER)], RPGMAKER)


class CsDriver(LineDriver):
    """One .NET process running unity/server-driver (built by cs_binary)."""

    def __init__(self, dotnet):
        super().__init__([dotnet, str(CS_DLL)], ROOT, env=_dotnet_env(dotnet))


def _b64(data):
    return base64.b64encode(data).decode("ascii")


def cpp_server_binary():
    """The C++ server driver (scfg_server_driver), built once per session. Skip when cmake (3.25 or
    newer), a C++20 compiler or a build tool is missing; with SCFG_REQUIRE_CPP=1 (CI) fail instead.
    A configure or compile failure always fails. (The same rule as tests/integration/cpp_driver.py.)"""
    return cpp_driver.skip_unless_available(cpp_driver.SERVER_TARGET)


class CppDriver(LineDriver):
    """One scfg_server_driver process (unreal/core-driver/ServerMain.cpp)."""

    def __init__(self, binary):
        super().__init__([binary], ROOT)


class DriverServer:
    """A server in a driver process (the GDScript, the Node or the C# one)."""

    def __init__(self, config, driver):
        self._driver = driver
        logo = config.get("logo")
        wire = {
            "assets": {k: _b64(v) for k, v in config.get("assets", DEFAULT_ASSETS).items()},
            "session_info": config.get("session_info", DEFAULT_SESSION),
            "status": config.get("status", DEFAULT_STATUS),
            "logo": logo if logo in (None, "broken") else {"b64": _b64(logo[0]), "ctype": logo[1]},
            "build": config.get("build"),
        }
        if "build_timeout_s" in config:
            wire["build_timeout_s"] = config["build_timeout_s"]
        started = driver.call("start", config=wire)
        self.port, self.secret, self.url = started["port"], started["secret"], started["url"]

    def calls(self):
        return self._driver.call("events")["calls"]

    def jobs(self):
        return self._driver.call("events")["jobs"]

    @property
    def running(self):
        return self._driver.call("events")["running"]

    def stop(self):
        self._driver.call("stop")

    def close(self):
        if self._driver.alive:
            self._driver.call("stop")


class GodotServer(DriverServer):
    """addons/scfg/local_server.gd in the Godot driver."""


class CppServer(DriverServer):
    """The Unreal core's scfg::local_server::CheckoutServer in the C++ driver."""


class NodeServer(DriverServer):
    """rpgmaker/src/mz/local_server.ts in the Node driver."""


class CsServer(DriverServer):
    """unity/Packages/com.repulse.scfg/Runtime/Desktop/LocalServer.cs in the C# driver."""
