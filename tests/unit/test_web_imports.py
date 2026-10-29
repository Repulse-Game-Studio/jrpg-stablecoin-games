"""Ren'Py's web build has no ssl module. The plugin must still load there: payments in
the browser go through fetch(), and owns() reads receipts already on the device."""
import subprocess
import sys

# Every scfg_core module that scfg_api.rpy imports at init time.
INIT_MODULES = [
    "scfg_core", "scfg_core.config", "scfg_core.errors", "scfg_core.models",
    "scfg_core.payments", "scfg_core.receipts", "scfg_core.engine",
    "scfg_core.worker", "scfg_core.rpc", "scfg_core.checkout",
    "scfg_core.restore_flow", "scfg_core.paylinks", "scfg_core.coio",
    "scfg_core.pump_worker", "scfg_core.overlay", "scfg_core.web_page",
    "scfg_core.page_messages",
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
