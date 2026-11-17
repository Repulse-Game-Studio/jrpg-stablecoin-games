#!/usr/bin/env python3
"""Run the Unreal desktop end-to-end test: the dev project's showcase (unreal/Project/ScfgDev,
Source/ScfgDev/ScfgShowcase.h) with the plugin's real Slate sheet, worker and localhost checkout
page, in a real rendered game window on a private X server, paying on a local chain.

The showcase is played the way a player plays it, through its e2e driver (ScfgE2EDriver.h, built
outside Shipping, made only with -scfg-e2e=<config>): the runner sends commands over a loopback
control socket and the driver clicks the sheet's and the HUD's buttons with real mouse input and
presses keys through FSlateApplication's platform message handler, and reports what the screen
shows. Per run (godot/demo's scenarios, tools/run_godot_e2e.py):

  0. Broken e2e configs (not a JSON object, a config Configure() refuses): the game quits with
     exit code 1 by itself, without showing its HUD.
  1. Unlock Chapter 2 by manual transfer: copy the amount and address from the sheet, pay them
     from the chain side, wait for "Payment confirmed", check Owns("chapter2") and the receipt in
     Saved/Scfg/state.json.
  2. Tip with the browser wallet: open the localhost page the game opens, in Playwright Chromium
     with a mock wallet (EIP-6963 for evm/arc, Wallet Standard for solana), and pay there. The tip
     is opened by a real click on the HUD right after the unlock sheet closed.
  3. Real-window input (task 7's kept cases): the sheet's mouse wish names the game window's real
     SViewport; in a game that hides the cursor, the game window is deactivated and activated
     again with a sheet open (a real second native window) and the sheet keeps the mouse and the
     focus, takes a real click, and gives the capture back when it closes; a close under
     LockAlways gives the cursor lock back without forcing a capture; in the cursor-showing
     configuration a HUD click after a close reaches the HUD, not the world.
  4. Restart the game: Chapter 2 is still owned.

--family evm (default) pays USDC on Anvil (the arbitrum entry pointed at a mock token); arc pays
native USDC on Anvil dressed as Arc; solana pays USDC on solana-test-validator. They need `anvil`
or `solana-test-validator` on PATH, Playwright's Chromium and `solders` (run it with `uv run`).

The game runs only on an X server of its own: the runner re-runs itself under `xvfb-run -a`
(DISPLAY and WAYLAND_DISPLAY dropped first) and refuses to start a game unless DISPLAY names the
Xvfb that xvfb-run started for this run (own_xvfb). Rendering: Vulkan on Mesa's lavapipe
(VK_ICD_FILENAMES / VK_DRIVER_FILES), which the engine accepts only with -AllowCPUDevices
(VulkanRHI.cpp's device choice skips CPU devices otherwise), at the Linux default feature level
SM5. The game's user data (HOME, XDG_*, -UserDir) lives in a temporary directory.

Two modes:
  default     the editor build (tools/run_unreal_build.py, ScfgDevEditor Development) run as a
              game, uncooked: `UnrealEditor <uproject> -game`. Fast.
  --packaged  `RunUAT BuildCookRun -platform=Linux -clientconfig=Development -build -cook -stage
              -pak -archive` into a temporary directory, then the staged game: this checks that
              the plugin's Resources/Web is staged where the page server finds it and that the
              default sheet host works in a cooked game.

Every process (the build, the game, the chain, the browser) runs under a timeout and is killed by
process group; the whole run is one session under xvfb-run, killed as a whole at the end. Ports
are ephemeral on 127.0.0.1. Screenshots of the sheet (one per run) go to --screenshots.

    uv run python tools/run_unreal_e2e.py --family solana [--packaged] [--screenshots DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
DEFAULT_ENGINE = Path(os.environ.get("UE_ROOT", "/mnt/files/UnrealEngine"))
PROJECT = ROOT / "unreal" / "Project" / "ScfgDev" / "ScfgDev.uproject"
# The engine's software Vulkan driver (Mesa lavapipe).
LAVAPIPE_ICD = "/usr/share/vulkan/icd.d/lvp_icd.json"
# The X server's screen: the 1280x720 game window and room around it for the second window.
XVFB_SCREEN = "1600x900x24"
WINDOW = (1280, 720)
# Set by the outer run for the inner one: the token proves the inner run was started by it.
INNER_ENV = "SCFG_UNREAL_E2E_INNER"
OUTER_PID_ENV = "SCFG_UNREAL_E2E_OUTER_PID"
# Variables that would reach the developer's desktop session (or a library path of the caller's).
DESKTOP_VARS = ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR",
                "XAUTHORITY", "LD_LIBRARY_PATH", "SESSION_MANAGER", "DESKTOP_SESSION")
ITEM = "chapter2"
ITEMS = {ITEM: {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"}}
CONFIRMED = "Payment confirmed. Thank you!"
UNLOCKED = "Chapter 2: unlocked. Thank you for supporting the game!"
READY_LINE = "showcase: ready"
# Seconds: a first start compiles the shaders on the CPU (minutes); a command may wait on a frame.
START_TIMEOUT = 900.0
REPLY_TIMEOUT = 120.0
GAME_TIMEOUT = 1800.0     # a game process lives at most this long
RUN_TIMEOUT = 7200.0      # the whole inner run (the packaged build included)
BUILD_TIMEOUT = 3600.0
# Lines of a game log that fail a run: our categories' errors and warnings, and any crash.
OUR_PROBLEM = re.compile(r"\bLogScfg(Dev)?: (Error|Warning): ")
FATAL = re.compile(r"Ensure condition failed|Assertion failed|Fatal error|=== Critical error ===|"
                   r"Unhandled Exception|Segmentation fault")


class E2EError(Exception):
    pass


# --- the X server guard ----------------------------------------------------------------------


@dataclass(frozen=True)
class Proc:
    pid: int
    ppid: int
    argv: tuple


def read_processes(proc_root: Path = Path("/proc")) -> list:
    """Every process this user can see, as Proc(pid, ppid, argv)."""
    out = []
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            argv = (entry / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        # "pid (comm) state ppid ...": comm may hold spaces and parentheses
        ppid = int(stat[stat.rindex(")") + 2:].split()[1])
        out.append(Proc(int(entry.name), ppid,
                        tuple(a.decode(errors="replace") for a in argv if a)))
    return out


def own_xvfb(display: str | None, my_pid: int, outer_pid: int, processes: list) -> str | None:
    """None when `display` is the X server xvfb-run started for this run, else why not.

    Ours means: DISPLAY is ":<n>"; this process's parent (xvfb-run) was started by the outer run
    (outer_pid); and an Xvfb process serving ":<n>" is a child of that same xvfb-run. So a
    developer's DISPLAY, or an Xvfb someone else started, is never used."""
    if not display or not re.fullmatch(r":\d+", display):
        return f"DISPLAY is {display!r}, not an X display of xvfb-run's"
    by_pid = {p.pid: p for p in processes}
    me = by_pid.get(my_pid)
    if me is None:
        return "this process is not in the process table"
    xvfb_run = by_pid.get(me.ppid)
    if xvfb_run is None or xvfb_run.ppid != outer_pid:
        return "this run was not started by xvfb-run under the outer run"
    for p in processes:
        if p.ppid == xvfb_run.pid and p.argv and Path(p.argv[0]).name == "Xvfb" \
                and display in p.argv[1:]:
            return None
    return f"no Xvfb serving {display} was started by this run's xvfb-run"


