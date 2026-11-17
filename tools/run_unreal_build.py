#!/usr/bin/env python3
"""Build an Unreal target of the Scfg dev project with UBT, under a timeout; fail on any warning.

    python tools/run_unreal_build.py [--target ScfgDevEditor] [--config Development]
        [--engine <dir>] [--project <uproject>] [--timeout <s>] [--log <file>]
        [-- <more UBT arguments, e.g. -DisableUnity>]

Runs `<engine>/Engine/Build/BatchFiles/Linux/Build.sh <target> Linux <config>
-Project=<uproject> -WaitMutex -NoHotReload -UBAHost=127.0.0.1` (UBT's build accelerator would
otherwise listen on every interface, 0.0.0.0:1345). The engine is --engine, else $UE_ROOT, else
/mnt/files/UnrealEngine. UBT's output is passed through as it comes and, with --log, saved too.
The build counts as green only when UBT exits 0 *and* no line of its output is a `warning:` or
`error:` about our own code: a path under unreal/ (the plugin, the dev project and their
generated unity files) or one of our modules. A build that takes longer than --timeout seconds
(default 3600) is killed, with every process it started, and fails.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENGINE = "/mnt/files/UnrealEngine"
DEFAULT_PROJECT = ROOT / "unreal" / "Project" / "ScfgDev" / "ScfgDev.uproject"
DEFAULT_TIMEOUT = 3600.0
DIAGNOSTIC = re.compile(r"\b(warning|error)\s*:", re.IGNORECASE)
OURS = re.compile(r"\b(ScfgCore|ScfgDev|Scfg\.uplugin|Plugins/Scfg)\b")


def our_diagnostics(output: str, root: Path = ROOT) -> list[str]:
    """The warning and error lines of a UBT log that are about our code."""
    unreal = str(root / "unreal")
    return [line for line in output.splitlines()
            if DIAGNOSTIC.search(line) and (unreal in line or OURS.search(line))]


def verdict(returncode: int, output: str, root: Path = ROOT) -> str | None:
    """None when the build is green, else why it is not."""
    found = our_diagnostics(output, root)
    if found:
        return (f"{len(found)} warning/error line(s) about our code, the first: "
                f"{found[0].strip()}")
    if returncode != 0:
        return f"UBT exited with {returncode}"
    return None


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
    build_sh = Path(args.engine) / "Engine" / "Build" / "BatchFiles" / "Linux" / "Build.sh"
    cmd = [str(build_sh), args.target, args.platform, args.config,
           f"-Project={Path(args.project).resolve()}", "-WaitMutex", "-NoHotReload",
           "-UBAHost=127.0.0.1", *args.ubt_args]
    print("run_unreal_build: " + " ".join(cmd), flush=True)
    try:
        # A session of its own: a timeout kills UBT's dotnet and compiler children, nothing else.
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace", start_new_session=True)
    except OSError as e:
        print(f"run_unreal_build: cannot start {build_sh}: {e}", file=sys.stderr)
        return 1
    timed_out = threading.Event()

    def kill():
        if proc.poll() is None:
            timed_out.set()
            os.killpg(proc.pid, signal.SIGKILL)   # its stdout closes, which ends the loop below

    timer = threading.Timer(args.timeout, kill)
    timer.daemon = True
    timer.start()
    lines = []
    log = open(args.log, "w", encoding="utf-8") if args.log else None
    try:
        for line in proc.stdout:   # passed through as it comes
            sys.stdout.write(line)
            sys.stdout.flush()
            if log:
                log.write(line)
                log.flush()   # a log someone tails shows progress too
            lines.append(line)
        returncode = proc.wait()
    finally:
        timer.cancel()
        if log:
            log.close()
    if timed_out.is_set():
        print(f"run_unreal_build: FAILED: UBT did not finish within {args.timeout:g} s "
              f"and was killed", file=sys.stderr)
        return 1
    why = verdict(returncode, "".join(lines))
    if why is not None:
        print(f"run_unreal_build: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
