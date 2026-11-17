import importlib.util
import stat
import sys
import time
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "run_unreal_build.py"
spec = importlib.util.spec_from_file_location("run_unreal_build", TOOL)
run_unreal_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_unreal_build)
verdict = run_unreal_build.verdict
ENGINE = Path("/engine")
CORE = "/repo/unreal/Plugins/Scfg/Source/ScfgCore"


def test_green_needs_exit_zero_and_no_diagnostic_outside_the_engine():
    log = ("[1/3] Compile [x64] Module.ScfgCore.cpp\n"
           "/engine/Engine/Source/Runtime/Core/Public/X.h:3:1: warning: the engine's own\n"
           "In file included from /engine/Engine/Source/Runtime/Core/Private/A.cpp:1:\n"
           "/engine/Engine/Source/Runtime/Core/Public/Y.h:9:2: warning: engine, via the engine\n"
           "Total execution time: 12.3 seconds\n")
    assert verdict(0, log, ENGINE) is None


@pytest.mark.parametrize("code, output", [
    (0, f"{CORE}/Private/Json.cpp:12:5: warning: unused 'x'\n"),
    (0, "/repo/unreal/Project/ScfgDev/Intermediate/Build/Linux/x64/UnrealEditor/Development/"
        "ScfgCore/Module.ScfgCore.cpp:2:10: error: no such file\n"),
    # UBT's own messages name no file
    (0, "ScfgDev.Build.cs: Warning: something about our module rules\n"),
    (0, "warning: ScfgDevEditor.Target.cs sets a deprecated property\n"),
    (0, "Warning: Plugin 'Scfg' does not list a supported platform\n"),
    # a relative MSVC-style path that is not an engine file
    (0, "Scfg/Json.h(12): warning C4251: 'scfg::Json::v_' needs a dll-interface\n"),
    # an engine header warning that our file triggers
    (0, f"In file included from {CORE}/Private/Json.cpp:3:\n"
        "/engine/Engine/Source/Runtime/Core/Public/X.h:3:1: warning: deprecated, because of us\n"),
    (0, "In file included from /engine/Engine/Source/Runtime/Core/Public/A.h:1:\n"
        f"                 from {CORE}/Private/Json.cpp:3:\n"
        "/engine/Engine/Source/Runtime/Core/Public/X.h:3:1: warning: two levels down\n"),
])
def test_any_warning_not_confined_to_the_engine_fails(code, output):
    assert "1 warning/error line(s) not confined to the engine" in verdict(code, output, ENGINE)


def test_a_failed_exit_fails():
    assert "UBT exited with 6" in verdict(6, "Result: Failed (OtherCompilationError)\n", ENGINE)


def fake_build_sh(tmp_path, body):
    script = tmp_path / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(f"#!{sys.executable}\nimport subprocess, sys, time\n{body}\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return str(tmp_path)


def printing(output, code):
    return f"print(' '.join(sys.argv[1:]))\nsys.stdout.write({output!r})\nsys.exit({code})"


def test_main_passes_a_clean_build_through_and_saves_the_log(tmp_path, capsys):
    engine = fake_build_sh(tmp_path, printing("Total execution time: 1.0 seconds\n", 0))
    log = tmp_path / "ubt.log"
    args = ["--engine", engine, "--log", str(log), "--", "-DisableUnity"]
    assert run_unreal_build.main(args) == 0
    out = capsys.readouterr().out
    assert "ScfgDevEditor Linux Development -Project=" in out
    assert "-WaitMutex -NoHotReload -UBAHost=127.0.0.1 -DisableUnity" in out
    assert "Total execution time" in log.read_text()


def test_main_fails_on_a_warning_of_ours_even_when_ubt_exits_zero(tmp_path, capsys):
    path = run_unreal_build.ROOT / "unreal/Plugins/Scfg/Source/ScfgCore/Private/Py.cpp"
    engine = fake_build_sh(tmp_path, printing(f"{path}:1:1: warning: shadows a member\n", 0))
    args = ["--engine", engine, "--target", "ScfgDev", "--config", "Shipping"]
    assert run_unreal_build.main(args) == 1
    assert "not confined to the engine" in capsys.readouterr().err


def test_main_kills_a_build_that_overruns(tmp_path, capsys):
    engine = fake_build_sh(tmp_path, "time.sleep(60)")
    started = time.monotonic()
    assert run_unreal_build.main(["--engine", engine, "--timeout", "0.5"]) == 1
    assert time.monotonic() - started < 10
    assert "did not finish within 0.5 s" in capsys.readouterr().err


def test_main_kills_a_grandchild_that_outlives_build_sh(tmp_path, capsys):
    # Build.sh exits at once, leaving a descendant that holds its stdout (R1)
    marker = tmp_path / "survived"
    grandchild = tmp_path / "grandchild.py"
    grandchild.write_text(f"import time\ntime.sleep(4)\nopen({str(marker)!r}, 'w').close()\n")
    engine = fake_build_sh(tmp_path, f"subprocess.Popen([sys.executable, {str(grandchild)!r}])\n"
                                     f"sys.exit(0)")
    started = time.monotonic()
    assert run_unreal_build.main(["--engine", engine, "--timeout", "1"]) == 1
    assert time.monotonic() - started < 3.5
    assert "did not finish within 1 s" in capsys.readouterr().err
    time.sleep(4)
    assert not marker.exists()   # the grandchild was killed with the group


def test_main_gives_up_on_a_holder_outside_the_group(tmp_path, capsys, monkeypatch):
    # a descendant in a session of its own cannot be killed through the group: the wrapper still
    # returns, after the grace period, and fails
    monkeypatch.setattr(run_unreal_build, "KILL_GRACE", 0.3)
    engine = fake_build_sh(tmp_path, "subprocess.Popen(['sleep', '5'], start_new_session=True)\n"
                                     "sys.exit(0)")
    started = time.monotonic()
    assert run_unreal_build.main(["--engine", engine, "--timeout", "0.5"]) == 1
    assert time.monotonic() - started < 3
    assert "did not finish within 0.5 s" in capsys.readouterr().err


def test_main_reports_a_missing_engine(tmp_path, capsys):
    assert run_unreal_build.main(["--engine", str(tmp_path / "nope")]) == 1
    assert "cannot start" in capsys.readouterr().err


def test_main_drops_the_targets_cached_build_plan_only(tmp_path):
    # UBT reuses Makefile.bin even under -NoUBTMakefiles and after a .uplugin or -DisableUnity
    # change: the wrapper deletes this target's one, and leaves the others
    engine = fake_build_sh(tmp_path, printing("Result: Succeeded\n", 0))
    project = tmp_path / "Proj" / "ScfgDev.uproject"
    build = tmp_path / "Proj" / "Intermediate" / "Build" / "Linux"
    ours = build / "x64" / "ScfgDevEditor" / "Development" / "Makefile.bin"
    other = build / "x64" / "ScfgDev" / "Shipping" / "Makefile.bin"
    for f in (ours, other):
        f.parent.mkdir(parents=True)
        f.write_text("plan")
    assert run_unreal_build.main(["--engine", engine, "--project", str(project)]) == 0
    assert not ours.exists()
    assert other.exists()