def outer_command(argv: list) -> list:
    """The command the outer run starts: this script again, under `xvfb-run -a`."""
    return ["xvfb-run", "-a", "-s", f"-screen 0 {XVFB_SCREEN} -nolisten tcp",
            sys.executable, str(Path(__file__).resolve()), *argv]


def outer_env(environ: dict, token: str, outer_pid: int) -> dict:
    """The environment of the inner run: no desktop session variables (xvfb-run sets DISPLAY and
    XAUTHORITY itself), plus the token and the outer pid that own_xvfb checks."""
    env = {k: v for k, v in environ.items() if k not in DESKTOP_VARS}
    env[INNER_ENV] = token
    env[OUTER_PID_ENV] = str(outer_pid)
    return env


# --- the X server: input focus ----------------------------------------------------------------


def focus_window_of(pid: int, display: str) -> int | None:
    """Gives the X input focus to the largest top-level window of process `pid` (SDL sets
    _NET_WM_PID) on `display`, as a window manager focuses the window a player clicks; returns
    the window id, or None when it has none. Xvfb runs no window manager, so nothing else does:
    without the X focus SDL reports no focus gain, the engine has no focus window, and its cursor
    lock (FLinuxCursor::Lock, SDL_SetWindowMouseRect) does nothing. libX11 through ctypes."""
    import ctypes
    import ctypes.util

    lib = ctypes.util.find_library("X11")
    if not lib:
        raise E2EError("libX11 not found (needed to focus the game window on Xvfb)")
    x = ctypes.CDLL(lib)
    ulong, c_int, c_uint = ctypes.c_ulong, ctypes.c_int, ctypes.c_uint
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XInternAtom.restype = ulong
    x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, c_int]
    x.XQueryTree.argtypes = [ctypes.c_void_p, ulong, ctypes.POINTER(ulong), ctypes.POINTER(ulong),
                             ctypes.POINTER(ctypes.POINTER(ulong)), ctypes.POINTER(c_uint)]
    x.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ulong, ulong, ctypes.c_long, ctypes.c_long, c_int, ulong,
        ctypes.POINTER(ulong), ctypes.POINTER(c_int), ctypes.POINTER(ulong), ctypes.POINTER(ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    x.XGetGeometry.argtypes = [ctypes.c_void_p, ulong, ctypes.POINTER(ulong),
                               ctypes.POINTER(c_int), ctypes.POINTER(c_int),
                               *[ctypes.POINTER(c_uint)] * 4]
    x.XSetInputFocus.argtypes = [ctypes.c_void_p, ulong, c_int, ulong]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XSync.argtypes = [ctypes.c_void_p, c_int]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    dpy = x.XOpenDisplay(display.encode())
    if not dpy:
        raise E2EError(f"cannot open the X display {display}")
    try:
        root = x.XDefaultRootWindow(dpy)
        wm_pid = x.XInternAtom(dpy, b"_NET_WM_PID", 1)
        best, best_area = None, -1
        r, parent = ulong(), ulong()
        children, n = ctypes.POINTER(ulong)(), c_uint()
        if not wm_pid or not x.XQueryTree(dpy, root, ctypes.byref(r), ctypes.byref(parent),
                                          ctypes.byref(children), ctypes.byref(n)):
            return None
        windows = [children[i] for i in range(n.value)]
        if children:
            x.XFree(children)
        for w in windows:
            kind, fmt, count, after = ulong(), c_int(), ulong(), ulong()
            data = ctypes.POINTER(ctypes.c_ubyte)()
            if x.XGetWindowProperty(dpy, w, wm_pid, 0, 1, 0, 6, ctypes.byref(kind),
                                    ctypes.byref(fmt), ctypes.byref(count), ctypes.byref(after),
                                    ctypes.byref(data)) != 0 or not data:
                continue
            owner = ctypes.cast(data, ctypes.POINTER(ulong))[0] if count.value == 1 else None
            x.XFree(data)
            if owner != pid:
                continue
            gx, gy = c_int(), c_int()
            width, height, border, depth = c_uint(), c_uint(), c_uint(), c_uint()
            x.XGetGeometry(dpy, w, ctypes.byref(r), ctypes.byref(gx), ctypes.byref(gy),
                           ctypes.byref(width), ctypes.byref(height), ctypes.byref(border),
                           ctypes.byref(depth))
            if width.value * height.value > best_area:
                best, best_area = w, width.value * height.value
        if best is not None:
            x.XSetInputFocus(dpy, best, 2, 0)  # RevertToParent, CurrentTime
            x.XSync(dpy, 0)
        return best
    finally:
        x.XCloseDisplay(dpy)


# --- processes -------------------------------------------------------------------------------


def new_group_kwargs() -> dict:
    """Popen arguments that put the child in a process group of its own (same session)."""
    if sys.version_info >= (3, 11):
        return {"process_group": 0}
    return {"preexec_fn": os.setpgrp}  # pragma: no cover (Python < 3.11)


def kill_group(pgid: int, grace: float = 5.0) -> None:
    """SIGTERM the process group, then SIGKILL what is left after `grace` seconds."""
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if not group_alive(pgid):
            return
        time.sleep(0.1)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def session_members(sid: int, proc_root: Path = Path("/proc")) -> list:
    """The pids of the processes in session `sid` (this user's)."""
    out = []
    for entry in proc_root.iterdir():
        if entry.name.isdigit():
            try:
                if os.getsid(int(entry.name)) == sid:
                    out.append(int(entry.name))
            except (ProcessLookupError, PermissionError):
                pass
    return out


def kill_session(sid: int) -> int:
    """SIGKILL every process of session `sid` except this one; returns how many were left."""
    left = [pid for pid in session_members(sid) if pid != os.getpid()]
    for pid in left:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return len(left)


# --- the game --------------------------------------------------------------------------------


def write_config(path: Path, family_config: dict, chain: dict | None, port: int,
                 token: str) -> dict:
    """The driver's e2e config (ScfgE2EDriver.h): the family's configure and items, the chain
    override and the control socket. Written readable by this user only."""
    config = {**family_config, "control": {"port": port, "token": token}}
    if chain is not None:
        config["chain"] = chain
    path.write_text(json.dumps(config, indent=1), encoding="utf-8")
    path.chmod(0o600)
    return config


def game_env(environ: dict, home: Path) -> dict:
    """The game's environment: the inner run's (DISPLAY is the Xvfb's) with lavapipe forced, SDL
    on X11, no desktop session bus, no caller library path, and HOME/XDG in `home`."""
    env = {k: v for k, v in environ.items()
           if k not in ("WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH",
                        "XDG_RUNTIME_DIR", "SESSION_MANAGER")}
    env.update({
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local" / "share"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "VK_ICD_FILENAMES": LAVAPIPE_ICD,
        "VK_DRIVER_FILES": LAVAPIPE_ICD,
        "SDL_VIDEODRIVER": "x11",
    })
    return env


def ddc_graph(engine: Path) -> str:
    installed = (engine / "Engine" / "Build" / "InstalledBuild.txt").is_file()
    return "InstalledNoZenLocalFallback" if installed else "NoZenLocalFallback"


def game_command(engine: Path, config: Path, user_dir: Path, staged: Path | None = None) -> list:
    """The game's command line: the editor binary as an uncooked game, or the staged game."""
    common = [
        f"-scfg-e2e={config}", f"-UserDir={user_dir}",
        "-windowed", f"-ResX={WINDOW[0]}", f"-ResY={WINDOW[1]}", "-WinX=0", "-WinY=0",
        "-vulkan", "-AllowCPUDevices", "-nosplash", "-nosound", "-unattended", "-stdout",
        "-FullStdOutLogOutput", "-notraceserver",
    ]
    if staged is not None:
        return [str(staged), "ScfgDev", *common]
    return [str(engine / "Engine" / "Binaries" / "Linux" / "UnrealEditor"), str(PROJECT), "-game",
            *common, "-NoZenAutoLaunch", f"-ddc={ddc_graph(engine)}",
            # the shaders compiled for this project stay between runs (the HOME is new each run)
            f"-LocalDataCachePath={PROJECT.parent / 'DerivedDataCache'}"]


def problems(log: str) -> list:
    """The lines of a game log that fail a run."""
    return [line for line in log.splitlines() if OUR_PROBLEM.search(line) or FATAL.search(line)]


class Control:
    """The runner's end of the control socket: listens on 127.0.0.1, port chosen by the kernel."""

    def __init__(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.token = secrets.token_hex(16)

    def close(self):
        self.server.close()


class Game:
    """One game process and its control connection; `call(op, **args)` sends one command."""

    def __init__(self, cmd: list, env: dict, log: Path, control: Control, label: str):
        self.label = label
        self.log_path = log
        self._log = log.open("wb")
        self.started = time.monotonic()
        self.proc = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=self._log,
                                     stderr=subprocess.STDOUT, **new_group_kwargs())
        self.control = control
        self.conn = None
        self._buf = b""
        self._next = 0
        self.last_click = None

    def connect(self, timeout: float = START_TIMEOUT) -> float:
        """Waits for the driver's hello; returns the seconds the start took."""
        deadline = time.monotonic() + timeout
        self.control.server.settimeout(1.0)
        while self.conn is None:
            if self.proc.poll() is not None:
                raise E2EError(f"{self.label}: the game exited ({self.proc.returncode}) before "
                               f"it connected:\n{self.tail()}")
            if time.monotonic() > deadline:
                raise E2EError(f"{self.label}: the game did not connect within {timeout:g} s:\n"
                               f"{self.tail()}")
            try:
                conn, peer = self.control.server.accept()
            except socket.timeout:
                continue
            if peer[0] != "127.0.0.1":
                conn.close()
                continue
            self.conn = conn
        hello = self._read(deadline)
        if hello.get("hello") != self.control.token:
            raise E2EError(f"{self.label}: a stranger connected: {hello}")
        if hello.get("pid") != self.proc.pid:
            raise E2EError(f"{self.label}: the driver is pid {hello.get('pid')}, not the game's "
                           f"{self.proc.pid}")
        return time.monotonic() - self.started

    def focus(self) -> None:
        """The X input focus to the game window (focus_window_of), then a frame for SDL."""
        if focus_window_of(self.proc.pid, os.environ["DISPLAY"]) is None:
            raise E2EError(f"{self.label}: no X window of the game to focus")
        self.snapshot()

    def _read(self, deadline: float) -> dict:
        while b"\n" not in self._buf:
            left = deadline - time.monotonic()
            if left <= 0:
                raise E2EError(f"{self.label}: no reply in time:\n{self.tail()}")
            self.conn.settimeout(min(left, 1.0))
            try:
                chunk = self.conn.recv(1 << 16)
            except socket.timeout:
                if self.proc.poll() is not None:
                    raise E2EError(f"{self.label}: the game exited ({self.proc.returncode}):\n"
                                   f"{self.tail()}") from None
                continue
            if not chunk:
                raise E2EError(f"{self.label}: the game closed the control connection:\n"
                               f"{self.tail()}")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return json.loads(line)

    def call(self, op: str, timeout: float = REPLY_TIMEOUT, **args):
        self._next += 1
        msg = {"id": self._next, "op": op, **args}
        try:
            self.conn.sendall(json.dumps(msg).encode() + b"\n")
        except OSError as e:
            raise E2EError(f"{self.label}: cannot send {op}: {e}:\n{self.tail()}") from None
        if time.monotonic() - self.started > GAME_TIMEOUT:
            raise E2EError(f"{self.label}: the game ran longer than {GAME_TIMEOUT:g} s")
        reply = self._read(time.monotonic() + timeout)
        if reply.get("id") != self._next:
            raise E2EError(f"{self.label}: reply {reply} to command {self._next} ({op})")
        if "error" in reply:
            raise E2EError(f"{self.label}: {op} {args}: {reply['error']}")
        return reply.get("ok")

    def snapshot(self) -> dict:
        return self.call("snapshot")

    def click(self, name: str, where: str = "screen"):
        reply = self.call("click", name=name, **{"in": where})
        self.last_click = {"name": name, "in": where, **reply}
        return reply

    def key(self, key: str, repeat: bool = False):
        return self.call("key", key=key, repeat=repeat)

    def quit(self) -> int:
        """Quits the normal way; returns the exit code."""
        self.call("quit")
        try:
            return self.proc.wait(120)
        except subprocess.TimeoutExpired:
            raise E2EError(f"{self.label}: the game did not quit:\n{self.tail()}") from None

    def wait_exit(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            raise E2EError(f"{self.label}: the game did not quit within {timeout:g} s:\n"
                           f"{self.tail()}") from None

    def output(self) -> str:
        self._log.flush()
        return self.log_path.read_text(errors="replace")

    def tail(self, lines: int = 60) -> str:
        return "\n".join(self.output().splitlines()[-lines:])

    def close(self) -> None:
        if self.conn is not None:
            self.conn.close()
            self.conn = None
        if self.proc.poll() is None:
            kill_group(self.proc.pid)
            self.proc.wait()
        elif group_alive(self.proc.pid):
            kill_group(self.proc.pid)  # a helper the game started (a shader compiler)
        self._log.close()


def wait_for(game: Game, what: str, pred, timeout: float = 90, tick=None) -> dict:
    """Snapshots until pred(snapshot) holds; `tick()` runs between them (a block, a page wait)."""
    deadline = time.monotonic() + timeout
    snap = None
    while time.monotonic() < deadline:
        snap = game.snapshot()
        if pred(snap):
            return snap
        if tick is not None:
            tick()
        else:
            time.sleep(0.1)
    raise E2EError(f"timed out waiting for {what} (the last click: {game.last_click}); the "
                   f"screen shows:\n{json.dumps(snap, indent=1)}")


def sheet(snap: dict) -> dict | None:
    """The newest open payment sheet's view."""
    for view in reversed(snap["screens"]):
        if view["kind"] == "sheet":
            return view
    return None


def button(view: dict | None, name: str) -> dict | None:
    for b in (view or {}).get("buttons", []):
        if b["name"] == name:
            return b
    return None


def has_button(name: str, text: str | None = None):
    def pred(snap):
        b = button(sheet(snap), name)
        return b is not None and not b["disabled"] and (text is None or b["text"] == text)

    return pred


def shows(text: str):
    return lambda snap: text in (sheet(snap) or {}).get("texts", [])


def no_screen(snap: dict) -> bool:
    return not snap["screens"]


def copy_value(game: Game, name: str) -> str:
    """Clicks a Copy button on the sheet and reads what it put on the (Xvfb's) clipboard."""
    before = game.snapshot()["clipboard"]
    game.click(name)
    return wait_for(game, f"{name} to copy", lambda s: s["clipboard"] != before, 20)["clipboard"]


def expect(cond: bool, why: str, data=None) -> None:
    if not cond:
        raise E2EError(why + ("" if data is None else ":\n" + json.dumps(data, indent=1)))


# --- the scenarios ---------------------------------------------------------------------------


def unlock_by_manual_transfer(game: Game, fam, state_file: Path, shots: Path, family: str) -> None:
    wait_for(game, "the Unlock button", lambda s: button(s["game"], "Unlock") is not None, 120)
    game.click("Unlock", "game")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    snap = wait_for(game, "the method step", has_button("method_manual"))
    expect(snap["paused"], "the showcase should pause the game while the sheet is open")
    expect(snap["input"]["focus"] == "sheet", "the sheet should have the keyboard focus",
           snap["input"])
    game.click("method_manual")
    wait_for(game, "the manual step", has_button("copy_amount"))
    amount = copy_value(game, "copy_amount")
    recipient = copy_value(game, "copy_recipient")
    token = copy_value(game, "copy_token")
    texts = sheet(game.snapshot())["texts"]
    expect(any(t.startswith(amount + " ") for t in texts) and recipient in texts,
           f"copied {amount!r} / {recipient!r}, which the sheet does not show", texts)
    fam.check_manual(recipient, token)
    shot = shots / f"unreal-e2e-{family}-manual.png"
    game.call("screenshot", path=str(shot), timeout=180)
    print(f"  manual step: send {amount} USDC to {recipient} (screenshot {shot})")
    game.click("forward")  # I've sent it
    wait_for(game, "the waiting step",
             lambda s: any(t.startswith("Waiting for your payment")
                           for t in (sheet(s) or {}).get("texts", [])))
    tx = fam.pay_manual(amount, recipient)
    print(f"  paid from the chain side: {tx}")
    snap = wait_for(game, "the payment to be confirmed", shows(CONFIRMED), 180, fam.tick)
    expect(game.call("owns", item=ITEM), "the sheet says paid but Owns('chapter2') is false")
    # Continue with the keyboard: the done step focuses it.
    expect(sheet(snap)["focus"] == "continue",
           f"the done step focuses {sheet(snap)['focus']!r}, not Continue")
    game.key("Enter")
    wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"])
    expect(button(snap["game"], "Unlock") is None, "the Unlock button is still shown")
    expect(not snap["paused"], "the game is still paused after the sheet closed")
    receipts = json.loads(state_file.read_text())["receipts"]
    found = [r for r in receipts.values() if r["item"] == ITEM and r["kind"] == "unlock"]
    expect(len(found) == 1 and found[0]["tx_hash"] == tx, f"state.json has no receipt for {tx}",
           receipts)
    print(f"  unlocked; state.json holds the receipt ({state_file})")


def tip_by_browser_wallet(game: Game, fam, browser, state_file: Path) -> None:
    before = game.snapshot()
    expect(before["input"]["captor"] == "none",
           "after the close nothing should hold the mouse in the cursor-showing game",
           before["input"])
    game.click("Tip", "game")  # right after a close: the click must reach the HUD, not the world
    snap = wait_for(game, "the amount step", has_button("forward", "Next"))
    expect(snap["world_clicks"] == before["world_clicks"],
           "the HUD click after the close reached the world", snap)
    game.click("forward")  # Next: the 1 USDC the showcase asks for is already chosen
    wait_for(game, "the method step", has_button("method_browser"))
    game.click("method_browser")
    wait_for(game, "the browser step", has_button("forward", "Open checkout in browser"))
    opened = len(game.snapshot()["opened"])
    game.click("forward")
    url = wait_for(game, "the game to open the browser", lambda s: len(s["opened"]) > opened,
                   30)["opened"][-1]
    expect(url.startswith("http://127.0.0.1:"), f"the game opened {url!r}, not its localhost page")
    print(f"  the game opened {url.split('?')[0]}...")
    page = browser.new_page()
    try:
        fam.install_wallet(page)
        page.goto(url)
        fam.connect(page).click()

        # The mock wallet's Python callbacks only run inside Playwright calls: wait in the page.
        def tick():
            page.wait_for_timeout(250)
            fam.tick()

        wait_for(game, "the tip to be confirmed", shows(CONFIRMED), 180, tick)
        page.wait_for_function(
            "document.getElementById('status').textContent.includes('Payment confirmed')",
            timeout=30_000)
    finally:
        page.close()
    game.click("continue")
    wait_for(game, "the sheet to close", no_screen)
    thanks = re.compile(r"^Thank you for the (1\.\d+) USDC tip!$")
    snap = wait_for(game, "the thank-you",
                    lambda s: any(thanks.match(t) for t in s["game"]["texts"]))
    shown = next(thanks.match(t)[1] for t in snap["game"]["texts"] if thanks.match(t))
    receipts = json.loads(state_file.read_text())["receipts"]
    tips = [r for r in receipts.values() if r["kind"] == "tip"]
    expect(len(receipts) == 2 and len(tips) == 1 and tips[0]["amount"] == shown,
           f"state.json should hold the unlock and the {shown} USDC tip", receipts)
    expect(snap["world_clicks"] == before["world_clicks"], "a click reached the world", snap)
    print("  tipped through the browser page; state.json holds both receipts")


def real_window_input(game: Game) -> dict:
    """Task 7's kept cases, in the real window. Returns what was observed (for the report)."""
    seen = {}
    wish = game.call("wish")
    seen["wish"] = wish
    expect(wish.get("viewport_type") == "SViewport" and wish.get("is_game_viewport_widget")
           and wish.get("in_game_window") and wish.get("window_viewport_is_its_viewport"),
           "the sheet's mouse wish should name the game window's own SViewport", wish)

    # a game that hides the cursor: the viewport holds the mouse
    state = game.call("input_mode", mode="game")
    state = wait_for(game, "the game-only input mode",
                     lambda s: s["input"]["captor"] == "viewport", 30)["input"]
    seen["game_mode"] = state
    expect(state["focus"] == "viewport" and state["high_precision"] and state["cursor_locked"],
           "game-only: the viewport should have the focus, the mouse in high precision and the "
           "cursor locked", state)
    clicks = game.snapshot()["world_clicks"]
    game.call("open", what="tip")
    snap = wait_for(game, "the tip sheet over the captured viewport", has_button("forward", "Next"))
    expect(snap["input"]["focus"] == "sheet" and snap["input"]["captor"] == "none"
           and not snap["input"]["high_precision"] and snap["input"]["cursor_locked"] is False,
           "open: the sheet should hold the focus and have taken the viewport's mouse (capture, "
           "high precision, lock)", snap["input"])
    holds = game.call("wish")["screen_holds"]
    expect(holds == "viewport", f"the sheet should hold the real viewport's capture, not {holds}")
    cycle = game.call("activation_cycle", timeout=240)
    seen["activation_cycle"] = cycle
    # The other window is gone: a window manager would give the X focus back to the game window.
    game.focus()
    expect(cycle["deactivated"] and cycle["reactivated"],
           "a second native window should deactivate the game window, and closing it activate "
           "it again", cycle)
    expect(any(e.endswith("->viewport (window_activate)") for e in cycle["focus_events"]),
           "the engine should give the viewport the focus when the window is activated again",
           cycle)
    after = cycle["after"]
    expect(after["focus"] == "sheet" and after["captor"] != "viewport"
           and not after["high_precision"],
           "after the activation the sheet should have the focus and the mouse again", cycle)
    game.click("forward")  # a real click on the sheet after the activation
    wait_for(game, "the method step after the activation", has_button("method_browser"))
    game.key("Escape")
    wait_for(game, "back on the amount step", has_button("forward", "Next"))
    game.key("Escape")
    snap = wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the capture given back",
                    lambda s: s["input"]["captor"] == "viewport", 30)
    seen["game_mode_after_close"] = snap["input"]
    expect(snap["input"]["focus"] == "viewport" and snap["input"]["high_precision"]
           and snap["input"]["cursor_locked"] is True,
           "closed: the game's focus, capture, high precision and lock should be back",
           snap["input"])
    expect(snap["world_clicks"] == clicks, "a click on the sheet reached the world", snap)
    print("  game-only input: window deactivated and activated again with the sheet open; the "
          "sheet kept the focus and the mouse, took a real click, and gave the capture back")

    # a cursor shown and locked to the window (LockAlways)
    state = game.call("input_mode", mode="lock_always")
    state = wait_for(game, "the lock-always input mode",
                     lambda s: s["input"]["captor"] == "none", 30)["input"]
    seen["lock_always"] = state
    expect(state["cursor_locked"] is True and game.call("wish")["always_lock"],
           "LockAlways: the cursor should be locked to the window", state)
    game.call("open", what="tip")
    snap = wait_for(game, "the tip sheet under LockAlways", has_button("forward", "Next"))
    holds = game.call("wish")["screen_holds"]
    seen["lock_always_open"] = {**snap["input"], "screen_holds": holds}
    expect(snap["input"]["cursor_locked"] is False and snap["input"]["captor"] == "none",
           "open under LockAlways: the lock should be released", snap["input"])
    expect(holds == "viewport", f"the sheet should hold the viewport's lock, not {holds}")
    game.key("Escape")
    snap = wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "a frame after the close", lambda s: True)
    seen["lock_always_after_close"] = snap["input"]
    expect(snap["input"]["captor"] == "none" and snap["input"]["cursor_locked"] is True,
           "closed under LockAlways: the lock should be back and no capture forced",
           snap["input"])
    print("  LockAlways: the lock released while the sheet was open, back after the close, and "
          "no capture forced")

    # the showcase's own cursor-showing mode: a HUD click after a close reaches the HUD
    game.call("input_mode", mode="ui")
    clicks = game.snapshot()["world_clicks"]
    game.click("Restore", "game")
    snap = wait_for(game, "the restore screen",
                    lambda s: any(v["kind"] == "restore" for v in s["screens"]))
    expect(snap["world_clicks"] == clicks, "the HUD click reached the world", snap)
    game.key("Escape")
    snap = wait_for(game, "the restore screen to close", no_screen)
    wait_for(game, "the restore status",
             lambda s: "Restore: cancelled" in s["game"]["texts"])
    print("  cursor shown: a HUD click after a close reached the HUD, not the world")
    return seen


