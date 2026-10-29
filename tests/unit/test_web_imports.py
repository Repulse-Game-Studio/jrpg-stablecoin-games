"""Ren'Py's web build has no ssl module. The plugin must still load there: payments in
the browser go through fetch(), and owns() reads receipts already on the device."""
import subprocess
import sys

# Every cryptopay_core module that cryptopay_api.rpy imports at init time.
INIT_MODULES = [
    "cryptopay_core", "cryptopay_core.config", "cryptopay_core.errors", "cryptopay_core.models",
    "cryptopay_core.payments", "cryptopay_core.receipts", "cryptopay_core.engine",
    "cryptopay_core.worker", "cryptopay_core.rpc", "cryptopay_core.checkout",
    "cryptopay_core.restore_flow", "cryptopay_core.paylinks", "cryptopay_core.coio",
    "cryptopay_core.pump_worker", "cryptopay_core.overlay", "cryptopay_core.web_page",
    "cryptopay_core.page_messages",
]


def test_init_imports_work_without_ssl():
    code = "import sys\nsys.modules['_ssl'] = None\nsys.modules['ssl'] = None\n" + "".join(
        f"import {m}\n" for m in INIT_MODULES)
    proc = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                          env={"PYTHONPATH": "plugin/game/python-packages"})
    assert proc.returncode == 0, proc.stderr


def test_init_imports_start_no_threads():
    code = ("import threading\nbefore = threading.active_count()\n" + "".join(
        f"import {m}\n" for m in INIT_MODULES) + "assert threading.active_count() == before\n")
    proc = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                          env={"PYTHONPATH": "plugin/game/python-packages"})
    assert proc.returncode == 0, proc.stderr
