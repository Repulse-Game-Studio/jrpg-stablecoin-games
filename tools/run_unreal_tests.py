#!/usr/bin/env python3
"""Build the Scfg dev project's editor and run its automation tests headless; fail unless all pass.

    uv run python tools/run_unreal_tests.py [--filter Scfg.Core] [--expect <n>] [--engine <dir>]
        [--project <uproject>] [--target ScfgDevEditor] [--timeout <s>] [--build-timeout <s>]
        [--no-build] [--report-dir <dir>] [--log <file>]

First builds --target (Development) through tools/run_unreal_build.py, which fails on any warning
of ours. Then runs

    <engine>/Engine/Binaries/Linux/UnrealEditor-Cmd <uproject>
        -ExecCmds="Automation RunTests <filter>;Quit" -unattended -nullrhi -nosplash -stdout
        -notraceserver -NoZenAutoLaunch -ddc=<graph without ZenLocal>
        -ReportExportPath=<report dir> -abslog=<report dir>/editor.log

The engine is --engine, else $UE_ROOT; with neither the run exits with 2 at once. The editor runs
in a session of its own under --timeout (default 1800 s; on expiry, or on a SIGTERM, SIGINT or
SIGHUP to this script, every process of the session is killed), passing its output through.

What the flags keep off the network: -notraceserver keeps the editor from forking an
UnrealTraceServer daemon (Core's TraceAuxiliary.cpp:2365). -NoZenAutoLaunch
(ZenServerInterface.cpp:1753) keeps it from launching, or restarting, the per-user zen DDC service:
auto-launch listens on every interface (BaseEngine.ini's RemoteNetworkService=GeneratedStaticKey,
ZenServerInterface.cpp:2085-2089), and it shuts down a running instance whose command line
differs from its own (:4153-4190, :4221-4237), which may be another editor's. The DDC graph
without ZenLocal (InstalledNoZenLocalFallback on an installed engine, else NoZenLocalFallback)
uses the file cache instead. What stays: the editor's trace control port, 0.0.0.0:1985, which
has no switch (unreal/README.md, "Under UBT").

The editor's report, <report dir>/index.json (UTF-8 with a BOM), decides: it prints "<n> passed,
<m> failed" and the run is green only when the editor exited 0, wrote a report, m == 0, no test
was left unrun or in process, every entry of its tests list succeeded and their number is n,
n > 0 and, with --expect, n >= that minimum, and no store opened the project's own
Saved/Scfg/state.json (the editor's full log, <report dir>/editor.log, which has the Log lines
its output leaves out, has no "scfg: the payment state file is <project>/Saved/Scfg/state.json"
line: a test's facade has a state directory of its own, and a test that makes a real game
instance starts it from blank project settings; ScfgTestsModule.cpp refuses that file, and fails
the test, while a test runs). The vectors the run read are printed (from
Scfg.Core.NothingLeftOut's entry), and whether $SCFG_VECTORS chose them. The report directory
defaults to a new temporary directory, which is kept (its path is printed); a given one loses any
old index.json first, so a stale report never passes.

The opt-in live test Scfg.Live.Https (two read-only HTTPS JSON-RPC calls to built-in public
testnet RPCs through the plugin's transport) is registered only when the environment has
SCFG_LIVE=1, which the editor inherits: `SCFG_LIVE=1 ... --filter Scfg.Live`.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_unreal_build  # noqa: E402
from unreal_common import (  # noqa: E402
    DEFAULT_PROJECT,
    ENGINE_HELP,
    NO_ENGINE,
    ddc_graph,
    engine_root,
)

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
            "-NoZenAutoLaunch", f"-ddc={ddc_graph(engine)}",
            f"-ReportExportPath={report_dir}", f"-abslog={report_dir / 'editor.log'}"]


def vectors_read(report: dict) -> str | None:
    """The vectors directory Scfg.Core.NothingLeftOut logged ("vectors: <dir>"), if it ran."""
    for test in report.get("tests") or []:
        for entry in test.get("entries") or []:
            message = entry.get("event", {}).get("message", "")
            if test.get("fullTestPath") == "Scfg.Core.NothingLeftOut" and message.startswith(
                    "vectors: "):
                return message[len("vectors: "):]
    return None


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


STATE_LINE = "scfg: the payment state file is "


def opened_project_state(output: str, project: Path) -> list[str]:
    """The output's lines where a store opened the project's own Saved/Scfg/state.json (FScfgStore
    logs "scfg: the payment state file is <path>" for each file it opens)."""
    folder = Path(os.path.abspath(project)).parent
    targets = {str(folder / "Saved" / "Scfg" / "state.json"),
               str(folder.resolve() / "Saved" / "Scfg" / "state.json")}
    return [line for line in output.splitlines()
            if STATE_LINE in line and line.split(STATE_LINE, 1)[1].strip() in targets]


def verdict(returncode: int, report: dict | str, expect: int | None) -> tuple[str, str | None]:
    """(the summary line, None when green else why not)."""
    if isinstance(report, str):
        return "no report", report
    counts = {}
    for key in ("succeeded", "succeededWithWarnings", "failed", "notRun", "inProcess"):
        value = report.get(key, 0)
        counts[key] = value if isinstance(value, int) and not isinstance(value, bool) else -1
    passed = counts["succeeded"] + counts["succeededWithWarnings"]
    summary = f"{passed} passed, {counts['failed']} failed"
    if counts["notRun"]:
        summary += f", {counts['notRun']} not run"
    if counts["inProcess"]:
        summary += f", {counts['inProcess']} in process"
    if min(counts.values()) < 0:
        return summary, "the report's counts are not integers"
    if counts["failed"] or counts["notRun"] or counts["inProcess"]:
        not_passed = counts["failed"] + counts["notRun"] + counts["inProcess"]
        return summary, f"{not_passed} test(s) did not pass"
    tests = report.get("tests")
    if not isinstance(tests, list):
        return summary, "the report has no tests list"
    states = [t.get("state") if isinstance(t, dict) else None for t in tests]
    if any(state != "Success" for state in states):
        bad = sum(state != "Success" for state in states)
        return summary, f"{bad} test entr(ies) did not succeed"
    if len(states) != passed:
        return summary, f"the report counts {passed} passed but lists {len(states)} tests"
    if passed <= 0:
        return summary, "no test ran (does the filter match any test?)"
    if expect is not None and passed < expect:
        return summary, f"expected at least {expect} tests, {passed} ran"
    if returncode != 0:
        return summary, f"the editor exited with {returncode}"
    return summary, None


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--filter", default=DEFAULT_FILTER,
                        help=f"the automation test filter (default {DEFAULT_FILTER})")
    parser.add_argument("--expect", type=int,
                        help="fail unless at least this many tests passed")
    parser.add_argument("--engine", help=ENGINE_HELP)
    parser.add_argument("--project", default=str(DEFAULT_PROJECT),
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
    return args


def report_folder(given: str | None) -> Path:
    """The report directory, without an old index.json or editor.log (a stale report never
    passes): `given`, made if need be, else a new temporary directory."""
    if given:
        report_dir = Path(given).resolve()
        report_dir.mkdir(parents=True, exist_ok=True)
    else:
        report_dir = Path(tempfile.mkdtemp(prefix="scfg-unreal-tests-"))
    (report_dir / "index.json").unlink(missing_ok=True)
    (report_dir / "editor.log").unlink(missing_ok=True)
    return report_dir


def run_editor(engine: Path, project: Path, test_filter: str, report_dir: Path, timeout: float,
               log: str | None) -> tuple[int | None, str] | str:
    """Runs the editor's tests: (its exit code, its output), the code None when it overran
    `timeout` and was killed; or why it could not be started."""
    cmd = editor_command(engine, project, test_filter, report_dir)
    chosen = os.environ.get("SCFG_VECTORS")
    print("run_unreal_tests: vectors: " + (f"$SCFG_VECTORS={chosen}" if chosen else
                                           "$SCFG_VECTORS unset: found above the plugin's folder"),
          flush=True)
    print("run_unreal_tests: live: " + (
        "SCFG_LIVE=1: Scfg.Live.Https makes two read-only HTTPS calls to public testnet RPCs"
        if os.environ.get("SCFG_LIVE") == "1" else
        "SCFG_LIVE unset: Scfg.Live.* is not registered (no network)"), flush=True)
    print("run_unreal_tests: " + " ".join(cmd), flush=True)
    try:
        return run_unreal_build.run_streamed(cmd, timeout, log, linger=LINGER)
    except OSError as e:
        return f"cannot start {cmd[0]}: {e}"


def judge(returncode: int, output: str, report_dir: Path, project: Path,
          expect: int | None) -> str | None:
    """Reads the editor's report and log, prints the summary (and the failing tests): None when
    the run is green, else why not."""
    report = read_report(report_dir / "index.json")
    summary, why = verdict(returncode, report, expect)
    editor_log = report_dir / "editor.log"
    try:
        full_log = editor_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        full_log = ""
        if why is None:
            why = f"the editor wrote no log ({editor_log} is missing)"
    opened = opened_project_state(full_log + "\n" + output, project)
    if why is None and opened:
        why = (f"{len(opened)} store(s) opened the project's own payment state file (a test "
               f"facade must not): {opened[0].strip()}")
    if isinstance(report, dict) and (read := vectors_read(report)):
        source = "$SCFG_VECTORS" if os.environ.get("SCFG_VECTORS") else "found above the plugin"
        print(f"run_unreal_tests: the tests read the vectors in {read} ({source})", flush=True)
    print(f"run_unreal_tests: {summary} (report: {report_dir})", flush=True)
    if why is not None and isinstance(report, dict):
        failures = failing(report)
        for line in failures[:SHOWN]:
            print(f"  {line}", file=sys.stderr)
        if len(failures) > SHOWN:
            print(f"  ... and {len(failures) - SHOWN} more", file=sys.stderr)
    return why


def main(argv=None) -> int:
    args = parse_args(argv)
    engine, project = engine_root(args.engine), Path(args.project)
    if engine is None:
        print(f"run_unreal_tests: {NO_ENGINE}", file=sys.stderr)
        return 2
    if not args.no_build:
        why = run_unreal_build.build(engine, project, args.target, "Development",
                                     timeout=args.build_timeout)
        if why is not None:
            print(f"run_unreal_tests: FAILED: the build failed: {why}", file=sys.stderr)
            return 1
    report_dir = report_folder(args.report_dir)
    ran = run_editor(engine, project, args.filter, report_dir, args.timeout, args.log)
    if isinstance(ran, str):
        print(f"run_unreal_tests: FAILED: {ran}", file=sys.stderr)
        return 1
    returncode, output = ran
    if returncode is None:
        print(f"run_unreal_tests: FAILED: the editor did not finish within {args.timeout:g} s "
              f"and was killed", file=sys.stderr)
        return 1
    why = judge(returncode, output, report_dir, project, args.expect)
    if why is not None:
        print(f"run_unreal_tests: FAILED: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
