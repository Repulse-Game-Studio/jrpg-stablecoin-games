#!/usr/bin/env python3
"""Build an Unreal target of the Scfg dev project with UBT, under a timeout; fail on any warning.

    python tools/run_unreal_build.py [--target ScfgDevEditor] [--config Development]
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
killed, even when Build.sh itself has already exited and only a descendant is left.
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
    engine = Path(args.engine)
    build_sh = engine / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    cmd = [str(build_sh), args.target, args.platform, args.config,
           f"-Project={Path(args.project).resolve()}", "-WaitMutex", "-NoHotReload",
           "-UBAHost=127.0.0.1", *args.ubt_args]
    print("run_unreal_build: " + " ".join(cmd), flush=True)
    deadline = time.monotonic() + args.timeout
    try:
        # A session of its own: a timeout kills UBT's dotnet and compiler children, nothing else.
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace", start_new_session=True)
    except OSError as e:
        print(f"run_unreal_build: cannot start {build_sh}: {e}", file=sys.stderr)
        return 1
    lines = []
    log = open(args.log, "w", encoding="utf-8") if args.log else None

    def pump():
        # Ends when every process holding the pipe has closed it, which may be after Build.sh
        # exits: so it runs on its own thread, and the deadline is kept by the main thread.
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            if log:
                log.write(line)
                log.flush()   # a log someone tails shows progress too
            lines.append(line)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    try:
        reader.join(max(0.0, deadline - time.monotonic()))
        timed_out = reader.is_alive()
        if not timed_out:
            try:
                returncode = proc.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                timed_out = True
        if timed_out:
            kill_group(proc.pid)
            reader.join(KILL_GRACE)   # a holder outside the group may keep it open: give up then
            try:
                proc.wait(timeout=KILL_GRACE)
            except subprocess.TimeoutExpired:
                pass
    finally:
        if log and not reader.is_alive():
            log.close()
    if timed_out:
        print(f"run_unreal_build: FAILED: UBT did not finish within {args.timeout:g} s "
              f"and was killed", file=sys.stderr)
        return 1
    why = verdict(returncode, "".join(lines), engine)
    if why is not None:
        print(f"run_unreal_build: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
