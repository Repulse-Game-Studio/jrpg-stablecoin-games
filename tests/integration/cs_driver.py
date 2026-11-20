"""Drives the C# Engine (unity/Packages/com.repulse.scfg/Runtime/Core) through its JSON-lines
driver process (unity/core-driver), so the Anvil / solana-test-validator fixtures can test that
core too.

Protocol: rpgmaker/test/integration/engine_driver.ts (stdout carries only replies). Methods mirror
the Python Engine's names and return the decoded JSON (Tier 2 `returns` encoding): plain dicts and
lists, amounts as Decimal strings.

dotnet comes from PATH, $DOTNET_ROOT or ~/.dotnet. The driver is built once per Python process
(`dotnet build unity/core-driver -c Release`) and its dll then run with `dotnet <dll>`.
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
PROJECT = ROOT / "unity" / "core-driver"
DLL = PROJECT / "bin" / "Release" / "net8.0" / "Scfg.Core.Driver.dll"
REPLY_TIMEOUT = 120
BUILD_TIMEOUT = 600

_built = None  # (dotnet, None) or (None, why not): the build's outcome, kept for the session


def find_dotnet():
    """Path of the `dotnet` host: PATH, then $DOTNET_ROOT/dotnet, then ~/.dotnet/dotnet; else None."""
    found = shutil.which("dotnet")
    if found:
        return found
    for base in (os.environ.get("DOTNET_ROOT"), os.path.expanduser("~/.dotnet")):
        if base:
            candidate = Path(base) / "dotnet"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def build_driver():
    """(dotnet path, None) once the driver is built, else (None, why not). Builds at most once."""
    global _built
    if _built is None:
        _built = _build()
    return _built


def _build():
    dotnet = find_dotnet()
    if dotnet is None:
        return None, "dotnet not installed (the C# core needs the .NET 8 SDK)"
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1",
               DOTNET_ROOT=os.environ.get("DOTNET_ROOT") or str(Path(dotnet).resolve().parent))
    try:
        out = subprocess.run([dotnet, "build", str(PROJECT), "-c", "Release"], cwd=ROOT, env=env,
                             capture_output=True, text=True, timeout=BUILD_TIMEOUT)
    except (OSError, subprocess.SubprocessError) as e:
        return None, f"dotnet build could not run: {e}"
    if out.returncode != 0 or not DLL.is_file():
        return None, f"dotnet build unity/core-driver failed:\n{out.stdout[-3000:]}{out.stderr[-1000:]}"
    return dotnet, None


def skip_unless_available():
    """Build the driver; skip when dotnet is missing; with SCFG_REQUIRE_CS=1 (CI) fail instead."""
    dotnet, why = build_driver()
    if dotnet is None:
        # A build that ran and failed is a failure whatever the flag says; only a missing dotnet skips.
        broken = why.startswith("dotnet build")
        give_up = pytest.fail if (broken or os.environ.get("SCFG_REQUIRE_CS") == "1") else pytest.skip
        give_up(why)
    return dotnet


class CsEngineError(Exception):
    def __init__(self, type_, code, message):
        super().__init__(f"{type_}({code}): {message}")
        self.type, self.code, self.message = type_, code, message


class CsEngine:
    def __init__(self, override, config, items, custom_chains=()):
        dotnet = skip_unless_available()
        self._stderr = tempfile.TemporaryFile()
        env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1")
        self._proc = subprocess.Popen(
            [dotnet, str(DLL)], cwd=ROOT, env=env,
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
            raise CsEngineError(e["type"], e["code"], e["message"])
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
