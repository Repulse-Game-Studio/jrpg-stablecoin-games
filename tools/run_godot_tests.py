#!/usr/bin/env python3
"""Run the headless GDScript test runner (godot/tests/run.gd) and fail closed.

    python tools/run_godot_tests.py --godot <binary> [--timeout <s>] [-- <run.gd arguments>]

By default run.gd gets `--vectors=<repo>/conformance/vectors`. Godot's own output is passed
through. The run counts as green only when Godot exits 0 *and* the runner's summary line
`<n> passed, 0 failed, <n> skipped` (the last one printed) has at least one test passed.
Godot exits 0 even when run.gd itself fails to compile (for example a parse error in a
preloaded script, or a project that was never imported), so the exit code alone is not enough.
A run that takes longer than --timeout seconds (default 900) is killed and fails, so a hung
Godot cannot hold a CI job until the job's own limit.

Import the project once first: `<binary> --headless --path godot --import`.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUMMARY = re.compile(r"^(\d+) passed, (\d+) failed, (\d+) skipped$")
DEFAULT_TIMEOUT = 900.0


def verdict(returncode: int, output: str) -> str | None:
    """None when the run is green, else why it is not."""
    summaries = [m for m in (SUMMARY.match(line.strip()) for line in output.splitlines()) if m]
    if not summaries:
        return "the runner printed no summary line (did run.gd fail to compile?)"
    passed, failed, _ = (int(g) for g in summaries[-1].groups())
    if failed:
        return f"{failed} test(s) failed"
    if passed == 0:
        return "no test passed"
    if returncode != 0:
        return f"Godot exited with {returncode}"
    return None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", required=True, help="the Godot 4.3+ binary")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"wall-clock limit in seconds; a slower run is killed and fails "
                             f"(default {DEFAULT_TIMEOUT:g})")
    parser.add_argument("runner_args", nargs="*", help="arguments for run.gd (after --)")
    args = parser.parse_args(argv)
    if not args.timeout > 0:
        parser.error("--timeout must be a positive number of seconds")
    runner_args = args.runner_args or [f"--vectors={ROOT / 'conformance' / 'vectors'}"]
    cmd = [args.godot, "--headless", "--path", str(ROOT / "godot"), "--script", "res://tests/run.gd",
           "--", *runner_args]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace")
    except OSError as e:
        print(f"run_godot_tests: cannot start {args.godot}: {e}", file=sys.stderr)
        return 1
    timed_out = threading.Event()

    def kill():
        if proc.poll() is None:
            timed_out.set()
            proc.kill()   # its stdout closes, which ends the loop below

    timer = threading.Timer(args.timeout, kill)
    timer.daemon = True
    timer.start()
    lines = []
    try:
        for line in proc.stdout:   # passed through as it comes: a CI log shows progress
            sys.stdout.write(line)
            sys.stdout.flush()
            lines.append(line)
        returncode = proc.wait()
    finally:
        timer.cancel()
    if timed_out.is_set():
        print(f"run_godot_tests: FAILED: Godot did not finish within {args.timeout:g} s "
              f"and was killed (a hung test or main loop?)", file=sys.stderr)
        return 1
    why = verdict(returncode, "".join(lines))
    if why is not None:
        print(f"run_godot_tests: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
