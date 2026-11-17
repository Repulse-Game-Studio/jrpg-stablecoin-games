#!/usr/bin/env python3
"""Build an Unreal target of the Scfg dev project with UBT, under a timeout; fail on any warning.

    uv run python tools/run_unreal_build.py [--target ScfgDevEditor] [--config Development]
        [--engine <dir>] [--project <uproject>] [--timeout <s>] [--log <file>]
        [-- <more UBT arguments, e.g. -DisableUnity>]

Runs `<engine>/Engine/Build/BatchFiles/Linux/Build.sh <target> Linux <config>
-Project=<uproject> -WaitMutex -NoHotReload -UBAHost=127.0.0.1` (UBT's build accelerator would
otherwise listen on every interface, 0.0.0.0:1345). The engine is --engine, else $UE_ROOT, else
/mnt/files/UnrealEngine. UBT's output is passed through as it comes and, with --log, saved too.

The build counts as green only when UBT exits 0 *and* its output has no `warning:` or `error:`
line, except one located in an engine file and reached through engine files only (its "In file
included from" chain, if any, is the engine's too): an engine header warning that our code
triggers fails the build, and so does any message of UBT's own, which names no file. A build that
takes longer than --timeout seconds (default 3600) fails: every process in its process group is
killed, even when Build.sh itself has already exited and only a descendant is left. UBT's cached
build plan of the target (Intermediate/Build/<platform>/*/<target>/<config>/Makefile.bin) is deleted
first, since UBT would reuse it even after a changed .uplugin or -DisableUnity (drop_makefile). A
game target (not *Editor) fails too when its link inputs hold a test-only module
(game_link_problem). SIGTERM, SIGINT or SIGHUP kills UBT's process group before the wrapper
exits.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENGINE = "/mnt/files/UnrealEngine"
DEFAULT_PROJECT = ROOT / "unreal" / "Project" / "ScfgDev" / "ScfgDev.uproject"
DEFAULT_TIMEOUT = 3600.0
KILL_GRACE = 5.0   # seconds the output may take to close after the kill
# Kill the child's group and exit on these: an outer `timeout`, a Ctrl-C, a closed terminal or ssh.
SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
DIAGNOSTIC = re.compile(r"\b(warning|error)\b(\s+[A-Z]+\d+)?\s*:", re.IGNORECASE)
# clang/gcc `path:line[:col]: warning:` and MSVC `path(line[,col]): warning C4251:`
LOCATED = re.compile(r"^\s*(?P<path>[^\s:(][^:(]*?)(:\d+(:\d+)?|\(\d+(,\d+)?\))\s*:")
INCLUDED = re.compile(r"^\s*(In file included from|from)\s+(?P<path>.+?):\d+(:\d+)?[:,]?\s*$")


def in_engine(path: str, engine: Path) -> bool:
    """path is a file of the engine: absolute under it, or relative to Engine/Source (UBT's
    working directory) and existing there."""
    p = Path(path)
    if p.is_absolute():
        return p.resolve().is_relative_to(engine.resolve())
    return (engine / "Engine" / "Source" / p).exists()


def our_diagnostics(output: str, engine: Path) -> list[str]:
    """The warning and error lines of a UBT log that are not the engine's own business."""
    found, chain = [], []
    for line in output.splitlines():
        inc = INCLUDED.match(line)
        if inc:
            chain.append(inc.group("path"))
            continue
        if DIAGNOSTIC.search(line):
            loc = LOCATED.match(line)
            engine_only = (loc is not None and in_engine(loc.group("path"), engine)
                           and all(in_engine(p, engine) for p in chain))
            if not engine_only:
                found.append(line)
        chain = []
    return found


def verdict(returncode: int, output: str, engine: Path) -> str | None:
    """None when the build is green, else why it is not."""
    found = our_diagnostics(output, engine)
    if found:
        return (f"{len(found)} warning/error line(s) not confined to the engine, the first: "
                f"{found[0].strip()}")
    if returncode != 0:
        return f"UBT exited with {returncode}"
    return None


def kill_group(pgid: int) -> None:
    """SIGKILL every process of the group, whether or not its leader is still alive."""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass   # the whole group is gone already


