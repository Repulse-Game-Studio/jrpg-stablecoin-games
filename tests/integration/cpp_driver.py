"""Drives the C++ Engine (unreal/Plugins/Scfg/Source/ScfgCore) through its JSON-lines driver
process (unreal/core-driver), so the Anvil / solana-test-validator fixtures can test that core too.

Protocol: rpgmaker/test/integration/engine_driver.ts (stdout carries only replies). Methods mirror
the Python Engine's names and return the decoded JSON (Tier 2 `returns` encoding): plain dicts and
lists, amounts as Decimal strings.

cmake comes from PATH, then ~/.local/bin; the compiler is $CXX, else c++, g++ or clang++ on PATH;
Ninja is used when it is found the same way. The driver is built once per Python process
(`cmake -S unreal -B unreal/build/integration -DCMAKE_BUILD_TYPE=Release`, then only the
scfg_core_driver target) and its binary then run directly.
"""
import json
import os
import select
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "unreal"
BUILD_DIR = SOURCE / "build" / "integration"
TARGET = "scfg_core_driver"
BINARY = BUILD_DIR / TARGET
REPLY_TIMEOUT = 120
BUILD_TIMEOUT = 900
MISSING = "cmake or a C++20 compiler not found"

_built = None  # (binary, None, False) or (None, why not, broken): the build's outcome, kept for the session


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
    """$CXX when it names a program, else c++, g++ or clang++ from PATH; else None."""
    cxx = os.environ.get("CXX")
    if cxx:
        return shutil.which(cxx)
    for name in ("c++", "g++", "clang++"):
        found = shutil.which(name)
        if found:
            return found
    return None


def build_driver():
    """(binary path, None, False) once the driver is built, else (None, why not, whether a build
    ran and failed). Builds at most once."""
    global _built
    if _built is None:
        _built = _build()
    return _built


def _build():
    cmake, cxx = find_tool("cmake"), find_compiler()
    if cmake is None or cxx is None:
        return None, f"{MISSING} (cmake: {cmake or 'missing'}, compiler: {cxx or 'missing'})", False
    configure = [cmake, "-S", str(SOURCE), "-B", str(BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"]
    if not (BUILD_DIR / "CMakeCache.txt").is_file():
        # The generator and the compiler are fixed when the tree is first configured: naming
        # others on a tree configured with different ones would be an error or a reset.
        configure.append(f"-DCMAKE_CXX_COMPILER={cxx}")
        ninja = find_tool("ninja")
        if ninja is not None:
            configure += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    steps = [configure, [cmake, "--build", str(BUILD_DIR), "--target", TARGET]]
    for step in steps:
        try:
            out = subprocess.run(step, cwd=ROOT, capture_output=True, text=True,
                                 timeout=BUILD_TIMEOUT)
        except (OSError, subprocess.SubprocessError) as e:
            return None, f"cmake could not run ({' '.join(step[:2])}): {e}", True
        if out.returncode != 0:
            return None, (f"building {TARGET} failed ({' '.join(step)}):\n"
                          f"{out.stdout[-3000:]}{out.stderr[-2000:]}"), True
    if not (BINARY.is_file() and os.access(BINARY, os.X_OK)):
        return None, f"building {TARGET} made no {BINARY}", True
    return str(BINARY), None, False


def skip_unless_available():
    """Build the driver; skip when cmake or a compiler is missing; with SCFG_REQUIRE_CPP=1 (CI)
    fail instead."""
    binary, why, broken = build_driver()
    if binary is None:
        # A build that ran and failed is a failure whatever the flag says; only a missing toolchain skips.
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
