"""The Unreal desktop e2e runner's helpers (tools/run_unreal_e2e.py), without Unreal, X or a chain:
the guard that keeps the game off any display but this run's Xvfb, the environments, the config
and command line the game gets, the log check, and the process-group and session cleanup."""

import json
import os
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_unreal_e2e as e2e  # noqa: E402

OUTER, XVFB_RUN, ME, XVFB = 100, 200, 300, 301


def table(*extra, display=":99", xvfb_parent=XVFB_RUN, xvfb_name="/usr/bin/Xvfb"):
    """A process table: the outer run, xvfb-run under it, this run and the Xvfb under xvfb-run."""
    return [
        e2e.Proc(OUTER, 1, ("python3", "tools/run_unreal_e2e.py")),
        e2e.Proc(XVFB_RUN, OUTER, ("/bin/sh", "/usr/bin/xvfb-run", "-a")),
        e2e.Proc(ME, XVFB_RUN, ("python3", "tools/run_unreal_e2e.py")),
        e2e.Proc(XVFB, xvfb_parent, (xvfb_name, display, "-screen", "0", "1600x900x24")),
        *extra,
    ]


def test_own_xvfb_accepts_the_xvfb_this_runs_xvfb_run_started():
    assert e2e.own_xvfb(":99", ME, OUTER, table()) is None


@pytest.mark.parametrize("display", [None, "", ":0.0", "localhost:10.0", "unix:0", ":99x"])
def test_own_xvfb_refuses_anything_but_a_plain_local_display(display):
    assert "DISPLAY is" in e2e.own_xvfb(display, ME, OUTER, table())


def test_own_xvfb_refuses_the_developers_display():
    """DISPLAY=:0 with the run's own Xvfb on :99: the developer's session, refused."""
    why = e2e.own_xvfb(":0", ME, OUTER, table())
    assert why is not None and ":0" in why


def test_own_xvfb_refuses_an_xvfb_someone_else_started():
    other = e2e.Proc(999, 1, ("/usr/bin/Xvfb", ":5"))
    assert e2e.own_xvfb(":5", ME, OUTER, table(other)) is not None
    assert e2e.own_xvfb(":99", ME, OUTER, table(xvfb_parent=1)) is not None


def test_own_xvfb_refuses_a_run_not_started_by_the_outer_run():
    """A developer who sets the inner variables by hand and runs xvfb-run himself: refused."""
    assert "not started by xvfb-run" in e2e.own_xvfb(":99", ME, OUTER + 1, table())
    assert "not in the process table" in e2e.own_xvfb(":99", 12345, OUTER, table())


def test_own_xvfb_wants_an_xvfb_binary_not_a_lookalike():
    assert e2e.own_xvfb(":99", ME, OUTER, table(xvfb_name="/tmp/Xvfb-fake")) is not None


def test_read_processes_parses_a_command_name_with_spaces_and_parentheses(tmp_path):
    proc = tmp_path / "proc"
    (proc / "42").mkdir(parents=True)
    (proc / "42" / "stat").write_text("42 (Web Content (x)) S 7 42 42 0 -1\n")
    (proc / "42" / "cmdline").write_bytes(b"/usr/bin/Xvfb\0:3\0-screen\0")
    (proc / "self").mkdir()
    assert e2e.read_processes(proc) == [e2e.Proc(42, 7, ("/usr/bin/Xvfb", ":3", "-screen"))]


def test_the_outer_run_reruns_itself_under_xvfb_run_without_the_desktop():
    cmd = e2e.outer_command(["--family", "arc"])
    assert cmd[:2] == ["xvfb-run", "-a"] and cmd[-2:] == ["--family", "arc"]
    env = e2e.outer_env({"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0", "XAUTHORITY": "/x",
                         "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/bus", "PATH": "/bin",
                         "LD_LIBRARY_PATH": "/appimage/lib"}, "tok", 4242)
    assert env == {"PATH": "/bin", e2e.INNER_ENV: "tok", e2e.OUTER_PID_ENV: "4242"}