def run_streamed(cmd: list[str], timeout: float, log_path: str | None = None,
                 linger: float | None = None) -> tuple[int | None, str]:
    """Run cmd in a session of its own, passing its output through as it comes (and to log_path).

    Returns (its exit code, its output); the code is None when it overran `timeout` seconds and
    every process of its group was killed. linger None: the run lasts until every holder of the
    output has closed it (a UBT compiler left behind is still the build). A number: the run ends
    when cmd exits, and the output is read for at most `linger` more seconds (an editor's detached
    helper may keep it open); the log is closed then all the same. A SIGTERM, SIGINT or SIGHUP to
    this process kills cmd's whole group too, and then exits with 128 + the signal number (an outer
    `timeout`, a Ctrl-C), so no editor or compiler of ours is left behind. Raises OSError when
    cmd cannot be started.
    """
    deadline = time.monotonic() + timeout
    # The handlers go in before the child is started, so no signal can orphan it. They read the
    # child from this cell: while it is empty there is nothing to kill yet, and a signal that
    # lands then (or between the fork and the assignment) is acted on as soon as the child is
    # known.
    cell: dict[str, subprocess.Popen | int | None] = {"proc": None, "pending": None}

    def stop(signum):
        proc_ = cell["proc"]
        kill_group(proc_.pid)
        print(f"{Path(sys.argv[0]).name}: signal {signum}: killed the process group of "
              f"{cmd[0]} (pid {proc_.pid})", file=sys.stderr, flush=True)
        raise SystemExit(128 + signum)

    def on_signal(signum, _frame):
        if cell["proc"] is None:
            cell["pending"] = signum
            return
        stop(signum)

    log = open(log_path, "w", encoding="utf-8") if log_path else None
    log_lock = threading.Lock()   # the pump writes, the main thread closes
    previous = {}
    if threading.current_thread() is threading.main_thread():
        for sig in SIGNALS:
            previous[sig] = signal.signal(sig, on_signal)
    try:
        # A session of its own: a timeout kills cmd's children (UBT's dotnet, compilers) only.
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace", start_new_session=True)
    except BaseException:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        if log:
            log.close()
        raise
    cell["proc"] = proc
    lines: list[str] = []

    def pump():
        # Ends when every process holding the pipe has closed it, which may be after cmd exits: so
        # it runs on its own thread, and the deadline is kept by the main thread.
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            with log_lock:
                if log:
                    log.write(line)
                    log.flush()   # a log someone tails shows progress too
            lines.append(line)

    reader = threading.Thread(target=pump, daemon=True)
    returncode = None
    try:
        if cell["pending"] is not None:
            stop(cell["pending"])
        reader.start()
        if linger is None:
            reader.join(max(0.0, deadline - time.monotonic()))
            timed_out = reader.is_alive()
        else:
            timed_out = False
        if not timed_out:
            try:
                returncode = proc.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                timed_out = True
        if timed_out:
            returncode = None
            kill_group(proc.pid)
            reader.join(KILL_GRACE)   # a holder outside the group may keep it open: give up then
            try:
                proc.wait(timeout=KILL_GRACE)
            except subprocess.TimeoutExpired:
                pass
        elif linger is not None:
            reader.join(min(linger, max(0.0, deadline - time.monotonic())))
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        with log_lock:   # a pump still reading a helper's output writes no more to it
            if log:
                log.close()
                log = None
    return returncode, "".join(lines)


NAME = re.compile(r"[A-Za-z0-9_]+")


def drop_makefile(project: Path, target: str, platform: str, config: str) -> list[Path]:
    """Delete UBT's cached build plan of this target (Makefile.bin), so that the build is planned
    from the rules, descriptors and arguments it is given. UBT 5.8 loads the cached plan even
    under -NoUBTMakefiles (Modes/BuildMode.cs:1293-1315), and keeps it valid when a .uplugin gains
    a module or -DisableUnity is added (neither is among the plan's checks, System/
    TargetMakefile.cs:695-888): the build then silently leaves out the new module or stays unity.
    target, platform and config must be plain names (letters, digits, _), and only a file inside
    the project's Intermediate/Build is deleted; ValueError otherwise. Returns the files deleted."""
    for what, value in (("target", target), ("platform", platform), ("config", config)):
        if not NAME.fullmatch(value):
            raise ValueError(f"{what} {value!r} is not a plain name (letters, digits, _)")
    inside = (Path(project).resolve().parent / "Intermediate" / "Build").resolve()
    base = inside / platform
    found = []
    for f in [*base.glob(f"*/{target}/{config}/Makefile.bin"),
              *base.glob(f"{target}/{config}/Makefile.bin")]:
        if not f.resolve().is_relative_to(inside):
            raise ValueError(f"{f} is outside {inside}")
        found.append(f)
    for f in found:
        f.unlink(missing_ok=True)
    return found