def still_owned_after_restart(game: Game) -> None:
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"], 120)
    expect(button(snap["game"], "Unlock") is None and game.call("owns", item=ITEM),
           "Chapter 2 is not owned after the restart")
    print("  restarted: Chapter 2 is still owned")


# Broken e2e configs (the file's text without the control part, the reason the log must give):
# the showcase must refuse each one and quit with exit code 1, never fall back to its built-in
# testnet setup.
BAD_CONFIGS = (
    ("[1, 2]", "not a JSON object"),
    # a config Configure() refuses (the plugin logs "LogScfg: Error: scfg: ...")
    ({"configure": {"wallet": "0x123"}, "items": {}}, "LogScfg: Error: scfg: "),
)


def bad_configs_quit_at_once(make_game, work: Path, control: Control) -> None:
    for i, (content, reason) in enumerate(BAD_CONFIGS):
        path = work / f"bad_config_{i}.json"
        if isinstance(content, str):
            path.write_text(content)
        else:
            write_config(path, content, None, control.port, control.token)
        game = make_game(path, f"bad config {i}")
        try:
            code = game.wait_exit(START_TIMEOUT)
            out = game.output()
        finally:
            game.close()
        errors = [line for line in out.splitlines() if ": Error: " in line]
        if code != 1 or not any(reason in line for line in errors) or READY_LINE in out:
            raise E2EError(f"with the e2e config {content!r} the showcase should log why "
                           f"({reason!r}) and quit with code 1 (it exited with {code}):\n"
                           + "\n".join(out.splitlines()[-40:]))
    print(f"0. bad e2e configs: the showcase quits with code 1 ({len(BAD_CONFIGS)} cases)")


