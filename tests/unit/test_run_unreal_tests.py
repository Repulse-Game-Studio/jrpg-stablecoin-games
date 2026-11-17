import importlib.util
import json
import stat
import sys
import time
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "run_unreal_tests.py"
spec = importlib.util.spec_from_file_location("run_unreal_tests", TOOL)
run_unreal_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_unreal_tests)


def make_report(passed=0, failed=(), not_run=0, warned=0):
    """An index.json as UE 5.8 writes it: counts, then one entry per test."""
    tests = [{"fullTestPath": f"Scfg.Core.Tier1.t{n}", "state": "Success", "entries": []}
             for n in range(passed)]
    tests += [{"fullTestPath": path, "state": "Fail",
               "entries": [{"event": {"type": "Info", "message": "context"}},
                           {"event": {"type": "Error", "message": f"{message}\nsecond line"}}]}
              for path, message in failed]
    return {"succeeded": passed, "succeededWithWarnings": warned, "failed": len(failed),
            "notRun": not_run, "inProcess": 0, "tests": tests}


def fake_editor(tmp_path, body="", report=None, code=0):
    """An engine whose UnrealEditor-Cmd records its argv, runs body, writes report (UTF-8 with a
    BOM, as the editor does) into -ReportExportPath and exits with code."""
    editor = tmp_path / "Engine" / "Binaries" / "Linux" / "UnrealEditor-Cmd"
    editor.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(report) if report is not None else None
    editor.write_text(
        f"#!{sys.executable}\n"
        "import json, pathlib, subprocess, sys, time\n"
        f"pathlib.Path({str(tmp_path / 'argv.json')!r}).write_text(json.dumps(sys.argv[1:]))\n"
        "out = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('-ReportExportPath='))\n"
        "print('LogAutomationController: Display: Test Completed.')\n"
        f"{body}\n"
        f"text = {text!r}\n"
        "if text is not None:\n"
        "    pathlib.Path(out, 'index.json').write_text(text, encoding='utf-8-sig')\n"
        f"sys.exit({code})\n")
    editor.chmod(editor.stat().st_mode | stat.S_IXUSR)
    return str(tmp_path)


def run(tmp_path, *extra):
    args = ["--engine", str(tmp_path), "--project", str(tmp_path / "P.uproject"), "--no-build",
            "--report-dir", str(tmp_path / "report"), *extra]
    return run_unreal_tests.main(args)


def test_all_passed_is_green_and_the_command_is_headless(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=3))
    assert run(tmp_path) == 0
    assert "3 passed, 0 failed" in capsys.readouterr().out
    argv = json.loads((tmp_path / "argv.json").read_text())
    assert argv[0] == str((tmp_path / "P.uproject").resolve())
    assert argv[1] == "-ExecCmds=Automation RunTests Scfg.Core;Quit"
    for flag in ("-unattended", "-nullrhi", "-nosplash", "-stdout", "-notraceserver"):
        assert flag in argv
    assert f"-ReportExportPath={(tmp_path / 'report').resolve()}" in argv


def test_the_filter_is_passed_through(tmp_path):
    fake_editor(tmp_path, report=make_report(passed=1))
    assert run(tmp_path, "--filter", "Scfg.Core.Tier1") == 0
    argv = json.loads((tmp_path / "argv.json").read_text())
    assert "-ExecCmds=Automation RunTests Scfg.Core.Tier1;Quit" in argv


def test_a_failed_test_fails_and_is_named_with_its_first_error(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=2, failed=[("Scfg.Core.Tier2.x", "step 0")]),
                code=255)
    assert run(tmp_path) == 1
    out = capsys.readouterr()
    assert "2 passed, 1 failed" in out.out
    assert "Scfg.Core.Tier2.x: Fail: step 0" in out.err
    assert "second line" not in out.err


def test_no_test_at_all_fails(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=0))
    assert run(tmp_path) == 1
    err = capsys.readouterr().err
    assert "no test ran" in err


