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
    assert e2e.read_processes(proc) == [e2e.Proc(42, 7, ("/usr/bin/Xvfb", ":3", "-screen"), 42, "S")]


def test_run_tree_is_the_session_and_every_descendant_also_in_other_sessions():
    """The browser Playwright starts in a session of its own, and what it starts, are ended with
    the run; a zombie, this process and an unrelated process are not."""
    procs = [
        e2e.Proc(10, 1, ("xvfb-run",), sid=10),
        e2e.Proc(11, 10, ("python",), sid=10),
        e2e.Proc(12, 11, ("node",), sid=10),
        e2e.Proc(13, 12, ("chrome",), sid=13),
        e2e.Proc(14, 13, ("chrome", "--type=renderer"), sid=13),
        e2e.Proc(15, 11, ("anvil",), sid=10, state="Z"),
        e2e.Proc(16, 1, ("unrelated",), sid=16),
        e2e.Proc(17, 1, ("orphan of ours",), sid=10),
        e2e.Proc(99, 1, ("outer",), sid=99),
    ]
    assert e2e.run_tree(10, procs, me=11) == [10, 12, 13, 14, 17]


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
    for gone in ("WAYLAND_DISPLAY", "LD_LIBRARY_PATH"):
        assert gone not in env
    # an address that goes nowhere: with none, libdbus would autolaunch a bus (and portals, gvfs,
    # the accessibility bus) in sessions of their own
    assert env["DBUS_SESSION_BUS_ADDRESS"] == e2e.NO_BUS and not Path(e2e.NO_BUS[10:]).exists()
    assert env["NO_AT_BRIDGE"] == "1" and env["GVFS_DISABLE_FUSE"] == "1"
    assert env["GIO_USE_VFS"] == "local"
    for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        assert Path(env[key]).is_relative_to(tmp_path / "home"), key


def test_the_config_names_the_control_socket_and_the_chain_and_is_private(tmp_path):
    path = tmp_path / "e2e.json"
    family = {"configure": {"wallet": "0xabc"}, "items": {"chapter2": {"code": 1}}}
    chain = {"network": "mainnet", "key": "arbitrum", "rpc": ["http://127.0.0.1:1"]}
    e2e.write_config(path, family, chain, 4321, "secret")
    assert json.loads(path.read_text()) == {**family, "chain": chain, "draw_world": False,
                                            "control": {"port": 4321, "token": "secret"}}
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    e2e.write_config(path, family, None, 4321, "secret")
    assert "chain" not in json.loads(path.read_text())


def test_the_tour_config_leaves_the_configuration_to_the_showcase_and_is_private(tmp_path):
    path = tmp_path / "tour.json"
    e2e.write_tour_config(path, 4321, "secret")
    assert json.loads(path.read_text()) == {"own_config": True, "draw_world": False,
                                            "control": {"port": 4321, "token": "secret"}}
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_the_tour_presses_the_e2es_buttons_too():
    """The e2e's HUD buttons (ScfgShowcase.h's names) are among the tour's, which checks the shop
    has exactly these."""
    assert e2e.UNLOCK == "Unlock.chapter2" and e2e.TIP == "Tip.1.00"
    assert {e2e.UNLOCK, e2e.TIP, "Restore"} <= set(e2e.TOUR_BUTTONS)
    assert len(set(e2e.TOUR_BUTTONS)) == len(e2e.TOUR_BUTTONS)


def test_the_tour_runs_the_editor_build_only(capsys):
    with pytest.raises(SystemExit) as raised:
        e2e.main(["--showcase", "--packaged"])
    assert raised.value.code == 2 and "--packaged" in capsys.readouterr().err


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


