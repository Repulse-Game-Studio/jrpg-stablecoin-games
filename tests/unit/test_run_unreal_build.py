import importlib.util
import stat
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "run_unreal_build.py"
spec = importlib.util.spec_from_file_location("run_unreal_build", TOOL)
run_unreal_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_unreal_build)
verdict = run_unreal_build.verdict
ROOT = Path("/repo")


def test_green_needs_exit_zero_and_no_diagnostic_of_ours():
    log = ("[1/3] Compile [x64] Module.ScfgCore.cpp\n"
           "/engine/Source/Runtime/Core/Public/X.h:3:1: warning: something in the engine\n"
           "Total execution time: 12.3 seconds\n")
    assert verdict(0, log, ROOT) is None


@pytest.mark.parametrize("code, output, why", [
    (0, "/repo/unreal/Plugins/Scfg/Source/ScfgCore/Private/Json.cpp:12:5: warning: unused 'x'\n",
     "1 warning/error line(s) about our code"),
    (0, "/repo/unreal/Plugins/Scfg/Intermediate/Build/Linux/x64/UnrealEditor/Development/ScfgCore/"
        "Module.ScfgCore.cpp:2:10: error: no such file\n", "about our code"),
    (0, "ScfgDev.Build.cs: Warning: something about our module rules\n", "about our code"),
    (6, "Result: Failed (OtherCompilationError)\n", "UBT exited with 6"),
])
def test_anything_else_fails(code, output, why):
    assert why in verdict(code, output, ROOT)


def fake_build_sh(tmp_path, output, code):
    script = tmp_path / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    script.parent.mkdir(parents=True)
    script.write_text(f"#!{sys.executable}\nimport sys\nprint(' '.join(sys.argv[1:]))\n"
                      f"sys.stdout.write({output!r})\nsys.exit({code})\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return str(tmp_path)


def test_main_passes_a_clean_build_through_and_saves_the_log(tmp_path, capsys):
    engine = fake_build_sh(tmp_path, "Total execution time: 1.0 seconds\n", 0)
    log = tmp_path / "ubt.log"
    args = ["--engine", engine, "--log", str(log), "--", "-DisableUnity"]
    assert run_unreal_build.main(args) == 0
    out = capsys.readouterr().out
    assert "ScfgDevEditor Linux Development -Project=" in out
    assert "-WaitMutex -NoHotReload -UBAHost=127.0.0.1 -DisableUnity" in out
    assert "Total execution time" in log.read_text()


def test_main_fails_on_a_warning_of_ours_even_when_ubt_exits_zero(tmp_path, capsys):
    path = run_unreal_build.ROOT / "unreal/Plugins/Scfg/Source/ScfgCore/Private/Py.cpp"
    engine = fake_build_sh(tmp_path, f"{path}:1:1: warning: shadows a member\n", 0)
    args = ["--engine", engine, "--target", "ScfgDev", "--config", "Shipping"]
    assert run_unreal_build.main(args) == 1
    assert "about our code" in capsys.readouterr().err


def test_main_kills_a_build_that_overruns(tmp_path, capsys):
    engine = fake_build_sh(tmp_path, "", 0)
    script = Path(engine) / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    script.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(60)\n")
    assert run_unreal_build.main(["--engine", engine, "--timeout", "0.5"]) == 1
    assert "did not finish within 0.5 s" in capsys.readouterr().err


def test_main_reports_a_missing_engine(tmp_path, capsys):
    assert run_unreal_build.main(["--engine", str(tmp_path / "nope")]) == 1
    assert "cannot start" in capsys.readouterr().err
