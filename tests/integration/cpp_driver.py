"""Drives the C++ Engine (unreal/Plugins/Scfg/Source/ScfgCore) through its JSON-lines driver
process (unreal/core-driver), so the Anvil / solana-test-validator fixtures can test that core too.

Protocol: rpgmaker/test/integration/engine_driver.ts (stdout carries only replies). Methods mirror
the Python Engine's names and return the decoded JSON (Tier 2 `returns` encoding): plain dicts and
lists, amounts as Decimal strings.

cmake (3.25 or newer) comes from PATH, then ~/.local/bin; the compiler is $CXX when its program
resolves (cmake then reads $CXX itself, arguments and all, as in CXX="ccache g++"), else c++, g++ or
clang++ on PATH. Ninja is used when it is found the same way, else make (Unix Makefiles).
The driver is built once per Python process (`cmake -S unreal -B unreal/build/integration
-DCMAKE_BUILD_TYPE=Release`, then only the scfg_core_driver target, one job per CPU) and its binary then
run directly. tests/server_security builds the checkout server's driver (SERVER_TARGET,
scfg_server_driver) through the same build_driver(), once per session too.
"""
import json
import os
import re
import select
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "unreal"
BUILD_DIR = SOURCE / "build" / "integration"
TARGET = "scfg_core_driver"
SERVER_TARGET = "scfg_server_driver"  # the localhost checkout server's driver (tests/server_security)
BINARY = BUILD_DIR / TARGET
REPLY_TIMEOUT = 120
BUILD_TIMEOUT = 900
MISSING = "cmake or a C++20 compiler not found"
CMAKE_MIN = (3, 25)  # unreal/CMakeLists.txt's cmake_minimum_required

# {target: (binary, None, False) or (None, why not, broken)}: each build's outcome, kept for the session
_built = {}


def find_tool(name):
    """Path of `name`: PATH, then ~/.local/bin; else None."""
    found = shutil.which(name)
    if found:
        return found
    candidate = Path.home() / ".local" / "bin" / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    return None


def find_compiler():
    """(path, from_env): the program $CXX starts with (its first word: CXX may carry a launcher or
    flags) when it resolves, with from_env True; else c++, g++ or clang++ from PATH, with from_env
    False; else (None, False)."""
    cxx = os.environ.get("CXX", "")
    try:
        words = shlex.split(cxx)
    except ValueError:  # unbalanced quotes: not a usable command line
        words = []
    if words:
        found = shutil.which(words[0])
        if found:
            return found, True
    for name in ("c++", "g++", "clang++"):
        found = shutil.which(name)
        if found:
            return found, False
    return None, False


