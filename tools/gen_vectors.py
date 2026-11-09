#!/usr/bin/env python3
"""Generate (or --check) the conformance vectors from the Python reference core."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "plugin" / "game" / "python-packages"))
sys.path.insert(0, str(ROOT / "conformance" / "python"))

from scfg_conformance import vectors  # noqa: E402


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "conformance" / "vectors"),
                        help="output directory (default: conformance/vectors)")
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed vectors are stale, missing or extra")
    args = parser.parse_args(argv)
    vectors.load_all()
    out = Path(args.out)
    if args.check:
        problems = vectors.check(out)
        for problem in problems:
            print(problem)
        if problems:
            print("conformance vectors out of date: run python tools/gen_vectors.py "
                  "(it does not remove extra files: delete those by hand)", file=sys.stderr)
            return 1
        return 0
    for path in vectors.write_all(out):
        print(f"wrote {path.relative_to(out).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
