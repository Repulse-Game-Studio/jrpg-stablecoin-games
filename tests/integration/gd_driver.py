"""Drives the GDScript Engine (godot/addons/scfg/core) in a headless Godot through its JSON-lines
driver process, so the Anvil / solana-test-validator fixtures can test that core too.

Protocol: godot/tests/integration/engine_driver.gd (the TypeScript driver's protocol; each reply
line starts with REPLY_PREFIX because Godot prints its own banner on stdout). Methods mirror the
Python Engine's names and return the decoded JSON (Tier 2 `returns` encoding): plain dicts and
lists, amounts as Decimal strings.

The Godot binary comes from SCFG_GODOT (4.3 or newer); the project must have been imported once
(`$SCFG_GODOT --headless --path godot --import`) so its class_name globals are registered.
"""
import json
import os
import select
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "godot"
DRIVER = "res://tests/integration/engine_driver.gd"
REPLY_PREFIX = b"SCFG-REPLY "
REPLY_TIMEOUT = 120
MAX_COMMAND_BYTES = (1 << 20) - 2  # engine_driver.gd's STDIN_BUFFER, less fgets' NUL and the newline


def godot_binary():
    """$SCFG_GODOT when it names an executable file. Skip otherwise; with SCFG_REQUIRE_GD=1 (CI)
    fail instead."""
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_GD") == "1" else pytest.skip
    exe = os.environ.get("SCFG_GODOT", "")
    if not exe:
        give_up("SCFG_GODOT is not set (the path of a Godot 4.3+ binary)")
    if not (os.path.isfile(exe) and os.access(exe, os.X_OK)):
        give_up(f"SCFG_GODOT={exe} is not an executable file")
    return exe


class GdEngineError(Exception):
    def __init__(self, type_, code, message):
        super().__init__(f"{type_}({code}): {message}")
        self.type, self.code, self.message = type_, code, message


class GdEngine:
    def __init__(self, override, config, items, custom_chains=()):
        exe = godot_binary()
        self._stderr = tempfile.TemporaryFile()
        self._noise = []  # stdout lines that are not replies (Godot's banner)
        self._out = b""   # stdout read but not yet split into lines
        self._proc = subprocess.Popen(
            [exe, "--headless", "--path", str(PROJECT), "--script", DRIVER], cwd=ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr)
        try:
            self._call("init", override=override, config=config, items=list(items),
                       custom_chains=list(custom_chains))
        except BaseException:
            self.close()
            raise

    def _call(self, op, **args):
        line = json.dumps({"op": op, **args}).encode()   # ASCII: Godot 4.3 reads stdin as Latin-1
        assert len(line) <= MAX_COMMAND_BYTES, f"{op} command too long for the driver ({len(line)} bytes)"
        self._proc.stdin.write(line + b"\n")
        self._proc.stdin.flush()
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            out = self._read_line(deadline)
            if out is None:
                raise AssertionError(
                    f"driver gave no reply to {op} (has the project been imported? "
                    f"`$SCFG_GODOT --headless --path godot --import`); stdout:\n"
                    + "".join(self._noise) + f"stderr:\n{self.stderr_text()}")
            if out.startswith(REPLY_PREFIX):
                break
            self._noise.append(out.decode(errors="replace"))
        reply = json.loads(out[len(REPLY_PREFIX):])
        if "error" in reply:
            e = reply["error"]
            raise GdEngineError(e["type"], e["code"], e["message"])
        return reply["ok"]

    def _read_line(self, deadline):
        """The next stdout line (with its newline), or None at EOF or the deadline. Reads the pipe
        fd itself (never the buffered reader, which could hold a line select() cannot see)."""
        fd = self._proc.stdout.fileno()
        while b"\n" not in self._out:
            ready, _, _ = select.select([fd], [], [], max(deadline - time.monotonic(), 0))
            chunk = os.read(fd, 1 << 16) if ready else b""
            if not chunk:
                return None
            self._out += chunk
        line, _, self._out = self._out.partition(b"\n")
        return line + b"\n"

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
                self._proc.wait(10)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        finally:
            self._proc.stdout.close()
            self._stderr.close()
