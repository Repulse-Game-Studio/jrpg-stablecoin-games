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
     SViewport; in a game that hides the cursor, with a sheet open, the game window is
     deactivated and activated again by a second native window of the game, and the whole
     application is deactivated by another X client taking the X focus and activated again by
     a real X click (XTest) on the sheet; the sheet keeps the mouse and the focus, takes the
     clicks, and gives the capture back when it closes; an open sheet the game hides leaves a
     real key and a real click (where its Next button is) to the game, which keeps its focus and
     capture, and takes them back when it is drawn again; a close under LockAlways gives the
     cursor lock back without forcing a capture; in the cursor-showing configuration a click on
     the empty screen reaches the world, a HUD click after a close reaches the HUD, and a click
     on the dimmed game while a screen is open reaches neither. The sheet's screenshot is
     checked to show the drawn sheet.
  4. Restart the game: Chapter 2 is still owned.

--showcase runs the showcase's boot test instead (tests/unreal_showcase, the twin of
tests/godot_showcase; see "the showcase tour" below): no chain, no browser. The showcase
configures itself (from the project settings, then in code with -scfg-showcase-code), and its
chain-free buttons are pressed with real clicks and their readouts checked; the plugin's HTTP
transport must have posted no request (the driver's http_requests), and the settings' logo
(Content/Showcase/logo.png) must have been found. With --packaged the tour runs on the cooked,
staged game: the settings then come from the staged ini, the logo from the pak (its folder is in
the project's Additional Non-Asset Directories to Package), and the plugin's editor module
(ScfgEditor) must not be in the game.

--showcase --live (opt-in, needs internet; never in CI) also checks the plugin's HTTPS path in
the game: after the second start's 0-request check, the driver's live_https makes one read-only
JSON-RPC call through FScfgHttpTransport per family, eth_blockNumber on Arbitrum Sepolia and
getSlot on Solana Devnet, each to that chain's first built-in testnet RPC url (no wallet, key or
item goes out), and each must answer a number above 0. With --packaged that is the cooked game,
with the engine's certificate bundle the plugin stages (Scfg.Build.cs), which the run checks is
there. The editor's twin is Scfg.Live.Https (SCFG_LIVE=1, unreal/README.md).

--showcase --packaged --live --no-certificates checks that the plugin fails closed (ruling
R5-4): the game runs with its certificate bundle replaced by an empty file
(-ini:Engine:[SSL]:OverrideCertificateBundlePath=<empty>, honoured outside Shipping) and the
system's off (-ini:Engine:[SSL]:bUsePlatformProvidedCertificates=False), so the SSL module has no
root certificate and libcurl could not verify a peer; each live call must then be refused with
"TLS certificate verification unavailable" without being sent (the driver's `sent`). (Deleting
the staged file is not enough: with the dev project's n.VerifyPeer=True, UAT puts the bundle in
the pak too.)

--family evm (default) pays USDC on Anvil (the arbitrum entry pointed at a mock token); arc pays
native USDC on Anvil dressed as Arc; solana pays USDC on solana-test-validator. They need `anvil`
or `solana-test-validator` on PATH, Playwright's Chromium and `solders` (run it with `uv run`).

The game runs only on an X server of its own: the runner re-runs itself under `xvfb-run -a`
(DISPLAY and WAYLAND_DISPLAY dropped first) and refuses to start a game unless DISPLAY names the
Xvfb that xvfb-run started for this run (own_xvfb). Rendering: Vulkan on Mesa's lavapipe
(VK_ICD_FILENAMES / VK_DRIVER_FILES), which the engine accepts only with -AllowCPUDevices
(VulkanRHI.cpp's device choice skips CPU devices otherwise), at the Linux default feature level
SM5. The e2e config asks the game to draw no world (draw_world false: lavapipe draws even the
empty Entry map at under one frame a second); the showcase itself draws it. The game's user data
(HOME, XDG_*, -UserDir) lives in a temporary directory, which a passed run removes (a failed one
removes only the packaged archive, once the games are closed; --keep keeps it all), and its D-Bus
session address points nowhere, so no session bus, portal, gvfs or accessibility daemon is
started for it.

Two modes:
  default     the editor build (tools/run_unreal_build.py, ScfgDevEditor Development) run as a
              game, uncooked: `UnrealEditor <uproject> -game`. Fast.
  --packaged  `RunUAT BuildCookRun -platform=Linux -clientconfig=Development -build -cook -stage
              -pak -archive` into a temporary directory, then the staged game: this checks that
              the plugin's Resources/Web is staged where the page server finds it and that the
              default sheet host works in a cooked game.

Every process (the build, the game, the chain, the browser) runs under a timeout; the game and
the builds are killed by process group. The inner run is one session under xvfb-run; at the end
the outer run ends that session and every process descended from it, including those that
started sessions of their own (Playwright's Chromium), as long as their chain of parents is
alive (a process whose parent died is reparented out of the tree and not found). UAT writes its
logs into the run's directory (uebp_LogFolder, uebp_EngineSavedFolder), checked: a packaged run
fails when UAT's default log folders changed meanwhile, which a UAT run of your own at the same
time also causes (a known false failure). A run fails on every Error or Warning line of
the game logs except the measured engine ones (ALLOWED).

Network: the control socket, Anvil, the game's checkout page and UBA are on 127.0.0.1, on ports
the kernel picks. The game process itself has the engine's trace control listener on
0.0.0.0:1985 (ruling R2-5). solana-test-validator (Agave 4.3) runs with --bind-address 127.0.0.1
(gossip, repair, TVU, TPU on loopback), but its RPC and pubsub (TCP <rpc>, <rpc>+1), its faucet
(TCP) and one UDP socket on port 8000 listen on 0.0.0.0, and Agave 4.3 has no option for those:
an accepted test-time exposure (ruling R2-16; the foreign MCP's TCP 127.0.0.1:8000 is not
touched). Screenshots of the sheet (one per run) go to --screenshots.

    uv run python tools/run_unreal_e2e.py --family solana [--packaged] [--screenshots DIR]
    uv run python tools/run_unreal_e2e.py --showcase [--packaged | --no-build] [--live]
        [--no-certificates] [--screenshots DIR]
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
# The showcase's HUD (ScfgShowcase.h): its buttons by name, and its readout of the owned items.
UNLOCK = f"Unlock.{ITEM}"
TIP = "Tip.1.00"
UNLOCKED = "Owned: Chapter 2: The Fall"
READY_LINE = "showcase: ready"
# Seconds: a first start compiles the shaders on the CPU (minutes); a command may wait on a frame.
START_TIMEOUT = 900.0
REPLY_TIMEOUT = 120.0
GAME_TIMEOUT = 1800.0     # a game process lives at most this long
RUN_TIMEOUT = 7200.0      # the whole inner run (the packaged build included)
BUILD_TIMEOUT = 3600.0
# Lines of a game log that fail a run: every Error and Warning line except ALLOWED, and any crash.
PROBLEM = re.compile(r"\b\w+: (Error|Warning): ")
FATAL = re.compile(r"Ensure condition failed|Assertion failed|Fatal error|=== Critical error ===|"
                   r"Unhandled Exception|Segmentation fault")
# The engine's own warnings, measured in every run: an uncooked -game run loads the editor's
# plugins, and the CSV profiler sanitizes the llvmpipe adapter name; and the two below.
ALLOWED = (
    re.compile(r"\bLogEditorDataStorageUI: Warning: Unable to register widget factory '\w+' as "
               r"purpose '\d+' isn't registered\.$"),
    re.compile(r"\bLogCsvProfiler: Warning: Metadata value sanitized due to invalid characters: "
               r"gpu=\"llvmpipe "),
    # The engine's check of reflected struct members at the end of the initial load builds each
    # struct twice and compares: FDataflowToolNodeSnapshot::Date is FDateTime::Now()
    # (DataflowToolNode.h:47), so the check fails whenever the clock ticks between the two (seen
    # once in a bad-config start, task 4.1); its line and the automation echo of it.
    re.compile(r"\b(LogAutomationTest: Error: )?LogClass: (Error: )?StructProperty "
               r"FDataflowToolNodeSnapshot::Date is not initialized properly"),
    # Ours, by design: the e2e driver refuses every LaunchURL (it records the URL instead, through
    # FCoreDelegates::ShouldLaunchUrl), and the plugin warns that the link did not open (phase 3
    # review P3-1; the scheme only, never the link).
    re.compile(r"\bLogScfg: Warning: scfg: cannot open the (http|ethereum|solana): link: "
               r"LaunchURL cancelled by delegate$"),
)


class E2EError(Exception):
    pass


# --- the X server guard ----------------------------------------------------------------------


@dataclass(frozen=True)
class Proc:
    pid: int
    ppid: int
    argv: tuple
    sid: int = 0
    state: str = "S"


def read_processes(proc_root: Path = Path("/proc")) -> list:
    """Every process this user can see, as Proc(pid, ppid, argv, sid, state)."""
    out = []
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            argv = (entry / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        # "pid (comm) state ppid pgrp session ...": comm may hold spaces and parentheses
        fields = stat[stat.rindex(")") + 2:].split()
        out.append(Proc(int(entry.name), int(fields[1]),
                        tuple(a.decode(errors="replace") for a in argv if a),
                        int(fields[3]), fields[0]))
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


# --- the X server: focus, windows and input of another client ---------------------------------


class XClient:
    """A client of this run's X server through libX11 and libXtst (ctypes): it finds the game's
    window (_NET_WM_PID), gives windows the input focus as a window manager would (Xvfb runs
    none), maps a window of its own (another application), and clicks with XTest (a real X
    button event, which SDL reports as the OS's). An X protocol error raises E2EError instead of
    Xlib's default handler, which would exit the process."""

    _errors: list = []
    _handler = None  # the ctypes callback, kept alive

    def __init__(self, display: str):
        import ctypes
        import ctypes.util

        self.ct = ctypes
        lib, tst = ctypes.util.find_library("X11"), ctypes.util.find_library("Xtst")
        if not lib or not tst:
            raise E2EError("libX11 and libXtst are needed to drive the windows on Xvfb")
        x, t = ctypes.CDLL(lib), ctypes.CDLL(tst)
        ulong, c_int, c_uint, vp = ctypes.c_ulong, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p
        x.XOpenDisplay.restype = vp
        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XDefaultRootWindow.restype = ulong
        x.XDefaultRootWindow.argtypes = [vp]
        x.XInternAtom.restype = ulong
        x.XInternAtom.argtypes = [vp, ctypes.c_char_p, c_int]
        x.XQueryTree.argtypes = [vp, ulong, ctypes.POINTER(ulong), ctypes.POINTER(ulong),
                                 ctypes.POINTER(ctypes.POINTER(ulong)), ctypes.POINTER(c_uint)]
        x.XGetWindowProperty.argtypes = [
            vp, ulong, ulong, ctypes.c_long, ctypes.c_long, c_int, ulong, ctypes.POINTER(ulong),
            ctypes.POINTER(c_int), ctypes.POINTER(ulong), ctypes.POINTER(ulong),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
        x.XGetGeometry.argtypes = [vp, ulong, ctypes.POINTER(ulong), ctypes.POINTER(c_int),
                                   ctypes.POINTER(c_int), *[ctypes.POINTER(c_uint)] * 4]
        x.XCreateSimpleWindow.restype = ulong
        x.XCreateSimpleWindow.argtypes = [vp, ulong, c_int, c_int, c_uint, c_uint, c_uint,
                                          ulong, ulong]
        x.XMapWindow.argtypes = [vp, ulong]
        x.XDestroyWindow.argtypes = [vp, ulong]
        x.XSetInputFocus.argtypes = [vp, ulong, c_int, ulong]
        x.XFree.argtypes = [vp]
        x.XSync.argtypes = [vp, c_int]
        x.XCloseDisplay.argtypes = [vp]
        t.XTestFakeMotionEvent.argtypes = [vp, c_int, c_int, c_int, ulong]
        t.XTestFakeButtonEvent.argtypes = [vp, c_uint, c_int, ulong]
        if XClient._handler is None:
            proto = ctypes.CFUNCTYPE(c_int, vp, vp)

            class XErrorEvent(ctypes.Structure):
                _fields_ = [("type", c_int), ("display", vp), ("resourceid", ulong),
                            ("serial", ulong), ("error_code", ctypes.c_ubyte),
                            ("request_code", ctypes.c_ubyte), ("minor_code", ctypes.c_ubyte)]

            def on_error(display, event):
                e = ctypes.cast(event, ctypes.POINTER(XErrorEvent)).contents
                XClient._errors.append((e.error_code, e.request_code))
                return 0

            XClient._handler = proto(on_error)
            x.XSetErrorHandler.argtypes = [proto]
            x.XSetErrorHandler.restype = vp
            x.XSetErrorHandler(XClient._handler)
        self.x, self.t, self.ulong = x, t, ulong
        self.dpy = x.XOpenDisplay(display.encode())
        if not self.dpy:
            raise E2EError(f"cannot open the X display {display}")
        self.root = x.XDefaultRootWindow(self.dpy)

    def sync(self, what: str) -> None:
        XClient._errors.clear()
        self.x.XSync(self.dpy, 0)
        if XClient._errors:
            codes = list(XClient._errors)
            XClient._errors.clear()
            raise E2EError(f"X error {codes} while {what}")

    def close(self) -> None:
        if self.dpy:
            self.x.XCloseDisplay(self.dpy)
            self.dpy = None

    def window_of(self, pid: int) -> int | None:
        """The largest top-level window of process `pid`, or None."""
        ct, x, ulong = self.ct, self.x, self.ulong
        wm_pid = x.XInternAtom(self.dpy, b"_NET_WM_PID", 1)
        r, parent = ulong(), ulong()
        children, n = ct.POINTER(ulong)(), ct.c_uint()
        if not wm_pid or not x.XQueryTree(self.dpy, self.root, ct.byref(r), ct.byref(parent),
                                          ct.byref(children), ct.byref(n)):
            return None
        windows = [children[i] for i in range(n.value)]
        if children:
            x.XFree(children)
        best, best_area = None, -1
        for w in windows:
            kind, fmt, count, after = ulong(), ct.c_int(), ulong(), ulong()
            data = ct.POINTER(ct.c_ubyte)()
            if x.XGetWindowProperty(self.dpy, w, wm_pid, 0, 1, 0, 6, ct.byref(kind),
                                    ct.byref(fmt), ct.byref(count), ct.byref(after),
                                    ct.byref(data)) != 0 or not data:
                continue
            owner = ct.cast(data, ct.POINTER(ulong))[0] if count.value == 1 else None
            x.XFree(data)
            if owner != pid:
                continue
            gx, gy = ct.c_int(), ct.c_int()
            width, height, border, depth = ct.c_uint(), ct.c_uint(), ct.c_uint(), ct.c_uint()
            x.XGetGeometry(self.dpy, w, ct.byref(r), ct.byref(gx), ct.byref(gy),
                           ct.byref(width), ct.byref(height), ct.byref(border), ct.byref(depth))
            if width.value * height.value > best_area:
                best, best_area = w, width.value * height.value
        self.sync("listing the windows")
        return best

    def focus(self, window: int) -> None:
        self.x.XSetInputFocus(self.dpy, window, 2, 0)  # RevertToParent, CurrentTime
        self.sync("focusing a window")

    def make_window(self) -> int:
        """A mapped window of this client's (another application), not over the game's sheet."""
        w = self.x.XCreateSimpleWindow(self.dpy, self.root, WINDOW[0] + 20, 20, 200, 120, 0, 0, 0)
        self.x.XMapWindow(self.dpy, w)
        self.sync("mapping a window")
        return w

    def destroy(self, window: int) -> None:
        self.x.XDestroyWindow(self.dpy, window)
        self.sync("destroying a window")

    def click(self, at, frame) -> None:
        """A left click at screen position `at` through XTest, as a hand makes one: the pointer
        moves in from 24 pixels away, then onto the spot (SButton clicks only while hovered), then
        the button goes down and up; `frame()` lets the game run a frame between the steps."""
        x, y = round(at[0]), round(at[1])
        for px, py in ((x - 24, y), (x, y)):
            self.t.XTestFakeMotionEvent(self.dpy, -1, px, py, 0)
            self.sync("moving the pointer")
            frame()
        for down in (1, 0):
            self.t.XTestFakeButtonEvent(self.dpy, 1, down, 0)
            self.sync("pressing the button" if down else "releasing the button")
            frame()


def focus_window_of(pid: int, display: str) -> int | None:
    """Gives the X input focus to the largest top-level window of process `pid` on `display`, as a
    window manager focuses the window a player clicks; returns the window id, or None when it has
    none. Xvfb runs no window manager, so nothing else does: without the X focus SDL reports no
    focus gain, the engine has no focus window, and its cursor lock (FLinuxCursor::Lock,
    SDL_SetWindowMouseRect) does nothing."""
    client = XClient(display)
    try:
        window = client.window_of(pid)
        if window is not None:
            client.focus(window)
        return window
    finally:
        client.close()


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


def run_tree(sid: int, processes: list, me: int | None = None) -> list:
    """The live processes of session `sid` and every process descended from one of them, also in
    sessions of their own (Playwright starts the browser in one): what the outer run ends. Found
    by parent links, so only while the chain of parents is alive: a process whose parent died is
    reparented (to pid 1 or a subreaper) and, in a session of its own, is not found."""
    live = [p for p in processes if p.state != "Z" and p.pid != me]
    found = {p.pid for p in live if p.sid == sid}
    grew = True
    while grew:
        grew = False
        for p in live:
            if p.pid not in found and p.ppid in found:
                found.add(p.pid)
                grew = True
    return sorted(found)


def alive(pid: int) -> bool:
    """pid is a process that has not ended (a zombie has)."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2:].split()[0] != "Z"


def kill_session(sid: int, grace: float = 3.0) -> int:
    """Ends the run's processes (run_tree: the session and its descendants) except this one:
    SIGTERM (so an Xvfb removes its lock file), then SIGKILL what is left after `grace` seconds;
    returns how many there were."""
    left = run_tree(sid, read_processes(), me=os.getpid())
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid in left:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + grace
        while sig == signal.SIGTERM and time.monotonic() < deadline:
            if not any(alive(pid) for pid in left):
                return len(left)
            time.sleep(0.1)
    return len(left)


# --- the game --------------------------------------------------------------------------------


def write_config(path: Path, family_config: dict, chain: dict | None, port: int,
                 token: str) -> dict:
    """The driver's e2e config (ScfgE2EDriver.h): the family's configure and items, the chain
    override and the control socket. Written readable by this user only."""
    # lavapipe draws even the empty Entry map at under one frame a second: no world
    config = {**family_config, "control": {"port": port, "token": token}, "draw_world": False}
    if chain is not None:
        config["chain"] = chain
    path.write_text(json.dumps(config, indent=1), encoding="utf-8")
    path.chmod(0o600)
    return config


# A D-Bus address that cannot be used: with no address at all and DISPLAY set, libdbus
# autolaunches a session bus (and with it portals, gvfs and the accessibility bus) in sessions of
# their own, which outlive the game.
NO_BUS = "unix:path=/nonexistent/scfg-e2e-no-bus"


def game_env(environ: dict, home: Path) -> dict:
    """The game's environment: the inner run's (DISPLAY is the Xvfb's) with lavapipe forced, SDL
    on X11, a session bus address that goes nowhere (NO_BUS), no accessibility bridge, no gvfs,
    no caller library path, and HOME/XDG in `home`."""
    env = {k: v for k, v in environ.items()
           if k not in ("WAYLAND_DISPLAY", "LD_LIBRARY_PATH", "XDG_RUNTIME_DIR", "SESSION_MANAGER")}
    env.update({
        "DBUS_SESSION_BUS_ADDRESS": NO_BUS,
        "NO_AT_BRIDGE": "1",
        "GVFS_DISABLE_FUSE": "1",
        "GIO_USE_VFS": "local",
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
    """The lines of a game log that fail a run: an Error or Warning line not in ALLOWED, a crash."""
    return [line for line in log.splitlines()
            if FATAL.search(line)
            or (PROBLEM.search(line) and not any(a.search(line) for a in ALLOWED))]


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


# A negative check holds this long, over at least this many snapshots (one a frame or more):
# something that happens a few frames later is not missed.
HOLD_SECONDS = 0.5
HOLD_SNAPSHOTS = 10


def holds_for(game: Game, what: str, pred, seconds: float = HOLD_SECONDS,
              snapshots: int = HOLD_SNAPSHOTS) -> dict:
    """pred holds on every snapshot for `seconds` and at least `snapshots` snapshots; the last."""
    deadline = time.monotonic() + seconds
    n = 0
    while True:
        snap = game.snapshot()
        n += 1
        if not pred(snap):
            raise E2EError(f"{what}: no longer true at snapshot {n}:\n{json.dumps(snap, indent=1)}")
        if n >= snapshots and time.monotonic() >= deadline:
            return snap


def read_png(path: Path) -> tuple:
    """(width, height, channels, rows) of an 8-bit, non-interlaced RGB or RGBA PNG; rows are
    bytes, filters undone. Enough for the engine's screenshots (no Pillow in this project)."""
    import struct
    import zlib

    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise E2EError(f"{path} is not a PNG")
    pos, idat, head = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = head
    channels = {2: 3, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        raise E2EError(f"{path}: an unexpected PNG format {head}")
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return width, height, channels, rows


# The sheet's panel colour (FScfgStyle::SheetColor, #fbfcfd).
SHEET_RGB = (0xFB, 0xFC, 0xFD)


def sheet_drawn(path: Path, view: dict) -> str | None:
    """None when the screenshot shows the sheet drawn where the snapshot lays it out, else why
    not: the window's size; in the area of the sheet's controls, mostly the panel colour and some
    dark text; the game beside it dimmed."""
    width, height, ch, rows = read_png(path)
    if (width, height) != WINDOW:
        return f"the screenshot is {width}x{height}, not {WINDOW[0]}x{WINDOW[1]}"
    rects = [b["rect"] for b in view["buttons"] if b["rect"][2] > 0]
    left = int(min(r[0] for r in rects))
    top, bottom = int(min(r[1] for r in rects)), int(max(r[1] + r[3] for r in rects))
    panel = dark = total = 0
    for y in range(max(top, 0), min(bottom, height), 2):
        row = rows[y]
        for x in range(max(left, 0), width, 2):
            r, g, b = row[x * ch], row[x * ch + 1], row[x * ch + 2]
            total += 1
            panel += all(abs(v - w) <= 6 for v, w in zip((r, g, b), SHEET_RGB))
            dark += max(r, g, b) < 90
    game = [max(rows[y][x * ch:x * ch + 3])
            for y in range(0, height, 8) for x in range(0, left // 2, 8)]
    if not total or panel / total < 0.4:
        return f"the sheet's area is {panel}/{total} panel-coloured pixels, not mostly the panel"
    if dark / total < 0.003:
        return f"the sheet's area has {dark}/{total} dark pixels: no text drawn"
    if sum(game) / max(len(game), 1) > 140:
        return "the game beside the sheet is not dimmed"
    return None


# --- the scenarios ---------------------------------------------------------------------------


def buy_link_drawn_in_the_sheet(game: Game, snap: dict, shots: Path, family: str) -> None:
    """The method step's buy link (configure's onramp, on by default for the mainnet chains the evm
    and arc families dress as): laid out within the method options' width, its text in one or more
    lines inside it, and drawn (the screenshot). The solana family runs on testnet: no link and no
    hint."""
    if family != "solana":
        # a step's first frame lays its buttons out at height 0: measure once the layout has settled
        def laid_out(s):
            view = sheet(s)
            return view is not None and button(view, "onramp") is not None and all(
                b["rect"][3] > 0 for b in view["buttons"]
                if b["name"] == "onramp" or b["name"].startswith("method_"))

        snap = wait_for(game, "the method step laid out", laid_out)
    view = sheet(snap)
    link = button(view, "onramp")
    if family == "solana":
        expect(link is None and button(view, "onramp_hint") is None,
               "a testnet Solana payment should show no buy link and no hint", view)
        return
    expect(link is not None and link["text"] == "No USDC yet? Buy some with a card",
           "the method step should show the buy link", view)
    options = [b["rect"] for b in view["buttons"] if b["name"].startswith("method_")]
    left = min(r[0] for r in options)
    right = max(r[0] + r[2] for r in options)
    x, _y, w, _h = link["rect"]
    expect(w > 0 and x >= left - 1 and x + w <= right + 1,
           f"the buy link {link['rect']} should lie inside the body ({left}..{right})", view)
    shot = shots / f"unreal-e2e-{family}-method.png"
    game.call("screenshot", path=str(shot), timeout=180)
    why = sheet_drawn(shot, view)
    expect(why is None, f"the screenshot {shot} does not show the sheet: {why}")
    print(f"  method step: the buy link inside the body, {link['rect']} (screenshot {shot})")


def unlock_by_manual_transfer(game: Game, fam, state_file: Path, shots: Path, family: str) -> None:
    wait_for(game, "the Unlock button", lambda s: button(s["game"], UNLOCK) is not None, 120)
    game.click(UNLOCK, "game")
    # The only enabled network is chosen by itself, so the sheet opens on the method step.
    snap = wait_for(game, "the method step", has_button("method_manual"))
    expect(snap["paused"], "the showcase should pause the game while the sheet is open")
    expect(snap["input"]["focus"] == "sheet", "the sheet should have the keyboard focus",
           snap["input"])
    buy_link_drawn_in_the_sheet(game, snap, shots, family)
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
    view = sheet(game.snapshot())
    game.call("screenshot", path=str(shot), timeout=180)
    why = sheet_drawn(shot, view)
    expect(why is None, f"the screenshot {shot} does not show the sheet: {why}")
    print(f"  manual step: send {amount} USDC to {recipient} (screenshot {shot}, the sheet drawn)")
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
    expect(button(snap["game"], UNLOCK) is None, "the Unlock button is still shown")
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
    game.click(TIP, "game")  # right after a close: the click must reach the HUD, not the world
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
    holds_for(game, "after the window activation the sheet keeps the focus and the mouse",
              lambda s: s["input"]["focus"] == "sheet" and s["input"]["captor"] != "viewport"
              and not s["input"]["high_precision"] and has_button("forward", "Next")(s))
    game.click("forward")  # a real click on the sheet after the activation
    wait_for(game, "the method step after the activation", has_button("method_browser"))
    game.key("Escape")
    snap = wait_for(game, "back on the amount step", has_button("forward", "Next"))

    # Another application takes the X focus (a window of this runner's own X client): the whole
    # application is deactivated. On Linux the engine activates it again on a mouse button in
    # its window (FLinuxApplication, not on a focus-in): a player clicks back into the game (a
    # real X click through XTest, on the sheet's title), then on the sheet's Next.
    client = XClient(os.environ["DISPLAY"])
    try:
        other = client.make_window()
        client.focus(other)
        snap = wait_for(game, "the application to be deactivated",
                        lambda s: not s["input"]["app_active"], 30)
        seen["app_deactivated"] = snap["input"]
        expect(has_button("forward", "Next")(snap), "the sheet should stay open", snap)
        client.destroy(other)
        r = button(sheet(snap), "forward")["rect"]
        client.click((r[0] + r[2] / 2, 40), game.snapshot)  # the sheet's header, no control
        snap = wait_for(game, "the application activated by the click",
                        lambda s: s["input"]["app_active"], 30)
        seen["app_activated_by_click"] = snap["input"]
        game.focus()  # a window manager focuses the window clicked
        expect(has_button("forward", "Next")(snap) and snap["world_clicks"] == clicks,
               "the click that activates the game is the sheet's, and does nothing", snap)
        client.click((r[0] + r[2] / 2, r[1] + r[3] / 2), game.snapshot)
    finally:
        client.close()
    snap = wait_for(game, "the X click on Next taken", has_button("method_browser"), 30)
    snap = holds_for(game, "after the application activation the sheet keeps the focus and the "
                     "mouse", lambda s: s["input"]["app_active"] and s["input"]["focus"] == "sheet"
                     and s["input"]["captor"] != "viewport" and not s["input"]["high_precision"]
                     and has_button("method_browser")(s))
    seen["app_reactivated"] = snap["input"]
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
    print("  game-only input, sheet open: the window deactivated and activated again (a second "
          "window), then the application (another X client, back with a real X click); the "
          "sheet kept the focus and the mouse, took the clicks, and gave the capture back")

    # an open sheet the game hides without closing it (a supported state, ruling R2-18), on the
    # game's own widget-path code (GSlateFastWidgetPath: every game's, the automation tests run the
    # editor's): it leaves the game's input alone, and takes it back when it is drawn again
    game.call("open", what="tip")
    snap = wait_for(game, "the tip sheet before the hide", has_button("forward", "Next"))
    nxt = button(sheet(snap), "forward")["rect"]
    game.call("sheet_visibility", shown=False)
    game.call("input_mode", mode="game")  # the game takes its input back (SetInputMode(GameOnly))
    snap = wait_for(game, "the game's capture back while the sheet is hidden",
                    lambda s: s["input"]["captor"] == "viewport", 30)
    before = snap
    game.key("W")
    after_key = game.call("input")
    seen["hidden_after_key"] = after_key
    expect(after_key["captor"] == "viewport" and after_key["focus"] == "viewport"
           and after_key["high_precision"],
           "hidden sheet: a key should leave the game its focus and capture", after_key)
    # a real click where the hidden sheet's Next button is: the world's
    under = game.call("click_at", x=nxt[0] + nxt[2] / 2, y=nxt[1] + nxt[3] / 2)["under"]
    snap = wait_for(game, "the world to get the click through the hidden sheet",
                    lambda s: s["world_clicks"] == before["world_clicks"] + 1, 30)
    seen["hidden_after_click"] = {**snap["input"], "under": under}
    expect(snap["input"]["captor"] == "viewport" and snap["input"]["focus"] == "viewport",
           "hidden sheet: a click should leave the game its focus and capture", snap["input"])
    expect(snap["world_keys"] == before["world_keys"] + 1,
           "hidden sheet: the key should be the world's", snap)
    expect(has_button("forward", "Next")(snap),
           "hidden sheet: the flow should still wait on the amount step (the click was not the "
           "sheet's)", snap)
    game.call("sheet_visibility", shown=True)
    snap = wait_for(game, "the sheet drawn again to take the input back",
                    lambda s: s["input"]["focus"] == "sheet" and s["input"]["captor"] != "viewport"
                    and not s["input"]["high_precision"], 30)
    seen["shown_again"] = snap["input"]
    game.key("Escape")
    wait_for(game, "the sheet to close", no_screen)
    wait_for(game, "the capture given back", lambda s: s["input"]["captor"] == "viewport", 30)
    print("  hidden open sheet: a real key and a real click (where its Next button is) were the "
          "game's, which kept its focus and capture; drawn again, the sheet took them back")

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
    wait_for(game, "the sheet to close", no_screen)
    snap = wait_for(game, "the lock back", lambda s: s["input"]["cursor_locked"] is True, 30)
    snap = holds_for(game, "closed under LockAlways: the lock back and no capture forced",
                     lambda s: s["input"]["captor"] == "none" and s["input"]["cursor_locked"])
    seen["lock_always_after_close"] = snap["input"]
    print("  LockAlways: the lock released while the sheet was open, back after the close, and "
          "no capture forced")

    # the showcase's own cursor-showing mode: a click on the empty screen is the world's ...
    game.call("input_mode", mode="ui")
    clicks = game.snapshot()["world_clicks"]
    empty = (WINDOW[0] * 0.8, WINDOW[1] * 0.85)  # no HUD there, and outside the docked sheet
    under = game.call("click_at", x=empty[0], y=empty[1])["under"]
    snap = wait_for(game, "the world to get the click",
                    lambda s: s["world_clicks"] == clicks + 1, 30)
    seen["world_click"] = under
    # ... a HUD click after a close reaches the HUD, not the world ...
    clicks = snap["world_clicks"]
    game.click("Restore", "game")
    snap = wait_for(game, "the restore screen",
                    lambda s: any(v["kind"] == "restore" for v in s["screens"]))
    expect(snap["world_clicks"] == clicks, "the HUD click reached the world", snap)
    # ... and with a screen open, a click on the dimmed game is the screen's
    under = game.call("click_at", x=empty[0] * 0.5, y=empty[1])["under"]
    seen["click_on_the_dim"] = under
    holds_for(game, f"a click on the dimmed game (on {under}) reaches neither the world nor "
              "closes the screen", lambda s: s["world_clicks"] == clicks
              and any(v["kind"] == "restore" for v in s["screens"]))
    game.key("Escape")
    snap = wait_for(game, "the restore screen to close", no_screen)
    wait_for(game, "the restore status",
             lambda s: "Restore: cancelled" in s["game"]["texts"])
    print("  cursor shown: a click on the empty screen reached the world; a HUD click after a "
          "close reached the HUD, and one on the dimmed game the screen, not the world")
    return seen


def still_owned_after_restart(game: Game) -> None:
    snap = wait_for(game, "the unlocked chapter", lambda s: UNLOCKED in s["game"]["texts"], 120)
    expect(button(snap["game"], UNLOCK) is None and game.call("owns", item=ITEM),
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
        bad = problems(out)
        expected = (len(bad) == 2 and reason in bad[0]
                    and "showcase: the setup failed, quitting with exit code 1" in bad[1])
        if code != 1 or not expected or READY_LINE in out:
            raise E2EError(f"with the e2e config {content!r} the showcase should log why "
                           f"({reason!r}) and quit with code 1 (it exited with {code}):\n"
                           + "\n".join(out.splitlines()[-40:]))
    print(f"0. bad e2e configs: the showcase quits with code 1 ({len(BAD_CONFIGS)} cases)")


# --- the showcase tour (--showcase): the chain-free buttons ---------------------------------------
#
# tests/unreal_showcase's run, the twin of tests/godot_showcase: the showcase configures itself
# (own_config), from the project settings and then, in a second start, in code; the buttons that
# need no chain are pressed with real clicks and the readouts checked. Testnet, a fresh user dir:
# no receipt to re-verify and no Solana wallet, so the start asks no chain anything, and the
# sheets are closed on their first step, before a network is chosen (no session is started).

TOUR_BUTTONS = ("Unlock.chapter2", "Unlock.soundtrack", "Unlock.supporter", "Pay.supporter",
                "Tip.0.50", TIP, "Tip.3.00", "Restore", "Export", "Clear", "Notify")
FROM_SETTINGS = "Configured from: the project settings (Config/DefaultGame.ini)"
FROM_CODE = "Configured from: code (ConfigureInCode)"
CODE_SWITCH = "-scfg-showcase-code"
# The showcase's logo (its project settings' and ConfigureInCode's), relative to the project.
SHOWCASE_LOGO = "Content/Showcase/logo.png"
# What the readouts say before anything was paid.
NOTHING_YET = ("Supported: yes", "Owned: nothing yet", "Tips: 0 USDC",
               "Payments still being watched: 0",
               "None yet. Pay for something, or restore a payment.")


def write_tour_config(path: Path, port: int, token: str) -> dict:
    """The driver's config for the tour: the showcase's own configuration, the control socket."""
    config = {"own_config": True, "control": {"port": port, "token": token}, "draw_world": False}
    path.write_text(json.dumps(config, indent=1), encoding="utf-8")
    path.chmod(0o600)
    return config


def hud(snap: dict) -> list:
    return snap["game"]["texts"]


def logged(text: str):
    """The event log (or any HUD line) has a line containing `text`."""
    return lambda snap: any(text in t for t in hud(snap))


def hud_button(snap: dict, name: str) -> dict | None:
    return button(snap["game"], name)


def closed_on_first_step(game: Game, press: str, what: str, first_step) -> dict:
    """Clicks the HUD button `press`, waits for the screen on its first step (game paused), and
    closes it with Escape; returns the snapshot after the close."""
    game.click(press, "game")
    snap = wait_for(game, f"the {what} on its first step", first_step)
    expect(snap["paused"], f"the showcase should pause the game while the {what} is open", snap)
    game.key("Escape")
    snap = wait_for(game, f"the {what} to close", no_screen)
    return wait_for(game, "the game to run again", lambda s: not s["paused"], 10)


def showcase_tour(game: Game, saved: Path, shots: Path) -> dict:
    """The tour of the showcase configured from the project settings. Returns what it read."""
    seen = {}
    snap = wait_for(game, "the showcase's shop", lambda s: hud_button(s, UNLOCK) is not None, 120)
    names = tuple(b["name"] for b in snap["game"]["buttons"])
    expect(names == TOUR_BUTTONS, f"the HUD's buttons are {names}, not {TOUR_BUTTONS}", snap)
    texts = hud(snap)
    for line in (FROM_SETTINGS, *NOTHING_YET):
        expect(line in texts, f"the HUD should show {line!r}", texts)
    expect(logged("Ready. Configured from the project settings")(snap),
           "the event log should say where the configuration came from", texts)
    seen["buttons"] = list(names)
    print(f"  shop: {len(names)} buttons; {FROM_SETTINGS}; {', '.join(NOTHING_YET[:4])}")

    # Export receipts: the file is written under the game's Saved/, an empty list
    game.click("Export", "game")
    snap = wait_for(game, "ExportReceipts' line", logged("ExportReceipts() wrote "))
    line = next(t for t in hud(snap) if "ExportReceipts() wrote " in t)
    exported = Path(line.split("ExportReceipts() wrote ", 1)[1])
    expect(exported.resolve().is_relative_to(saved.resolve()),
           f"the receipts were exported to {exported}, outside the game's Saved/ ({saved})")
    expect(json.loads(exported.read_text()) == [], f"{exported} should hold no receipt")
    print(f"  Export receipts: wrote {exported.name} (no receipt)")

    # Clear local data
    game.click("Clear", "game")
    wait_for(game, "ClearLocalData's line",
             logged("ClearLocalData(): receipts forgotten on this device."))
    print("  Clear local data: logged")

    # Notifications off and on again
    game.click("Notify", "game")
    snap = wait_for(game, "notifications off", lambda s: logged("SetNotify(false)")(s)
                    and (hud_button(s, "Notify") or {}).get("text") == "Notify: off")
    game.click("Notify", "game")
    snap = wait_for(game, "notifications on", lambda s: logged("SetNotify(true)")(s)
                    and (hud_button(s, "Notify") or {}).get("text") == "Notify: on")
    print("  Notify: off, then on")

    # Restore purchases: the restore screen, closed with Escape
    closed_on_first_step(game, "Restore", "restore screen",
                         lambda s: any(v["kind"] == "restore" for v in s["screens"]))
    snap = wait_for(game, "the restore result", lambda s: "Restore: cancelled" in hud(s)
                    and logged("restore result: cancelled")(s))
    # (at the start the engine gives the viewport the focus when the window is activated)
    expect(snap["input"]["focus"] == "hud", "after a flow the HUD's first button should have "
           "the focus", snap["input"])
    print("  Restore purchases: cancelled; the HUD has the focus back")

    # A tip: the sheet on its amount step, closed with Escape
    closed_on_first_step(game, TIP, "tip sheet", has_button("forward", "Next"))
    snap = wait_for(game, "the tip result", lambda s: "Tip: cancelled" in hud(s)
                    and logged("PayAsync(Tip, Amount 1.00, Token USDC)")(s)
                    and logged("tip result: cancelled")(s))
    print("  Tip 1.00 USDC: cancelled on the amount step")

    # An unlock: the sheet on its network step (the testnets), closed with Escape
    def network_step(s):
        view = sheet(s)
        return view is not None and any(b["name"].startswith("network_") for b in view["buttons"])

    game.click(UNLOCK, "game")
    snap = wait_for(game, "the unlock sheet on its network step", network_step)
    networks = [b["text"] for b in sheet(snap)["buttons"] if b["name"].startswith("network_")]
    expect(any("Sepolia" in n for n in networks),
           f"the unlock sheet should offer the testnets, not {networks}", snap)
    expect(snap["paused"], "the showcase should pause the game while the sheet is open", snap)
    seen["networks"] = networks
    game.key("Escape")
    wait_for(game, "the unlock sheet to close", no_screen)
    snap = wait_for(game, "the unlock result",
                    lambda s: "Maybe later. (Status: cancelled)" in hud(s)
                    and logged("unlock result: cancelled")(s) and not s["paused"])
    print(f"  Unlock Chapter 2: cancelled on the network step ({len(networks)} testnets)")

    # nothing was paid, nothing reached the world
    shot = shots / "unreal-showcase.png"
    game.call("screenshot", path=str(shot), timeout=180)
    print(f"  screenshot {shot}")
    texts = hud(snap)
    for line in NOTHING_YET:
        expect(line in texts, f"after the tour the HUD should still show {line!r}", texts)
    expect(not game.call("owns", item=ITEM), "Owns('chapter2') after a cancelled unlock")
    expect(snap["world_clicks"] == 0, "a HUD click reached the world", snap)
    expect(hud_button(snap, UNLOCK) is not None, "the Unlock button is gone", snap)
    seen["texts"] = texts
    seen["configuration"] = configured_offline_with_the_logo(game, from_settings=True)
    return seen


def configured_offline_with_the_logo(game: Game, from_settings: bool) -> dict:
    """The facade's configuration (from the settings or not) has the showcase's logo, found on
    disk (in a packaged game: in the pak), and the plugin's HTTP transport posted nothing."""
    config = game.call("configuration")
    expect(config["configured"] and config["from_settings"] == from_settings,
           f"the configuration should {'' if from_settings else 'not '}be the settings'", config)
    expect(config["logo"].replace("\\", "/").endswith(SHOWCASE_LOGO) and config["logo_exists"],
           f"the configured logo should be the showcase's {SHOWCASE_LOGO}, found", config)
    requests = game.call("http_requests")
    expect(requests == 0, f"the showcase posted {requests} JSON-RPC request(s): it should ask "
           "no chain anything", requests)
    print(f"  the logo {config['logo']} was found; 0 JSON-RPC requests")
    return {**config, "http_requests": requests}


def showcase_from_code(game: Game, tour: dict) -> None:
    """The second start, with -scfg-showcase-code: configured in code, the same shop."""
    snap = wait_for(game, "the showcase's shop", lambda s: hud_button(s, UNLOCK) is not None, 120)
    names = [b["name"] for b in snap["game"]["buttons"]]
    expect(names == tour["buttons"], f"configured in code the HUD has {names}", snap)
    expect(FROM_CODE in hud(snap) and logged("Ready. Configured from code")(snap),
           f"the HUD should show {FROM_CODE!r}", hud(snap))
    for line in NOTHING_YET:
        expect(line in hud(snap), f"the HUD should show {line!r}", hud(snap))
    print(f"  {FROM_CODE}: the same {len(names)} buttons and readouts")
    configured_offline_with_the_logo(game, from_settings=False)


# The driver waits at most 40 s for both answers (ScfgE2EDriver.cpp, LIVE_PATIENCE).
LIVE_REPLY_TIMEOUT = 60.0
# The transport's refusal when TLS peer verification is unavailable (ScfgHttpTransport.h).
TLS_UNAVAILABLE = "TLS certificate verification unavailable"
# The engine's root certificate bundle as the plugin stages it with a Linux game (Scfg.Build.cs).
STAGED_BUNDLE = Path("Linux", "Engine", "Content", "Certificates", "ThirdParty", "cacert.pem")
# --no-certificates: the bundle replaced by an empty file (SslCertificateManager.cpp:318-326, not in
# Shipping) and the system's bundle off (UnixPlatformSslCertificateManager.cpp:18-22).
def no_certificates_args(empty_bundle: Path) -> list:
    return [f"-ini:Engine:[SSL]:OverrideCertificateBundlePath={empty_bundle}",
            "-ini:Engine:[SSL]:bUsePlatformProvidedCertificates=False"]
# --no-certificates: the transport's reason, logged once per transport (ScfgHttpTransport.cpp).
REFUSAL_LINE = "LogScfg: Error: scfg: refusing https:// RPC requests: " + TLS_UNAVAILABLE
# --no-certificates: the lines that run is meant to log.
REFUSED_ALLOWED = (
    re.compile(r"\bLogScfg: Error: scfg: refusing https:// RPC requests: "
               + re.escape(TLS_UNAVAILABLE) + ": the engine found no root certificates"),
    re.compile(r"\bLogSsl: Warning: Unable to find a cert bundle in any of known locations\."),
)


# The engine's last line before a clean process exit (LaunchEngineLoop's FEngineLoop::Exit).
EXITING = re.compile(r"\bLogExit: Exiting\.$", re.MULTILINE)


# --no-certificates: the exit codes of that teardown crash (SIGSEGV, SIGABRT), as Popen reports a
# signal (-11, -6) or a shell does (139, 134). Any other nonzero exit still fails the run.
TEARDOWN_CRASH_CODES = frozenset({-11, -6, 139, 134})


def before_exit(log: str) -> tuple[str, bool]:
    """--no-certificates: a game whose SSL module holds no root certificate crashes after its last
    log line, in the process's own teardown (measured: SIGSEGV or "free(): invalid size" after
    "LogExit: Exiting.", with -ExecCmds=quit and no plugin call; one certificate is enough to
    avoid it). The log up to that line, and whether it got there."""
    found = EXITING.search(log)
    return (log[:found.end()], True) if found else (log, False)


def live_https(game: Game, refused: bool = False) -> list:
    """--live: the driver's live_https, one read-only call per family over real HTTPS through the
    plugin's transport; each must be answered with a number above 0 or, with `refused`
    (--no-certificates), fail without being sent: the transport's TransportError(TLS_UNAVAILABLE)
    reaches the caller as the RpcClient's RpcUnavailable, as for a payment, and the reason is the
    log line REFUSAL_LINE (checked on the game's log by run_showcase)."""
    calls = game.call("live_https", timeout=LIVE_REPLY_TIMEOUT)
    expect(isinstance(calls, list) and len(calls) == 2, "live_https should answer two calls", calls)
    for call in calls:
        where = f"{call.get('method')} on {call.get('chain')} at {call.get('url')}"
        if refused:
            expect(not call.get("ok") and call.get("sent") is False
                   and str(call.get("error")).startswith(("RpcUnavailable", "TransportError")),
                   f"live {where} should be refused unsent ({TLS_UNAVAILABLE!r})", call)
            print(f"  live {where}: refused, not sent ({call['error']})")
            continue
        expect(call.get("ok") and call.get("url", "").startswith("https://"),
               f"live {where} failed after {call.get('ms')} ms: {call.get('error')}", call)
        expect(int(call["value"]) > 0, f"live {where} answered {call['value']}", call)
        expect(call.get("sent") is not False, f"live {where} was answered but not sent", call)
        print(f"  live {where}: {call['value']} in {call['ms']} ms")
    return calls


def run_showcase(args, started: float) -> int:
    """--showcase: the tour, in the inner run (own_xvfb already checked)."""
    work = Path(tempfile.mkdtemp(prefix="scfg-unreal-showcase-"))
    shots = Path(args.screenshots).resolve() if args.screenshots else work
    shots.mkdir(parents=True, exist_ok=True)
    engine = Path(args.engine)
    games = []
    control = Control()
    times = {}
    failed = False
    try:
        mode = "packaged" if args.packaged else "editor build"
        print(f"Unreal showcase tour ({mode}), DISPLAY {os.environ['DISPLAY']} (Xvfb of this "
              f"run), work {work}")
        t = time.monotonic()
        staged = None
        if args.packaged:
            staged = build_packaged(engine, work)
            found = editor_module_staged(work / "archive", staged)
            if found:
                raise E2EError("the staged game holds the plugin's editor module (ScfgEditor): "
                               + "; ".join(found))
            print("  the staged game holds nothing of ScfgEditor")
            bundle = work / "archive" / STAGED_BUNDLE
            if not bundle.is_file():
                raise E2EError(f"the staged game has no certificate bundle at {STAGED_BUNDLE}")
            print(f"  the staged game has the engine's certificate bundle, {STAGED_BUNDLE}")
        elif not args.no_build:
            build_editor(engine, work)
        times["build"] = time.monotonic() - t
        user_dir = work / "userdir"
        env = game_env(os.environ, work / "home")
        config = work / "showcase_config.json"
        write_tour_config(config, control.port, control.token)
        outputs = []
        tour = None
        no_certs = []
        if args.no_certificates:
            empty = work / "no-certificates.pem"
            empty.write_bytes(b"")
            no_certs = no_certificates_args(empty)
            print("  --no-certificates: " + " ".join(no_certs))
        for label, extra in (("settings", no_certs), ("code", [CODE_SWITCH, *no_certs])):
            t = time.monotonic()
            game = Game(game_command(engine, config, user_dir, staged) + extra, env,
                        work / f"showcase_{label}.log", control, f"showcase ({label})")
            games.append(game)
            start = game.connect()
            game.focus()
            saved = Path(game.call("saved_dir"))
            if not saved.resolve().is_relative_to(work.resolve()):
                raise E2EError(f"the game's saved data is not isolated: {saved}")
            if tour is None:
                print(f"1. configured from the project settings (started in {start:.1f} s)")
                tour = showcase_tour(game, saved, shots)
            else:
                print(f"2. configured in code, {CODE_SWITCH} (started in {start:.1f} s)")
                showcase_from_code(game, tour)
                if args.live:
                    print("3. live HTTPS through the plugin's transport (--live)")
                    tour["live"] = live_https(game, refused=args.no_certificates)
            code = game.quit()
            output = game.output()
            tail = game.tail()  # before close(), which closes the log
            game.close()
            if args.no_certificates and code in TEARDOWN_CRASH_CODES:
                output, exited = before_exit(output)
                if exited:
                    print(f"  exit code {code}: the engine's crash after 'LogExit: Exiting.' "
                          "without root certificates (before_exit)")
                    code = 0
            outputs.append(output)
            if code != 0:
                raise E2EError(f"the showcase exited with {code}:\n{tail}")
            if args.live and args.no_certificates and label == "code":
                expect(REFUSAL_LINE in output, f"the game should log {REFUSAL_LINE!r}", tail)
                print(f"  the game logged: {REFUSAL_LINE}: ...")
            times[label] = time.monotonic() - t
        bad = [line for out in outputs for line in problems(out)
               if not (args.no_certificates and any(a.search(line) for a in REFUSED_ALLOWED))]
        if bad:
            raise E2EError("the game logged errors or warnings (not in ALLOWED), or crashed:\n"
                           + "\n".join(bad))
        (work / "tour.json").write_text(json.dumps(tour, indent=1))
    except Exception as e:
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(f"Unreal showcase tour FAILED after {time.monotonic() - started:.1f} s "
              f"(logs in {work})")
        failed = True
        return 1
    finally:
        for game in games:
            game.close()
        control.close()
        if failed and not args.keep:
            remove_work(work / "archive")  # the logs stay, the 1 GB archive goes
    shown = ", ".join(f"{k} {v:.1f} s" for k, v in times.items())
    kept = work if args.keep else None
    if kept is None:
        remove_work(work)
    print(f"Unreal showcase tour PASSED in {time.monotonic() - started:.1f} s ({shown}); "
          + (f"logs in {kept}" if kept else "work directory removed (--keep keeps it)"))
    return 0


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


# The plugin's editor module (Scfg.uplugin: ScfgEditor, Type Editor) in a staged game: a file named
# after it, or its module name or a text only it has in the game binary (UTF-8 or UTF-16, TCHAR).
EDITOR_MODULE = "ScfgEditor"
EDITOR_MARKS = (EDITOR_MODULE, "Show the SCFG log")


def editor_module_staged(archive: Path, game: Path) -> list:
    """What of the plugin's editor module the staged game holds (empty: nothing)."""
    found = [f"file {p.relative_to(archive)}" for p in sorted(archive.rglob(f"*{EDITOR_MODULE}*"))]
    data = game.read_bytes()
    for mark in EDITOR_MARKS:
        for encoding in ("utf-8", "utf-16-le"):
            if mark.encode(encoding) in data:
                found.append(f"{mark!r} ({encoding}) in {game.name}")
    return found


def uat_env(environ: dict, work: Path) -> dict:
    """UAT's environment: its logs and saved files in the run's directory. Without these an
    installed engine's UAT logs to ~/Library/Logs/Unreal Engine/LocalBuildLogs and
    Engine/Programs/AutomationTool/Saved, and clears the log folder first when it is the only UAT
    running (AutomationUtils/CommandEnvironment.cs): a developer's own UAT logs would go."""
    return {**environ, "uebp_LogFolder": str(work / "uat-logs"),
            "uebp_EngineSavedFolder": str(work / "uat-saved")}


# Where UAT writes by default (uat_env keeps it out of both): checked unchanged by a packaged run.
# The check sees the whole folders, not only our run's files: a UAT run of the developer's own
# (another BuildCookRun, the editor's packaging) during a packaged run fails it too (a known false
# failure: run it again when nothing else packages).
UAT_DEFAULT_LOGS = (Path.home() / "Library" / "Logs" / "Unreal Engine" / "LocalBuildLogs",)


def newest_mtime(paths) -> float:
    newest = 0.0
    for root in paths:
        if root.exists():
            for f in [root, *root.rglob("*")]:
                try:
                    newest = max(newest, f.stat().st_mtime)
                except OSError:
                    pass
    return newest


def changed_since(paths, since: float, limit: int = 10) -> list:
    """The files and folders under `paths` modified after `since` (at most `limit`), to name what
    the UAT check saw: a removal shows only as its folder's change."""
    found = []
    for root in paths:
        if root.exists():
            for f in [root, *root.rglob("*")]:
                try:
                    if f.stat().st_mtime > since:
                        found.append(f)
                except OSError:
                    pass
    return sorted(found)[:limit]


def build_packaged(engine: Path, work: Path) -> Path:
    archive = work / "archive"
    log = work / "buildcookrun.log"
    defaults = (*UAT_DEFAULT_LOGS, engine / "Engine" / "Programs" / "AutomationTool" / "Saved")
    before = newest_mtime(defaults)
    code = run_grouped(buildcookrun_command(engine, archive), log, BUILD_TIMEOUT,
                       env=uat_env(os.environ, work))
    if newest_mtime(defaults) != before:
        seen = "".join(f"\n  {f}" for f in changed_since(defaults, before))
        raise E2EError("UAT wrote outside the run's directory (under "
                       f"{', '.join(map(str, defaults))}):{seen or ' a file removed'}\n"
                       "(a UAT run of your own at the same time trips this check too: run again "
                       "when nothing else packages)")
    if not any((work / "uat-logs").rglob("*")):
        raise E2EError(f"UAT wrote no log into {work / 'uat-logs'}")
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


def remove_work(path: Path) -> None:
    """Removes a directory of this run, unless something is mounted under it (a FUSE helper left
    running, which rmtree would descend into): then it says so and leaves it."""
    if not path.exists():
        return
    mounts = [line.split()[1] for line in Path("/proc/mounts").read_text().splitlines()]
    inside = [m for m in mounts if Path(m) == path or path in Path(m).parents]
    if inside:
        print(f"Unreal e2e: {path} kept: something is mounted under it ({inside})")
        return
    shutil.rmtree(path, ignore_errors=True)


def run_inner(args) -> int:
    started = time.monotonic()
    why = own_xvfb(os.environ.get("DISPLAY"), os.getpid(),
                   int(os.environ.get(OUTER_PID_ENV, "0")), read_processes())
    if why is not None:
        print(f"Unreal e2e: refusing to start a game: {why}")
        return 2
    if args.showcase:
        return run_showcase(args, started)
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
    failed = False
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
            tail = game.tail()  # before close(), which closes the log
            game.close()
            if code != 0:
                raise E2EError(f"the game exited with {code}:\n{tail}")
            print("4. restart")
            t = time.monotonic()
            game = make_game(config, "game2")
            times["start2"] = game.connect()
            game.focus()
            still_owned_after_restart(game)
            code = game.quit()
            outputs.append(game.output())
            tail = game.tail()  # before close(), which closes the log
            game.close()
            if code != 0:
                raise E2EError(f"the restarted game exited with {code}:\n{tail}")
            times["restart"] = time.monotonic() - t
            browser.close()
        bad = [line for out in outputs for line in problems(out)]
        if bad:
            raise E2EError("the game logged errors or warnings (not in ALLOWED), or crashed:\n"
                           + "\n".join(bad))
    except Exception as e:
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(f"Unreal e2e ({args.family}, {mode}) FAILED after "
              f"{time.monotonic() - started:.1f} s (logs in {work})")
        failed = True
        return 1
    finally:
        for game in games:
            game.close()
        control.close()
        if fam is not None:
            fam.cleanup()
        if failed and not args.keep:
            # once the games are closed: the logs stay, the 1 GB archive goes
            remove_work(work / "archive")
    shown = ", ".join(f"{k} {v:.1f} s" for k, v in times.items())
    kept = work if args.keep else None
    if kept is None:
        remove_work(work)
    print(f"Unreal e2e ({args.family}, {mode}) PASSED in {time.monotonic() - started:.1f} s "
          f"({shown}); "
          + (f"logs in {kept}" if kept else "work directory removed (--keep keeps it)"))
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
                    "run's temporary directory, removed with it when the run passes)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the whole work directory (logs, the game's user data, the "
                    "packaged archive); without it a passed run removes it and a failed one "
                    "removes only the archive")
    ap.add_argument("--showcase", action="store_true",
                    help="the showcase tour instead of the payments: its chain-free buttons, "
                    "configured from the project settings, then in code (no chain, no browser; "
                    "tests/unreal_showcase); --family does not apply; with --packaged, on the "
                    "cooked game")
    ap.add_argument("--no-certificates", action="store_true",
                    help="with --showcase --packaged --live: replace the certificate bundle with "
                    "an empty override file and turn the system's off; the live calls must be "
                    "refused unsent")
    ap.add_argument("--live", action="store_true",
                    help="with --showcase: also one read-only HTTPS JSON-RPC call per family "
                    "(Arbitrum Sepolia eth_blockNumber, Solana Devnet getSlot) through the "
                    "plugin's transport in the game; needs internet, sends nothing of the user's")
    args = ap.parse_args(argv)
    if args.live and not args.showcase:
        ap.error("--live goes with --showcase")
    if args.no_certificates and not (args.live and args.packaged):
        ap.error("--no-certificates goes with --showcase --packaged --live")
    if os.environ.get(INNER_ENV):
        return run_inner(args)
    return run_outer(argv)


if __name__ == "__main__":
    raise SystemExit(main())