def test_fewer_than_expected_fails(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=3))
    assert run(tmp_path, "--expect", "4") == 1
    assert "expected at least 4 tests, 3 ran" in capsys.readouterr().err
    assert run(tmp_path, "--expect", "3") == 0


def test_a_test_left_unrun_fails(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=3, not_run=1))
    assert run(tmp_path) == 1
    assert "3 passed, 0 failed, 1 not run" in capsys.readouterr().out


def test_warnings_count_as_passed(tmp_path, capsys):
    report = make_report(passed=2, warned=1)
    fake_editor(tmp_path, report=report)
    assert run(tmp_path) == 0
    assert "3 passed, 0 failed" in capsys.readouterr().out


def test_a_non_zero_exit_fails_even_when_every_test_passed(tmp_path, capsys):
    fake_editor(tmp_path, report=make_report(passed=3), code=3)
    assert run(tmp_path) == 1
    assert "the editor exited with 3" in capsys.readouterr().err


def test_no_report_fails_and_a_stale_one_is_not_read(tmp_path, capsys):
    stale = tmp_path / "report" / "index.json"
    stale.parent.mkdir()
    stale.write_text(json.dumps(make_report(passed=5)))
    fake_editor(tmp_path, report=None)
    assert run(tmp_path) == 1
    assert "the editor wrote no report" in capsys.readouterr().err
    assert not stale.exists()


def test_a_malformed_report_fails(tmp_path, capsys):
    fake_editor(tmp_path, body="pathlib.Path(out, 'index.json').write_text('{oops')")
    assert run(tmp_path) == 1
    assert "cannot be read" in capsys.readouterr().err


def test_an_editor_that_overruns_is_killed(tmp_path, capsys):
    fake_editor(tmp_path, body="time.sleep(60)", report=make_report(passed=1))
    started = time.monotonic()
    assert run(tmp_path, "--timeout", "0.5") == 1
    assert time.monotonic() - started < 10
    assert "did not finish within 0.5 s" in capsys.readouterr().err


def test_a_helper_holding_the_output_does_not_hold_the_run(tmp_path, monkeypatch, capsys):
    # a detached helper of the editor (zen's DDC service) may keep stdout open after the editor
    # exits: the run ends LINGER seconds later, and is judged by the report
    monkeypatch.setattr(run_unreal_tests, "LINGER", 0.3)
    fake_editor(tmp_path, body="subprocess.Popen(['sleep', '5'], start_new_session=True)",
                report=make_report(passed=2))
    started = time.monotonic()
    assert run(tmp_path, "--timeout", "30") == 0
    assert time.monotonic() - started < 3
    assert "2 passed, 0 failed" in capsys.readouterr().out


def test_the_editor_target_is_built_first_and_a_failed_build_stops(tmp_path, capsys):
    build_sh = tmp_path / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    build_sh.parent.mkdir(parents=True)
    build_sh.write_text(f"#!{sys.executable}\nimport sys\nprint(' '.join(sys.argv[1:]))\n"
                        "sys.exit(6)\n")
    build_sh.chmod(build_sh.stat().st_mode | stat.S_IXUSR)
    fake_editor(tmp_path, report=make_report(passed=1))
    args = ["--engine", str(tmp_path), "--project", str(tmp_path / "P.uproject"),
            "--report-dir", str(tmp_path / "report")]
    assert run_unreal_tests.main(args) == 1
    out = capsys.readouterr()
    assert "ScfgDevEditor Linux Development -Project=" in out.out
    assert "the build failed: UBT exited with 6" in out.err
    assert not (tmp_path / "argv.json").exists()   # the editor never ran

    build_sh.write_text(f"#!{sys.executable}\nprint('Result: Succeeded')\n")
    assert run_unreal_tests.main(args) == 0
    assert (tmp_path / "argv.json").exists()


def test_a_missing_editor_fails(tmp_path, capsys):
    assert run(tmp_path) == 1
    assert "cannot start" in capsys.readouterr().err