def test_the_game_environment_forces_lavapipe_and_isolates_the_game(tmp_path):
    env = e2e.game_env({"DISPLAY": ":99", "XAUTHORITY": "/tmp/xvfb-run.x/Xauthority",
                        "WAYLAND_DISPLAY": "wayland-0", "DBUS_SESSION_BUS_ADDRESS": "unix:x",
                        "LD_LIBRARY_PATH": "/appimage/lib", "HOME": "/home/dev", "PATH": "/bin"},
                       tmp_path / "home")
    assert env["DISPLAY"] == ":99" and env["XAUTHORITY"] == "/tmp/xvfb-run.x/Xauthority"
    assert env["VK_ICD_FILENAMES"] == env["VK_DRIVER_FILES"] == e2e.LAVAPIPE_ICD
    assert env["SDL_VIDEODRIVER"] == "x11"
    for gone in ("WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH"):
        assert gone not in env
    for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        assert Path(env[key]).is_relative_to(tmp_path / "home"), key


def test_the_config_names_the_control_socket_and_the_chain_and_is_private(tmp_path):
    path = tmp_path / "e2e.json"
    family = {"configure": {"wallet": "0xabc"}, "items": {"chapter2": {"code": 1}}}
    chain = {"network": "mainnet", "key": "arbitrum", "rpc": ["http://127.0.0.1:1"]}
    e2e.write_config(path, family, chain, 4321, "secret")
    assert json.loads(path.read_text()) == {**family, "chain": chain,
                                            "control": {"port": 4321, "token": "secret"}}
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    e2e.write_config(path, family, None, 4321, "secret")
    assert "chain" not in json.loads(path.read_text())


def test_the_game_command_renders_on_the_cpu_and_never_headless(tmp_path):
    engine = Path("/ue")
    cmd = e2e.game_command(engine, tmp_path / "c.json", tmp_path / "user")
    assert cmd[0] == "/ue/Engine/Binaries/Linux/UnrealEditor" and cmd[1:3] == [str(e2e.PROJECT),
                                                                               "-game"]
    for flag in ("-AllowCPUDevices", "-vulkan", "-windowed", f"-scfg-e2e={tmp_path / 'c.json'}",
                 f"-UserDir={tmp_path / 'user'}", "-NoZenAutoLaunch", "-notraceserver"):
        assert flag in cmd, flag
    assert not any(a.lower() in ("-nullrhi", "-renderoffscreen") for a in cmd)
    staged = Path("/archive/Linux/ScfgDev/Binaries/Linux/ScfgDev")
    packaged = e2e.game_command(engine, tmp_path / "c.json", tmp_path / "user", staged)
    assert packaged[:2] == [str(staged), "ScfgDev"] and "-game" not in packaged
    assert "-AllowCPUDevices" in packaged


def test_problems_are_our_errors_and_warnings_and_crashes_only():
    log = "\n".join([
        "[1][0]LogScfg: scfg: the payment state file is /tmp/x/state.json",
        "[1][0]LogEditorDataStorageUI: Warning: Unable to register widget factory 'X'",
        "[1][0]LogScfg: Error: scfg: worker error: boom",
        "[1][0]LogScfgDev: Warning: showcase: no game viewport, no HUD",
        "[1][0]LogOutputDevice: Error: Ensure condition failed: x",
    ])
    assert e2e.problems(log) == log.splitlines()[2:]


def test_the_packaged_build_command_stages_a_development_game(tmp_path):
    cmd = e2e.buildcookrun_command(Path("/ue"), tmp_path / "archive")
    assert cmd[0] == "/ue/Engine/Build/BatchFiles/RunUAT.sh" and cmd[1] == "BuildCookRun"
    for flag in ("-platform=Linux", "-clientconfig=Development", "-build", "-cook", "-stage",
                 "-pak", "-archive", f"-archivedirectory={tmp_path / 'archive'}",
                 "-UBAHost=127.0.0.1"):
        assert flag in cmd, flag


def test_staged_game_finds_the_binary_or_fails(tmp_path):
    with pytest.raises(e2e.E2EError):
        e2e.staged_game(tmp_path)
    binary = tmp_path / "Linux" / "ScfgDev" / "Binaries" / "Linux" / "ScfgDev"
    binary.parent.mkdir(parents=True)
    binary.write_text("")
    assert e2e.staged_game(tmp_path) == binary


