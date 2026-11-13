import importlib.util
import stat
import sys
import time
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "run_godot_tests.py"
spec = importlib.util.spec_from_file_location("run_godot_tests", TOOL)
run_godot_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_godot_tests)
verdict = run_godot_tests.verdict


def test_green_needs_a_pass_no_failure_and_exit_zero():
    assert verdict(0, "PASS a (3 checks)\n635 passed, 0 failed, 0 skipped\n") is None
    assert verdict(0, "12 passed, 0 failed, 3 skipped\n") is None


@pytest.mark.parametrize("code, output, why", [
    # Godot exits 0 when run.gd fails to compile: no summary line at all
    (0, "SCRIPT ERROR: Parse Error: Could not resolve script\n", "no summary line"),
    (0, "", "no summary line"),
    (1, "10 passed, 2 failed, 0 skipped\n", "2 test(s) failed"),
    (1, "0 passed, 0 failed, 4 skipped\n", "no test passed"),
    (1, "10 passed, 0 failed, 0 skipped\n", "exited with 1"),
    # the last summary counts (a test printing a look-alike line cannot mask a failure)
    (0, "1 passed, 0 failed, 0 skipped\n9 passed, 1 failed, 0 skipped\n", "1 test(s) failed"),
    (0, "summary: 10 passed, 0 failed, 0 skipped\n", "no summary line"),
])
def test_anything_else_fails(code, output, why):
    assert why in verdict(code, output)


def fake_godot(tmp_path, output, code):
    exe = tmp_path / "godot"
    exe.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.write({output!r})\nsys.exit({code})\n")
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    return str(exe)


def test_main_fails_closed_on_a_compile_error_that_exits_zero(tmp_path, capsys):
    exe = fake_godot(tmp_path, "SCRIPT ERROR: Parse Error\n", 0)
    assert run_godot_tests.main(["--godot", exe]) == 1
    out = capsys.readouterr()
    assert "SCRIPT ERROR" in out.out and "no summary line" in out.err


def test_main_passes_a_green_run_through(tmp_path, capsys):
    exe = fake_godot(tmp_path, "PASS x (1 checks)\n1 passed, 0 failed, 0 skipped\n", 0)
    assert run_godot_tests.main(["--godot", exe]) == 0
    assert "1 passed" in capsys.readouterr().out


def test_main_reports_a_missing_binary(tmp_path, capsys):
    assert run_godot_tests.main(["--godot", str(tmp_path / "nope")]) == 1
    assert "cannot start" in capsys.readouterr().err


def hanging_godot(tmp_path, output):
    """A fake binary that prints `output` and then never exits on its own."""
    exe = tmp_path / "godot"
    exe.write_text(f"#!{sys.executable}\nimport sys, time\nsys.stdout.write({output!r})\nsys.stdout.flush()\n"
                   "time.sleep(600)\n")
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    return str(exe)


def test_main_kills_a_hung_godot_and_fails(tmp_path, capsys):
    exe = hanging_godot(tmp_path, "PASS x (1 checks)\n")
    started = time.monotonic()
    assert run_godot_tests.main(["--godot", exe, "--timeout", "1"]) == 1
    assert time.monotonic() - started < 30
    out = capsys.readouterr()
    assert "PASS x" in out.out   # what it printed before hanging is still passed through
    assert "did not finish within 1 s and was killed" in out.err


def test_a_hang_after_a_green_summary_still_fails(tmp_path, capsys):
    exe = hanging_godot(tmp_path, "1 passed, 0 failed, 0 skipped\n")
    assert run_godot_tests.main(["--godot", exe, "--timeout", "1"]) == 1
    assert "was killed" in capsys.readouterr().err


def test_the_timeout_defaults_to_900_seconds_and_must_be_positive(tmp_path):
    assert run_godot_tests.DEFAULT_TIMEOUT == 900
    exe = fake_godot(tmp_path, "1 passed, 0 failed, 0 skipped\n", 0)
    with pytest.raises(SystemExit):
        run_godot_tests.main(["--godot", exe, "--timeout", "0"])