# --- building --------------------------------------------------------------------------------


def run_grouped(cmd: list, log: Path, timeout: float, env: dict | None = None) -> int:
    """Runs cmd in a process group of its own under `timeout`, output to `log`; the group is
    killed on expiry."""
    with log.open("wb") as out:
        proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env,
                                stdin=subprocess.DEVNULL, **new_group_kwargs())
        try:
            return proc.wait(timeout)
        except subprocess.TimeoutExpired:
            kill_group(proc.pid)
            proc.wait()
            raise E2EError(f"{cmd[0]} ran longer than {timeout:g} s (log: {log})") from None
        finally:
            if group_alive(proc.pid):
                kill_group(proc.pid)


def build_editor(engine: Path, work: Path) -> None:
    log = work / "build-editor.log"
    code = run_grouped([sys.executable, str(TOOLS / "run_unreal_build.py"), "--engine",
                        str(engine), "--target", "ScfgDevEditor", "--config", "Development"],
                       log, BUILD_TIMEOUT)
    if code != 0:
        raise E2EError(f"the editor build failed:\n{log.read_text(errors='replace')[-4000:]}")


def buildcookrun_command(engine: Path, archive: Path) -> list:
    """UAT's BuildCookRun of a Development Linux game, staged, in paks, archived to `archive`.
    UBT's build accelerator listens on 127.0.0.1 only (-UBAHost through -ubtargs: UAT does not
    forward its own); the cooker (an editor process) launches no zen server and uses a DDC graph
    without one, as tools/run_unreal_tests.py's editor runs do (the project's DefaultGame.ini turns
    the zen store off for the cooked output)."""
    return [str(engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.sh"), "BuildCookRun",
            f"-project={PROJECT}", "-platform=Linux", "-clientconfig=Development", "-build",
            "-cook", "-stage", "-pak", "-archive", f"-archivedirectory={archive}",
            "-unattended", "-utf8output", "-nop4", "-ubtargs=-UBAHost=127.0.0.1",
            "-NoZenAutoLaunch",
            f"-AdditionalCookerOptions=-ddc={ddc_graph(engine)} -notraceserver"]


def staged_game(archive: Path) -> Path:
    """The staged game's binary under the archive directory."""
    found = sorted(archive.glob("Linux*/ScfgDev/Binaries/Linux/ScfgDev"))
    if not found:
        raise E2EError(f"no staged game under {archive}")
    return found[0]


def build_packaged(engine: Path, work: Path) -> Path:
    archive = work / "archive"
    log = work / "buildcookrun.log"
    code = run_grouped(buildcookrun_command(engine, archive), log, BUILD_TIMEOUT)
    text = log.read_text(errors="replace")
    if code != 0 or "BUILD SUCCESSFUL" not in text:
        raise E2EError(f"BuildCookRun failed ({code}, log {log}):\n{text[-4000:]}")
    game = staged_game(archive)
    # A plugin outside the project (AdditionalPluginDirectories) is staged under RemappedPlugins/
    web = sorted(archive.glob("Linux*/**/Scfg/Resources/Web/index.html"))
    if not web:
        raise E2EError(f"the staged game has no Scfg/Resources/Web (archive {archive})")
    print(f"  staged: {game.relative_to(archive)}; page files at "
          f"{web[0].parent.relative_to(archive)}")
    return game


# --- the run ---------------------------------------------------------------------------------


def run_inner(args) -> int:
    started = time.monotonic()
    why = own_xvfb(os.environ.get("DISPLAY"), os.getpid(),
                   int(os.environ.get(OUTER_PID_ENV, "0")), read_processes())
    if why is not None:
        print(f"Unreal e2e: refusing to start a game: {why}")
        return 2
    sys.path.insert(0, str(TOOLS))
    import run_godot_e2e as godot  # noqa: E402  (the chain families and the browser launch)

    fam_cls = godot.FAMILIES[args.family]
    exe = shutil.which(fam_cls.tool)
    if not exe:
        print(fam_cls.missing)
        return 2
    from playwright.sync_api import sync_playwright

    work = Path(tempfile.mkdtemp(prefix="scfg-unreal-e2e-"))
    shots = Path(args.screenshots).resolve() if args.screenshots else work
    shots.mkdir(parents=True, exist_ok=True)
    engine = Path(args.engine)
    fam = None
    games = []
    control = Control()
    times = {}
    mode = "packaged" if args.packaged else "default"
    try:
        print(f"Unreal e2e: {args.family}, {mode} mode, DISPLAY {os.environ['DISPLAY']} "
              f"(Xvfb of this run), work {work}")
        t = time.monotonic()
        staged = None
        if args.packaged:
            staged = build_packaged(engine, work)
        elif not args.no_build:
            build_editor(engine, work)
        times["build"] = time.monotonic() - t
        user_dir = work / "userdir"
        env = game_env(os.environ, work / "home")

        def make_game(config: Path, label: str) -> Game:
            game = Game(game_command(engine, config, user_dir, staged), env,
                        work / f"{label.replace(' ', '_')}.log", control, label)
            games.append(game)
            return game

        t = time.monotonic()
        bad_configs_quit_at_once(make_game, work, control)
        times["bad_configs"] = time.monotonic() - t
        fam = fam_cls(exe, work)
        config = work / "e2e_config.json"
        write_config(config, fam.config(), fam.chain(), control.port, control.token)
        outputs = []
        with sync_playwright() as p:
            browser = godot.launch_chromium(p)
            t = time.monotonic()
            game = make_game(config, "game1")
            times["start1"] = game.connect()
            game.focus()
            saved = Path(game.call("saved_dir"))
            state_file = saved / "Scfg" / "state.json"
            if not state_file.resolve().is_relative_to(work.resolve()):
                raise E2EError(f"the game's saved data is not isolated: {state_file}")
            print(f"1. unlock by manual transfer (game started in {times['start1']:.1f} s)")
            t = time.monotonic()
            unlock_by_manual_transfer(game, fam, state_file, shots, args.family)
            times["unlock"] = time.monotonic() - t
            print("2. tip through the browser wallet")
            t = time.monotonic()
            tip_by_browser_wallet(game, fam, browser, state_file)
            times["tip"] = time.monotonic() - t
            print("3. real-window input")
            t = time.monotonic()
            seen = real_window_input(game)
            times["real_window"] = time.monotonic() - t
            (work / "real_window.json").write_text(json.dumps(seen, indent=1))
            code = game.quit()
            outputs.append(game.output())
            game.close()
            if code != 0:
                raise E2EError(f"the game exited with {code}:\n{game.tail()}")
            print("4. restart")
            t = time.monotonic()
            game = make_game(config, "game2")
            times["start2"] = game.connect()
            game.focus()
            still_owned_after_restart(game)
            code = game.quit()
            outputs.append(game.output())
            game.close()
            if code != 0:
                raise E2EError(f"the restarted game exited with {code}:\n{game.tail()}")
            times["restart"] = time.monotonic() - t
            browser.close()
        bad = [line for out in outputs for line in problems(out)]
        if bad:
            raise E2EError("the game logged errors or warnings of ours, or crashed:\n"
                           + "\n".join(bad))
    except Exception as e:
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(f"Unreal e2e ({args.family}, {mode}) FAILED after "
              f"{time.monotonic() - started:.1f} s (logs in {work})")
        return 1
    finally:
        for game in games:
            game.close()
        control.close()
        if fam is not None:
            fam.cleanup()
    shown = ", ".join(f"{k} {v:.1f} s" for k, v in times.items())
    print(f"Unreal e2e ({args.family}, {mode}) PASSED in {time.monotonic() - started:.1f} s "
          f"({shown}); logs in {work}")
    if not args.keep:
        shutil.rmtree(work / "archive", ignore_errors=True)
    return 0


def run_outer(argv: list) -> int:
    """Re-runs this script under `xvfb-run -a` in a session of its own, under RUN_TIMEOUT; the
    whole session is killed at the end, whatever is left in it."""
    if not shutil.which("xvfb-run"):
        print("Unreal e2e: xvfb-run not found (install xvfb); the game never runs on your display")
        return 2
    token = secrets.token_hex(8)
    proc = subprocess.Popen(outer_command(argv), env=outer_env(os.environ, token, os.getpid()),
                            start_new_session=True)
    sid = proc.pid

    def stop(signum, frame):
        kill_session(sid)
        raise SystemExit(128 + signum)

    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, stop)
    try:
        code = proc.wait(RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        print(f"Unreal e2e: the run took longer than {RUN_TIMEOUT:g} s; killing it")
        code = 1
    left = kill_session(sid)
    if left:
        print(f"Unreal e2e: killed {left} process(es) left in the run's session")
    return code


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--family", choices=("arc", "evm", "solana"), default="evm",
                    help="chain family (default: evm)")
    ap.add_argument("--packaged", action="store_true",
                    help="BuildCookRun a Development game and run the staged game")
    ap.add_argument("--engine", default=str(DEFAULT_ENGINE), help="the UE 5.8 install")
    ap.add_argument("--no-build", action="store_true",
                    help="default mode: use the editor build as it is")
    ap.add_argument("--screenshots", help="where the sheet's screenshot goes (default: the "
                    "run's temporary directory)")
    ap.add_argument("--keep", action="store_true", help="keep the packaged archive")
    args = ap.parse_args(argv)
    if os.environ.get(INNER_ENV):
        return run_inner(args)
    return run_outer(argv)


if __name__ == "__main__":
    raise SystemExit(main())
