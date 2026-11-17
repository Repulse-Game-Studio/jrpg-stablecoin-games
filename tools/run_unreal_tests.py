#!/usr/bin/env python3
"""Build the Scfg dev project's editor and run its automation tests headless; fail unless all pass.

    python tools/run_unreal_tests.py [--filter Scfg.Core] [--expect <n>] [--engine <dir>]
        [--project <uproject>] [--target ScfgDevEditor] [--timeout <s>] [--build-timeout <s>]
        [--no-build] [--report-dir <dir>] [--log <file>]

First builds --target (Development) through tools/run_unreal_build.py, which fails on any warning
of ours. Then runs

    <engine>/Engine/Binaries/Linux/UnrealEditor-Cmd <uproject>
        -ExecCmds="Automation RunTests <filter>;Quit" -unattended -nullrhi -nosplash -stdout
        -notraceserver -ReportExportPath=<report dir>

in a session of its own under --timeout (default 1800 s; on expiry every process of the session
is killed), passing its output through. -notraceserver keeps the editor from forking an
UnrealTraceServer daemon (Core's TraceAuxiliary.cpp), which would outlive the run and listen on
its own ports. The editor's report, <report dir>/index.json (UTF-8 with a BOM), decides: it prints
"<n> passed, <m> failed" and the run is green only when the editor exited 0, wrote a report,
m == 0, no test was left unrun, n > 0 and, with --expect, n >= the expected count. The report
directory defaults to a new temporary directory, which is kept (its path is printed); a given one
loses any old index.json first, so a stale report never passes.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("run_unreal_build", HERE / "run_unreal_build.py")
run_unreal_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(run_unreal_build)

DEFAULT_FILTER = "Scfg.Core"
DEFAULT_TIMEOUT = 1800.0
# Seconds the editor's output may stay open after it exits (a detached helper, such as the zen
# DDC service it starts, may hold it): the run is over when the editor is.
LINGER = 30.0
SHOWN = 20   # failing tests listed one by one


def editor_command(engine: Path, project: Path, test_filter: str, report_dir: Path) -> list[str]:
    editor = engine / "Engine" / "Binaries" / "Linux" / "UnrealEditor-Cmd"
    return [str(editor), str(Path(project).resolve()),
            f"-ExecCmds=Automation RunTests {test_filter};Quit",
            "-unattended", "-nullrhi", "-nosplash", "-stdout", "-notraceserver",
            f"-ReportExportPath={report_dir}"]


def read_report(index: Path) -> dict | str:
    """The parsed index.json, or why it cannot be read."""
    try:
        report = json.loads(index.read_text(encoding="utf-8-sig"))
    except FileNotFoundError:
        return f"the editor wrote no report ({index} is missing)"
    except (OSError, ValueError) as e:
        return f"{index} cannot be read: {e}"
    if not isinstance(report, dict):
        return f"{index} is not a JSON object"
    return report


def failing(report: dict) -> list[str]:
    """'<path>: <state>: <first error>' for every test that did not succeed."""
    out = []
    for test in report.get("tests") or []:
        state = test.get("state")
        if state == "Success":
            continue
        errors = [entry.get("event", {}).get("message", "")
                  for entry in test.get("entries") or []
                  if entry.get("event", {}).get("type") == "Error"]
        first = f": {errors[0].splitlines()[0]}" if errors else ""
        out.append(f"{test.get('fullTestPath')}: {state}{first}")
    return out


def verdict(returncode: int, report: dict | str, expect: int | None) -> tuple[str, str | None]:
    """(the summary line, None when green else why not)."""
    if isinstance(report, str):
        return "no report", report
    counts = {}
    for key in ("succeeded", "succeededWithWarnings", "failed", "notRun"):
        value = report.get(key, 0)
        counts[key] = value if isinstance(value, int) and not isinstance(value, bool) else -1
    passed = counts["succeeded"] + counts["succeededWithWarnings"]
    summary = f"{passed} passed, {counts['failed']} failed"
    if counts["notRun"]:
        summary += f", {counts['notRun']} not run"
    if min(counts.values()) < 0:
        return summary, "the report's counts are not integers"
    if counts["failed"] or counts["notRun"]:
        return summary, f"{counts['failed'] + counts['notRun']} test(s) did not pass"
    if passed <= 0:
        return summary, "no test ran (does the filter match any test?)"
    if expect is not None and passed < expect:
        return summary, f"expected at least {expect} tests, {passed} ran"
    if returncode != 0:
        return summary, f"the editor exited with {returncode}"
    return summary, None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--filter", default=DEFAULT_FILTER,
                        help=f"the automation test filter (default {DEFAULT_FILTER})")
    parser.add_argument("--expect", type=int,
                        help="fail unless at least this many tests passed")
    parser.add_argument("--engine",
                        default=os.environ.get("UE_ROOT") or run_unreal_build.DEFAULT_ENGINE,
                        help=f"the engine's root directory (default $UE_ROOT, else "
                             f"{run_unreal_build.DEFAULT_ENGINE})")
    parser.add_argument("--project", default=str(run_unreal_build.DEFAULT_PROJECT),
                        help="the .uproject (default the dev project)")
    parser.add_argument("--target", default="ScfgDevEditor",
                        help="the editor target to build first (default ScfgDevEditor)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"wall-clock limit of the editor run in seconds (default "
                             f"{DEFAULT_TIMEOUT:g})")
    parser.add_argument("--build-timeout", type=float, default=run_unreal_build.DEFAULT_TIMEOUT,
                        help=f"wall-clock limit of the build in seconds (default "
                             f"{run_unreal_build.DEFAULT_TIMEOUT:g})")
    parser.add_argument("--no-build", action="store_true",
                        help="run the tests on the binaries as they are")
    parser.add_argument("--report-dir", help="where the editor writes its report "
                                             "(default a new temporary directory)")
    parser.add_argument("--log", help="also write the editor's output to this file")
    args = parser.parse_args(argv)
    if not (args.timeout > 0 and args.build_timeout > 0):
        parser.error("--timeout and --build-timeout must be positive numbers of seconds")
    engine, project = Path(args.engine), Path(args.project)

    if not args.no_build:
        why = run_unreal_build.build(engine, project, args.target, "Development",
                                     timeout=args.build_timeout)
        if why is not None:
            print(f"run_unreal_tests: FAILED: the build failed: {why}", file=sys.stderr)
            return 1

    if args.report_dir:
        report_dir = Path(args.report_dir).resolve()
        report_dir.mkdir(parents=True, exist_ok=True)
    else:
        report_dir = Path(tempfile.mkdtemp(prefix="scfg-unreal-tests-"))
    index = report_dir / "index.json"
    index.unlink(missing_ok=True)

    cmd = editor_command(engine, project, args.filter, report_dir)
    print("run_unreal_tests: " + " ".join(cmd), flush=True)
    try:
        returncode, _ = run_unreal_build.run_streamed(cmd, args.timeout, args.log, linger=LINGER)
    except OSError as e:
        print(f"run_unreal_tests: FAILED: cannot start {cmd[0]}: {e}", file=sys.stderr)
        return 1
    if returncode is None:
        print(f"run_unreal_tests: FAILED: the editor did not finish within {args.timeout:g} s "
              f"and was killed", file=sys.stderr)
        return 1

    report = read_report(index)
    summary, why = verdict(returncode, report, args.expect)
    print(f"run_unreal_tests: {summary} (report: {report_dir})", flush=True)
    if why is not None:
        if isinstance(report, dict):
            failures = failing(report)
            for line in failures[:SHOWN]:
                print(f"  {line}", file=sys.stderr)
            if len(failures) > SHOWN:
                print(f"  ... and {len(failures) - SHOWN} more", file=sys.stderr)
        print(f"run_unreal_tests: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
