"""The two localhost checkout servers behind one test interface, for test_server_security.py.

A server is described by a plain, JSON-able config (so the GDScript one can get it over the
driver channel) and wrapped in a handle with the same members for both implementations:

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

PythonServer runs scfg_core.local_server.CheckoutServer in-process. GodotDriver runs
godot/tests/integration/server_driver.gd in one headless Godot per test session; each
GodotServer is a fresh ScfgCheckoutServer in it (the driver stops the previous one first).
"""
from __future__ import annotations

import base64
import json
import os
import select
import subprocess
import tempfile
import threading
import time
from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeout
from pathlib import Path

import pytest

from scfg_core import local_server

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "godot"
DRIVER = "res://tests/integration/server_driver.gd"
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


class GodotDriver:
    """One headless Godot running server_driver.gd: JSON commands on stdin, replies on stdout
    as lines starting with REPLY_PREFIX (Godot prints its own banner there too)."""

    def __init__(self, exe):
        self._stderr = tempfile.TemporaryFile()
        self._noise = []
        self._out = b""
        self._proc = subprocess.Popen(
            [exe, "--headless", "--path", str(PROJECT), "--script", DRIVER], cwd=ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr)

    def call(self, op, **args):
        line = json.dumps({"op": op, **args}).encode()  # ASCII: Godot 4.3 reads stdin as Latin-1
        self._proc.stdin.write(line + b"\n")
        self._proc.stdin.flush()
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            out = self._read_line(deadline)
            if out is None:
                raise AssertionError(
                    f"server driver gave no reply to {op} (has the project been imported? "
                    f"`$SCFG_GODOT --headless --path godot --import`); stdout:\n"
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
        """Everything Godot wrote to stderr so far (all of it, once closed)."""
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


def _b64(data):
    return base64.b64encode(data).decode("ascii")


class GodotServer:
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
