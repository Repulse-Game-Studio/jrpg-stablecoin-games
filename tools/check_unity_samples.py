"""Check the Unity package's samples (unity/Packages/com.repulse.scfg/Samples~).

Without --unity: package.json lists exactly the folders in Samples~, and every file and folder in
them has its .meta (the Package Manager copies the .meta files on import, so a scene keeps its
script references). With --unity: the samples are also copied into the dev project
(unity/Project/Assets/Samples/, git-ignored), as the Package Manager's Import button does; the
editor compiles them, opens each sample's scene (Scfg.Maintenance.ScfgSamplesCheck.Check: no
missing script, the sample's component there) and runs each sample's Configure with SCFG Settings
assets it must accept. The copy is removed afterwards.

    uv run python tools/check_unity_samples.py [--unity ~/Unity/Hub/Editor/<version>/Editor/Unity]

--make-scenes (maintainers, with --unity) writes each sample's scene in the dev project with
Scfg.Maintenance.ScfgSamplesCheck.MakeScenes and copies it, and the .meta files Unity made for
the sample's files, back into Samples~.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "unity" / "Packages" / "com.repulse.scfg"
SAMPLES = PACKAGE / "Samples~"
PROJECT = ROOT / "unity" / "Project"
COPY = PROJECT / "Assets" / "Samples"
TIMEOUT = 1800


def listed(package_json: Path = PACKAGE / "package.json") -> list[dict]:
    return json.loads(package_json.read_text(encoding="utf-8")).get("samples") or []


def problems(samples: Path = SAMPLES, package_json: Path = PACKAGE / "package.json") -> list[str]:
    out: list[str] = []
    entries = listed(package_json)
    paths = [e.get("path") for e in entries]
    folders = sorted(p.name for p in samples.iterdir() if p.is_dir()) if samples.is_dir() else []
    for e in entries:
        for key in ("displayName", "description", "path"):
            if not e.get(key):
                out.append(f"package.json sample without {key}: {e}")
    for folder in folders:
        if f"Samples~/{folder}" not in paths:
            out.append(f"Samples~/{folder} is not listed in package.json's samples")
    for p in paths:
        if p and not (samples.parent / p).is_dir():
            out.append(f"package.json lists {p}, which does not exist")
    for folder in folders:
        for path in sorted((samples / folder).rglob("*")):
            if path.name.startswith("."):
                continue
            rel = path.relative_to(samples.parent)
            if path.name.endswith(".meta"):
                if not path.with_name(path.name[: -len(".meta")]).exists():
                    out.append(f"orphan .meta: {rel}")
            elif not path.with_name(path.name + ".meta").exists():
                out.append(f"missing .meta: {rel}")
    return out


def run_editor(unity: str, method: str, log: Path) -> int:
    cmd = [unity, "-batchmode", "-nographics", "-quit", "-projectPath", str(PROJECT),
           "-executeMethod", f"Scfg.Maintenance.ScfgSamplesCheck.{method}", "-logFile", str(log)]
    return subprocess.run(cmd, timeout=TIMEOUT).returncode


def with_copy(unity: str, method: str, make: bool) -> int:
    if COPY.exists():
        print(f"{COPY} already exists: remove it first (it is the check's own copy)")
        return 2
    work = Path(tempfile.mkdtemp(prefix="scfg-samples-"))
    log = work / "editor.log"
    try:
        shutil.copytree(SAMPLES, COPY)
        code = run_editor(unity, method, log)
        text = log.read_text(errors="replace") if log.exists() else ""
        lines = [x for x in text.splitlines() if "scfg-samples:" in x or "error CS" in x]
        print("\n".join(lines) or text[-4000:])
        if code == 0 and make:
            for path in COPY.rglob("*"):
                if path.parent == COPY:
                    continue  # the sample folders' own .meta: the Package Manager makes its own
                if path.suffix == ".meta" or path.suffix == ".unity":
                    dest = SAMPLES / path.relative_to(COPY)
                    if not dest.exists() or path.suffix == ".unity":
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, dest)
        return code
    finally:
        shutil.rmtree(COPY, ignore_errors=True)
        (PROJECT / "Assets" / "Samples.meta").unlink(missing_ok=True)
        shutil.rmtree(work, ignore_errors=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--unity", help="the Unity editor binary: also compile and open the samples")
    ap.add_argument("--make-scenes", action="store_true",
                    help="maintainers: write the samples' scenes and .meta files (needs --unity)")
    args = ap.parse_args(argv)
    if args.make_scenes:
        if not args.unity:
            print("--make-scenes needs --unity")
            return 2
        return with_copy(args.unity, "MakeScenes", True)
    found = problems()
    for p in found:
        print(p)
    if found:
        return 1
    if args.unity:
        code = with_copy(args.unity, "Check", False)
        print("samples " + ("OK" if code == 0 else f"FAILED (editor exit {code})"))
        return 1 if code else 0
    print(f"samples OK ({len(listed())} listed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