def test_problems_are_every_error_and_warning_but_the_measured_engine_ones():
    allowed = [
        "[1][0]LogScfg: scfg: the payment state file is /tmp/x/state.json",
        "[1][0]LogEditorDataStorageUI: Warning: Unable to register widget factory "
        "'AlertWidgetConstructor' as purpose '0' isn't registered.",
        '[1][0]LogCsvProfiler: Warning: Metadata value sanitized due to invalid characters: '
        'gpu="llvmpipe (LLVM 20.1.2, 256 bits)"',
        "[1][0]LogClass: Error: StructProperty FDataflowToolNodeSnapshot::Date is not initialized "
        "properly even though its struct probably has a custom default constructor.",
        "[1][0]LogAutomationTest: Error: LogClass: StructProperty FDataflowToolNodeSnapshot::Date "
        "is not initialized properly even though its struct probably has a custom default constructor.",
        "[1][0]LogScfg: Warning: scfg: cannot open the http: link: LaunchURL cancelled by delegate",
        "[1][0]LogScfg: Warning: scfg: cannot open the ethereum: link: LaunchURL cancelled by delegate",
    ]
    bad = [
        "[1][0]LogScfg: Error: scfg: worker error: boom",
        "[1][0]LogClass: Error: StructProperty FSomeOtherStruct::Date is not initialized properly",
        "[1][0]LogScfg: Warning: scfg: cannot open the http: link: no xdg-open",
        "[1][0]LogScfgDev: Warning: showcase: no game viewport, no HUD",
        "[1][0]LogSlate: Error: something the plugin made Slate say",
        "[1][0]LogHttp: Warning: a request of ours",
        "[1][0]LogEditorDataStorageUI: Warning: something else of that category",
        "[1][0]LogOutputDevice: Error: Ensure condition failed: x",
        "Segmentation fault (core dumped)",
    ]
    assert e2e.problems("\n".join(allowed + bad)) == bad


def test_the_packaged_build_command_stages_a_development_game(tmp_path):
    cmd = e2e.buildcookrun_command(Path("/ue"), tmp_path / "archive")
    assert cmd[0] == "/ue/Engine/Build/BatchFiles/RunUAT.sh" and cmd[1] == "BuildCookRun"
    for flag in ("-platform=Linux", "-clientconfig=Development", "-build", "-cook", "-stage",
                 "-pak", "-archive", f"-archivedirectory={tmp_path / 'archive'}",
                 "-ubtargs=-UBAHost=127.0.0.1", "-NoZenAutoLaunch"):
        assert flag in cmd, flag
    cooker = [a for a in cmd if a.startswith("-AdditionalCookerOptions=")]
    assert cooker == ["-AdditionalCookerOptions=-ddc=NoZenLocalFallback -notraceserver"]


def test_uat_writes_its_logs_into_the_run(tmp_path):
    env = e2e.uat_env({"PATH": "/bin"}, tmp_path)
    assert env == {"PATH": "/bin", "uebp_LogFolder": str(tmp_path / "uat-logs"),
                   "uebp_EngineSavedFolder": str(tmp_path / "uat-saved")}


def test_changed_since_names_what_changed_after_the_mark(tmp_path):
    import os as _os

    old = tmp_path / "logs" / "old.txt"
    new = tmp_path / "logs" / "sub" / "new.txt"
    new.parent.mkdir(parents=True)
    old.write_text("x")
    new.write_text("y")
    for p in (old, old.parent, new.parent):
        _os.utime(p, (100.0, 100.0))
    _os.utime(new, (300.0, 300.0))
    assert e2e.changed_since([tmp_path / "logs", tmp_path / "missing"], 200.0) == [new]
    assert e2e.newest_mtime([tmp_path / "logs"]) == 300.0
    assert e2e.changed_since([tmp_path / "logs"], 50.0, limit=2) == sorted(
        [old.parent, old, new.parent, new])[:2]