# Modules that must never be linked into a game (Scfg.uplugin: UncookedOnly), and one that must.
UNCOOKED_ONLY = ("ScfgCoreTests", "ScfgTests")
SHIPPED = "ScfgCore"


def game_link_problem(project: Path, target: str, platform: str, config: str) -> str | None:
    """For a game target (a single monolithic binary): None when its link inputs, UBT's
    Intermediate/Build/<platform>/*/<target>/<config>/<binary>.rsp, hold an object of ScfgCore and
    none of an UncookedOnly module, else why not. The receipt (.target) of a monolithic target
    lists no modules, so the link inputs are what tells."""
    binary = target if config == "Development" else f"{target}-{platform}-{config}"
    base = Path(project).resolve().parent / "Intermediate" / "Build" / platform
    found = sorted(base.glob(f"*/{target}/{config}/{binary}.rsp"))
    if not found:
        return f"no link inputs of {binary} under {base} to check for test modules"
    text = found[0].read_text(encoding="utf-8", errors="replace")
    for module in UNCOOKED_ONLY:
        if f"/{module}/" in text:
            return f"{binary} links the test-only module {module} ({found[0]})"
    if f"/{SHIPPED}/" not in text:
        return f"{found[0]} links no {SHIPPED} object: the check would prove nothing"
    return None


def build(engine: Path, project: Path, target: str = "ScfgDevEditor",
          config: str = "Development", platform: str = "Linux",
          timeout: float = DEFAULT_TIMEOUT, log: str | None = None,
          ubt_args: list[str] | tuple[str, ...] = ()) -> str | None:
    """Build target with UBT under the timeout: None when the build is green, else why not. A game
    target (one not named *Editor) must also not link a test-only module (game_link_problem)."""
    build_sh = engine / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    if not build_sh.is_file():
        return f"cannot start {build_sh}: no such file"
    try:
        dropped = drop_makefile(project, target, platform, config)
    except ValueError as e:
        return str(e)
    for f in dropped:
        print(f"run_unreal_build: deleted the cached {f}", flush=True)
    cmd = [str(build_sh), target, platform, config, f"-Project={Path(project).resolve()}",
           "-WaitMutex", "-NoHotReload", "-UBAHost=127.0.0.1", *ubt_args]
    print("run_unreal_build: " + " ".join(cmd), flush=True)
    try:
        returncode, output = run_streamed(cmd, timeout, log)
    except OSError as e:
        return f"cannot start {build_sh}: {e}"
    if returncode is None:
        return f"UBT did not finish within {timeout:g} s and was killed"
    why = verdict(returncode, output, engine)
    if why is None and not target.endswith("Editor"):
        why = game_link_problem(project, target, platform, config)
    return why


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", default="ScfgDevEditor",
                        help="the UBT target (default ScfgDevEditor)")
    parser.add_argument("--config", default="Development",
                        help="Debug, DebugGame, Development, Test or Shipping "
                             "(default Development)")
    parser.add_argument("--platform", default="Linux", help="the target platform (default Linux)")
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT") or DEFAULT_ENGINE,
                        help=f"the engine's root directory (default $UE_ROOT, else "
                             f"{DEFAULT_ENGINE})")
    parser.add_argument("--project", default=str(DEFAULT_PROJECT),
                        help="the .uproject (default the dev project)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"wall-clock limit in seconds; a slower build is killed and fails "
                             f"(default {DEFAULT_TIMEOUT:g})")
    parser.add_argument("--log", help="also write UBT's output to this file")
    parser.add_argument("ubt_args", nargs="*", help="more UBT arguments (after --)")
    args = parser.parse_args(argv)
    if not args.timeout > 0:
        parser.error("--timeout must be a positive number of seconds")
    why = build(Path(args.engine), Path(args.project), args.target, args.config, args.platform,
                args.timeout, args.log, args.ubt_args)
    if why is not None:
        print(f"run_unreal_build: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
