import importlib.util
import os
import signal
import stat
import subprocess
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


@pytest.fixture(autouse=True)
def scratch_project(tmp_path, monkeypatch):
    """The default --project is a scratch one: no test touches the real dev project's Intermediate."""
    project = tmp_path / "Proj" / "ScfgDev.uproject"
    monkeypatch.setattr(run_unreal_build, "DEFAULT_PROJECT", project)
    return project


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


@pytest.mark.parametrize("flag, value", [
    ("--target", "*"), ("--target", "ScfgDev/../x"), ("--platform", "/x"),
    ("--platform", "../../Elsewhere/.."), ("--config", "Development/.."), ("--config", ""),
])
def test_a_name_that_is_not_plain_is_refused_before_anything_is_deleted_or_run(
        tmp_path, capsys, scratch_project, flag, value):
    marker = tmp_path / "ran"
    engine = fake_build_sh(tmp_path, f"open({str(marker)!r}, 'w').close()\nsys.exit(0)")
    plan = scratch_project.parent / "Intermediate" / "Build" / "Linux" / "x64" / "ScfgDevEditor" \
        / "Development" / "Makefile.bin"
    plan.parent.mkdir(parents=True)
    plan.write_text("plan")
    assert run_unreal_build.main(["--engine", engine, flag, value]) == 1
    assert "is not a plain name" in capsys.readouterr().err
    assert plan.exists()
    assert not marker.exists()


def test_drop_makefile_refuses_a_plan_that_resolves_outside_the_intermediate(tmp_path):
    project = tmp_path / "Proj" / "ScfgDev.uproject"
    elsewhere = tmp_path / "Elsewhere" / "ScfgDevEditor" / "Development"
    elsewhere.mkdir(parents=True)
    (elsewhere / "Makefile.bin").write_text("someone else's")
    linux = project.parent / "Intermediate" / "Build" / "Linux"
    linux.mkdir(parents=True)
    (linux / "x64").symlink_to(tmp_path / "Elsewhere")
    with pytest.raises(ValueError, match="outside"):
        run_unreal_build.drop_makefile(project, "ScfgDevEditor", "Linux", "Development")
    assert (elsewhere / "Makefile.bin").exists()


def game_link_inputs(project, text, config="Shipping"):
    binary = "ScfgDev" if config == "Development" else f"ScfgDev-Linux-{config}"
    rsp = project.parent / "Intermediate" / "Build" / "Linux" / "x64" / "ScfgDev" / config \
        / f"{binary}.rsp"
    rsp.parent.mkdir(parents=True, exist_ok=True)
    rsp.write_text(text)
    return rsp


GAME_OBJECTS = ('"/p/Intermediate/External/Build/Linux/x64/UnrealGame/Shipping/ScfgCore/'
                'Module.ScfgCore.cpp.o"\n"/p/Intermediate/Build/Linux/x64/UnrealGame/Shipping/'
                'ScfgDev/ScfgDev.cpp.o"\n')


def test_a_game_build_that_links_no_test_module_is_green(tmp_path, scratch_project):
    engine = fake_build_sh(tmp_path, printing("Result: Succeeded\n", 0))
    game_link_inputs(scratch_project, GAME_OBJECTS)
    assert run_unreal_build.main(["--engine", engine, "--target", "ScfgDev",
                                  "--config", "Shipping"]) == 0


@pytest.mark.parametrize("text, why", [
    (GAME_OBJECTS + '"/p/Intermediate/External/Build/Linux/x64/UnrealGame/Shipping/ScfgCoreTests/'
                    'Module.ScfgCoreTests.cpp.o"\n', "links the test-only module ScfgCoreTests"),
    ('"/p/ScfgDev/ScfgDev.cpp.o"\n', "links no ScfgCore object"),
    (None, "no link inputs of ScfgDev-Linux-Shipping"),
])
def test_a_game_build_fails_when_its_link_inputs_hold_a_test_module_or_cannot_be_checked(
        tmp_path, capsys, scratch_project, text, why):
    engine = fake_build_sh(tmp_path, printing("Result: Succeeded\n", 0))
    if text is not None:
        game_link_inputs(scratch_project, text)
    assert run_unreal_build.main(["--engine", engine, "--target", "ScfgDev",
                                  "--config", "Shipping"]) == 1
    assert why in capsys.readouterr().err


def test_a_development_game_is_checked_too(tmp_path, capsys, scratch_project):
    engine = fake_build_sh(tmp_path, printing("Result: Succeeded\n", 0))
    game_link_inputs(scratch_project, GAME_OBJECTS + '"/x/ScfgCoreTests/a.o"\n', "Development")
    assert run_unreal_build.main(["--engine", engine, "--target", "ScfgDev"]) == 1
    assert "test-only module ScfgCoreTests" in capsys.readouterr().err


@pytest.mark.parametrize("sig", [signal.SIGTERM, signal.SIGINT])
def test_a_signal_to_the_wrapper_kills_the_build_and_exits(tmp_path, sig):
    # an outer `timeout` or a Ctrl-C must not leave UBT (or a compiler it started) running
    pids = tmp_path / "pids"
    engine = fake_build_sh(
        tmp_path,
        f"child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
        f"open({str(pids)!r}, 'w').write(f'{{child.pid}}\\n')\n"
        "time.sleep(60)")
    proc = subprocess.Popen([sys.executable, str(TOOL), "--engine", engine, "--project",
                             str(tmp_path / "Proj" / "ScfgDev.uproject")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 10
    while not (pids.exists() and pids.read_text().endswith("\n")):
        assert time.monotonic() < deadline, "the fake build never started"
        time.sleep(0.05)
    proc.send_signal(sig)
    _, err = proc.communicate(timeout=10)
    assert proc.returncode == 128 + sig
    assert "killed the process group" in err
    child = int(pids.read_text())
    deadline = time.monotonic() + 5
    while alive(child):
        assert time.monotonic() < deadline, "the build's child survived the signal"
        time.sleep(0.05)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # a zombie (killed, not yet reaped by its dead parent's reaper) counts as gone
    try:
        return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    except OSError:
        return False


def test_the_log_is_closed_when_a_helper_outlives_the_linger(tmp_path):
    # linger mode: the run ends while a detached helper still writes to the pipe; what it writes
    # afterwards does not reach the closed log
    helper = ("import sys, time; time.sleep(0.8); print('late', flush=True)")
    script = tmp_path / "cmd.py"
    script.write_text("import subprocess, sys\nprint('early', flush=True)\n"
                      f"subprocess.Popen([sys.executable, '-c', {helper!r}], start_new_session=True)\n")
    log = tmp_path / "out.log"
    code, output = run_unreal_build.run_streamed([sys.executable, str(script)], 30, str(log),
                                                 linger=0.2)
    assert code == 0
    assert "early" in output
    time.sleep(1.5)
    assert log.read_text() == "early\n"