def _png(path, width, height, pixel):
    """An RGBA PNG whose rows use every filter type in turn (the decoder must undo each)."""
    import struct
    import zlib

    raw = b""
    for y in range(height):
        kind = y % 5
        line = bytes(c for x in range(width) for c in pixel(x, y))
        prev = bytes(c for x in range(width) for c in pixel(x, y - 1)) if y else bytes(len(line))
        out = bytearray()
        for i, v in enumerate(line):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            pred = [0, a, b, (a + b) // 2, a if pa <= pb and pa <= pc else b if pb <= pc else c][kind]
            out.append((v - pred) & 255)
        raw += bytes([kind]) + bytes(out)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(
            ">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                     8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def test_read_png_undoes_every_filter(tmp_path):
    def pixel(x, y):
        return ((x * 7 + y) & 255, (x * y) & 255, (255 - x) & 255, 255)

    _png(tmp_path / "p.png", 9, 10, pixel)
    width, height, ch, rows = e2e.read_png(tmp_path / "p.png")
    assert (width, height, ch) == (9, 10, 4)
    assert all(rows[y][x * 4:x * 4 + 4] == bytes(pixel(x, y)) for y in range(10) for x in range(9))


def test_sheet_drawn_wants_the_panel_with_text_beside_a_dimmed_game(tmp_path, monkeypatch):
    monkeypatch.setattr(e2e, "WINDOW", (64, 40))
    view = {"buttons": [{"rect": [36, 4, 20, 6]}, {"rect": [36, 30, 20, 6]}]}

    def drawn(x, y):
        if x < 32:
            return (8, 8, 12, 255)  # the dimmed game
        return (20, 30, 40, 255) if (x + y) % 9 == 0 else (*e2e.SHEET_RGB, 255)

    _png(tmp_path / "ok.png", 64, 40, drawn)
    assert e2e.sheet_drawn(tmp_path / "ok.png", view) is None
    _png(tmp_path / "black.png", 64, 40, lambda x, y: (0, 0, 0, 255))
    assert "panel" in e2e.sheet_drawn(tmp_path / "black.png", view)
    _png(tmp_path / "blank.png", 64, 40, lambda x, y: (8, 8, 8, 255) if x < 32 else
         (*e2e.SHEET_RGB, 255))
    assert "text" in e2e.sheet_drawn(tmp_path / "blank.png", view)
    _png(tmp_path / "small.png", 32, 20, drawn)
    assert "32x20" in e2e.sheet_drawn(tmp_path / "small.png", view)


def test_remove_work_leaves_a_directory_with_a_mount_under_it(tmp_path, monkeypatch, capsys):
    work = tmp_path / "scfg-unreal-e2e-x"
    (work / "home" / ".gvfs").mkdir(parents=True)
    real_read = Path.read_text

    def read_text(self, *a, **kw):
        if str(self) == "/proc/mounts":
            return f"gvfsd-fuse {work / 'home' / '.gvfs'} fuse.gvfsd-fuse rw 0 0\n"
        return real_read(self, *a, **kw)

    monkeypatch.setattr(Path, "read_text", read_text)
    e2e.remove_work(work)
    assert work.exists() and "mounted" in capsys.readouterr().out
    monkeypatch.setattr(Path, "read_text", real_read)
    e2e.remove_work(work)
    assert not work.exists()


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
    deadline = time.monotonic() + 5  # the killed child is a zombie until its new parent reaps it
    while e2e.group_alive(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not e2e.group_alive(proc.pid)


def test_kill_session_kills_the_runs_session_and_what_it_started_elsewhere(tmp_path):
    """The outer run's last step: whatever is left in the session (a chain node, a game in its own
    process group) and what those started in sessions of their own (a browser) is ended."""
    pids = tmp_path / "pids"
    proc = subprocess.Popen(
        ["sh", "-c", f"sleep 60 & echo $! >> {pids}; (sleep 62 & echo $! >> {pids}; wait) & "
         f"setsid sh -c 'sleep 63 & echo $! >> {pids}; wait' & wait"],
        start_new_session=True)
    deadline = time.monotonic() + 5
    while not pids.exists() or len(pids.read_text().split()) < 3:
        assert time.monotonic() < deadline
        time.sleep(0.05)
    started = [int(p) for p in pids.read_text().split()]
    tree = e2e.run_tree(proc.pid, e2e.read_processes())
    assert proc.pid in tree and set(started) <= set(tree)
    assert os.getsid(started[2]) != proc.pid  # the third is in a session of its own
    assert e2e.kill_session(proc.pid) >= 5
    proc.wait(5)
    assert _wait_gone(started) == []
    assert e2e.run_tree(proc.pid, e2e.read_processes()) == []


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
    assert e2e.main(["--showcase"]) == 2  # the tour too
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