def _wait_gone(pids, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        alive = []
        for pid in pids:
            try:
                os.kill(pid, 0)
                with open(f"/proc/{pid}/stat") as f:
                    if f.read().split(") ")[1].startswith("Z"):
                        continue  # a zombie waiting for its reaper is gone
                alive.append(pid)
            except (ProcessLookupError, FileNotFoundError):
                pass
        if not alive:
            return []
        time.sleep(0.05)
    return alive


def test_kill_group_kills_the_child_and_what_it_started(tmp_path):
    """A game's helper processes (a shader compiler) are in its group: they go with it, even
    when they ignore SIGTERM."""
    pids = tmp_path / "pids"
    proc = subprocess.Popen(
        ["sh", "-c", f"trap '' TERM; sleep 60 & echo $! > {pids}; trap '' TERM; wait"],
        **e2e.new_group_kwargs())
    deadline = time.monotonic() + 5
    while not pids.exists() or not pids.read_text().strip():
        assert time.monotonic() < deadline
        time.sleep(0.05)
    child = int(pids.read_text())
    assert os.getpgid(child) == proc.pid != os.getpgid(0)
    e2e.kill_group(proc.pid, grace=0.5)
    proc.wait(5)
    assert _wait_gone([child]) == []
    assert not e2e.group_alive(proc.pid)


def test_kill_session_kills_every_process_of_the_runs_session(tmp_path):
    """The outer run's last step: whatever is left in the session (a chain node, a browser, a
    game in its own process group) is killed."""
    pids = tmp_path / "pids"
    proc = subprocess.Popen(
        ["sh", "-c", f"sleep 60 & echo $! >> {pids}; (sleep 62 & echo $! >> {pids}; wait) & wait"],
        start_new_session=True)
    deadline = time.monotonic() + 5
    while not pids.exists() or len(pids.read_text().split()) < 2:
        assert time.monotonic() < deadline
        time.sleep(0.05)
    members = e2e.session_members(proc.pid)
    started = [int(p) for p in pids.read_text().split()]
    assert proc.pid in members and set(started) <= set(members)
    assert e2e.kill_session(proc.pid) >= 3
    proc.wait(5)
    assert _wait_gone(started) == []
    assert e2e.session_members(proc.pid) == []


def test_the_inner_run_refuses_a_display_that_is_not_its_own(monkeypatch, capsys):
    """Started by hand with the inner variable set (or on a developer's DISPLAY): no game, no
    chain, no build."""
    monkeypatch.setenv(e2e.INNER_ENV, "x")
    monkeypatch.setenv(e2e.OUTER_PID_ENV, "1")
    monkeypatch.setenv("DISPLAY", ":0")
    monkeypatch.setattr(e2e, "read_processes", lambda: table())

    def must_not_run(*a, **kw):
        raise AssertionError("the run started")

    monkeypatch.setattr(e2e.tempfile, "mkdtemp", must_not_run)
    monkeypatch.setattr(e2e.subprocess, "Popen", must_not_run)
    assert e2e.main(["--family", "solana"]) == 2
    assert "refusing to start a game" in capsys.readouterr().out


def test_the_outer_run_needs_xvfb_run(monkeypatch, capsys):
    monkeypatch.delenv(e2e.INNER_ENV, raising=False)
    monkeypatch.setattr(e2e.shutil, "which", lambda name: None)
    assert e2e.main([]) == 2
    assert "xvfb-run not found" in capsys.readouterr().out


def test_the_outer_run_kills_its_session_when_done(monkeypatch):
    """The inner run is started in a session of its own; at the end every process left in it is
    killed, and the inner run's exit code is returned."""
    monkeypatch.delenv(e2e.INNER_ENV, raising=False)
    monkeypatch.setattr(e2e.shutil, "which", lambda name: "/usr/bin/" + name)
    seen = {}
    real_popen = subprocess.Popen

    def fake_popen(cmd, env, start_new_session):
        seen["cmd"], seen["env"], seen["new"] = cmd, env, start_new_session
        return real_popen(["sh", "-c", "sleep 60 & exit 3"], start_new_session=True)

    monkeypatch.setattr(e2e.subprocess, "Popen", fake_popen)
    killed = []
    real_kill_session = e2e.kill_session
    monkeypatch.setattr(e2e, "kill_session", lambda sid: killed.append(real_kill_session(sid)))
    saved = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)}
    try:
        assert e2e.main(["--family", "evm"]) == 3
    finally:
        for s, handler in saved.items():
            signal.signal(s, handler)
    assert seen["cmd"][:2] == ["xvfb-run", "-a"] and seen["new"] is True
    assert seen["env"][e2e.OUTER_PID_ENV] == str(os.getpid()) and "DISPLAY" not in seen["env"]
    assert killed == [1]  # the background sleep
