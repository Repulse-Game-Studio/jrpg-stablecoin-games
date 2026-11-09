"""Drives the TypeScript Engine (rpgmaker/src/core) through its JSON-lines driver process,
so the Anvil / solana-test-validator fixtures can test both cores.

Protocol: rpgmaker/test/integration/engine_driver.ts. Methods mirror the Python Engine's names
and return the decoded JSON (Tier 2 `returns` encoding): plain dicts and lists, amounts as
Decimal strings.
"""
import json
import os
import re
import select
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RPGMAKER = ROOT / "rpgmaker"
DRIVER = RPGMAKER / "test" / "integration" / "engine_driver.ts"
REPLY_TIMEOUT = 120
MIN_NODE = (22, 18)  # runs .ts directly (type stripping)


def node_version(node):
    """(major, minor) of `node --version`, or None if it can't be read."""
    try:
        out = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.match(r"v(\d+)\.(\d+)", out.strip())
    return (int(m.group(1)), int(m.group(2))) if m else None


def skip_unless_available():
    """Skip when the JS core can't run here; with SCFG_REQUIRE_JS=1 (CI) fail instead."""
    give_up = pytest.fail if os.environ.get("SCFG_REQUIRE_JS") == "1" else pytest.skip
    need = "the JS core needs Node {}.{} or newer".format(*MIN_NODE)
    node = shutil.which("node")
    if node is None:
        give_up(f"node not installed ({need})")
    version = node_version(node)
    if version is None or version < MIN_NODE:
        found = "unknown" if version is None else "{}.{}".format(*version)
        give_up(f"node {found} is too old or unreadable ({need})")
    if not (RPGMAKER / "node_modules").is_dir():
        give_up("rpgmaker/node_modules missing (cd rpgmaker && npm ci)")


class JsEngineError(Exception):
    def __init__(self, type_, code, message):
        super().__init__(f"{type_}({code}): {message}")
        self.type, self.code, self.message = type_, code, message


class JsEngine:
    def __init__(self, override, config, items, custom_chains=()):
        skip_unless_available()
        self._stderr = tempfile.TemporaryFile()
        self._proc = subprocess.Popen(
            [shutil.which("node"), str(DRIVER)], cwd=RPGMAKER,
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
            raise JsEngineError(e["type"], e["code"], e["message"])
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