def cmake_version(cmake):
    """(major, minor) that `cmake --version` reports, else None."""
    try:
        out = subprocess.run([cmake, "--version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"cmake version (\d+)\.(\d+)", out.stdout)
    return (int(m.group(1)), int(m.group(2))) if out.returncode == 0 and m else None


def build_driver(target=TARGET):
    """(binary path, None, False) once `target` (TARGET or SERVER_TARGET) is built, else (None,
    why not, whether a build ran and failed). Builds each target at most once."""
    if target not in _built:
        _built[target] = _build(target)
    return _built[target]


def _build(target):
    # A missing or unusable toolchain is (None, why, False): a skip, or a failure under
    # SCFG_REQUIRE_CPP=1. Once it is usable, a failed configure or build is a failure.
    cmake, (cxx, cxx_from_env) = find_tool("cmake"), find_compiler()
    if cmake is None or cxx is None:
        return None, f"{MISSING} (cmake: {cmake or 'missing'}, compiler: {cxx or 'missing'})", False
    version = cmake_version(cmake)
    if version is None or version < CMAKE_MIN:
        found = "no version" if version is None else f"{version[0]}.{version[1]}"
        return None, (f"cmake {CMAKE_MIN[0]}.{CMAKE_MIN[1]} or newer needed "
                      f"(found {found} at {cmake})"), False
    ninja, make = find_tool("ninja"), find_tool("make")
    if ninja is None and make is None:
        return None, "no build tool for cmake: neither ninja nor make found", False
    configure = [cmake, "-S", str(SOURCE), "-B", str(BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"]
    if not (BUILD_DIR / "CMakeCache.txt").is_file():
        # The generator and the compiler are fixed when the tree is first configured: naming
        # others on a tree configured with different ones would be an error or a reset.
        if not cxx_from_env:
            configure.append(f"-DCMAKE_CXX_COMPILER={cxx}")  # else cmake reads $CXX itself
        if ninja is not None:
            configure += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
        else:
            configure += ["-G", "Unix Makefiles", f"-DCMAKE_MAKE_PROGRAM={make}"]
    # A job count: a bare --parallel is make's unbounded -j (every compile at once).
    jobs = str(os.cpu_count() or 2)
    steps = [configure, [cmake, "--build", str(BUILD_DIR), "--target", target, "--parallel", jobs]]
    for step in steps:
        try:
            out = subprocess.run(step, cwd=ROOT, capture_output=True, text=True,
                                 timeout=BUILD_TIMEOUT)
        except (OSError, subprocess.SubprocessError) as e:
            return None, f"cmake could not run ({' '.join(step[:2])}): {e}", True
        if out.returncode != 0:
            return None, (f"building {target} failed ({' '.join(step)}):\n"
                          f"{out.stdout[-3000:]}{out.stderr[-2000:]}"), True
    binary = BUILD_DIR / target
    if not (binary.is_file() and os.access(binary, os.X_OK)):
        return None, f"building {target} made no {binary}", True
    return str(binary), None, False


def skip_unless_available(target=TARGET):
    """Build `target`; skip when cmake (3.25 or newer), a compiler or a build tool (ninja or
    make) is missing; with SCFG_REQUIRE_CPP=1 (CI) fail instead."""
    binary, why, broken = build_driver(target)
    if binary is None:
        # A build that ran and failed is a failure whatever the flag says; only a missing or
        # unusable toolchain skips.
        give_up = pytest.fail if (broken or os.environ.get("SCFG_REQUIRE_CPP") == "1") else pytest.skip
        give_up(why)
    return binary


class CppEngineError(Exception):
    def __init__(self, type_, code, message):
        super().__init__(f"{type_}({code}): {message}")
        self.type, self.code, self.message = type_, code, message


class CppEngine:
    def __init__(self, override, config, items, custom_chains=()):
        binary = skip_unless_available()
        self._stderr = tempfile.TemporaryFile()
        self._proc = subprocess.Popen(
            [binary], cwd=ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr)
        try:
            self._call("init", override=override, config=config, items=list(items),
                       custom_chains=list(custom_chains))
        except BaseException:
            self.close()
            raise

    def _call(self, op, **args):
        self._proc.stdin.write((json.dumps({"op": op, **args}) + "\n").encode())
        self._proc.stdin.flush()
        ready, _, _ = select.select([self._proc.stdout], [], [], REPLY_TIMEOUT)
        line = self._proc.stdout.readline() if ready else b""
        if not line:
            raise AssertionError(f"driver gave no reply to {op}; stderr:\n{self.stderr_text()}")
        reply = json.loads(line)
        if "error" in reply:
            e = reply["error"]
            raise CppEngineError(e["type"], e["code"], e["message"])
        return reply["ok"]

    def stderr_text(self):
        self._stderr.seek(0)
        return self._stderr.read().decode(errors="replace")

    def start_session(self, kind, chain, **opts):
        return self._call("start_session", kind=kind, chain=chain, **opts)

    def report_hint(self, sid, tx):
        return self._call("report_hint", sid=sid, tx=tx)

    def tick(self):
        return self._call("tick")

    def reverify(self):
        return self._call("reverify")

    def restore_lookup(self, tx):
        return self._call("restore_lookup", tx=tx)

    def restore_apply(self, candidates, choices=None):
        return self._call("restore_apply", candidates=candidates, choices=choices)

    def build_solana_payment(self, sid, account):
        return self._call("build_solana_payment", sid=sid, account=account)

    def snapshot(self):
        return self._call("snapshot")

    def transport_stats(self):
        """{"kind": the transport's class name, "posts": requests made through it}."""
        return self._call("transport_stats")

    # Test-only: the receipt tampering the Python tests do directly on `engine.receipts`.
    def set_receipt(self, key, receipt):
        return self._call("set_receipt", key=key, receipt=receipt)

    def clear_receipts(self):
        return self._call("clear_receipts")

    def close(self):
        """End the driver (EOF on stdin) and release every handle, even if it already exited."""
        try:
            try:
                self._proc.stdin.close()
            except OSError:
                pass  # the driver is gone: nothing left to flush
            try:
                self._proc.wait(5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        finally:
            self._proc.stdout.close()
            self._stderr.close()
