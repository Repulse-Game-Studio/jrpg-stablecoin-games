#!/usr/bin/env python3
"""Check the RPG Maker plugin's player-facing text ids (rpgmaker/src/mz/texts.ts).

    uv run python tools/check_text_ids.py --check    # exit 1 and list the problems, else 0

The RM default texts are read as JSON from `node rpgmaker/src/mz/texts_dump.ts` (the module's own
DEFAULT_TEXTS), never parsed out of TypeScript. Checked:
  - the RM ids include every id of conformance/vectors/messages.json (with the same English);
  - every id the sheet code passes to `text("...")` / `.text("...")` / `_notify("...")` (the
    non-test sources in rpgmaker/src/mz) exists;
  - Godot, BY ID where Godot has ids: the checkout's messages are the messages.json ids above
    (the Godot core is checked against those vectors), and each entry of the restore screen's
    MESSAGES (godot/addons/scfg/core/restore_flow.gd) is the RM text `restore_<id>`, word for
    word;
  - Godot, BY VALUE ONLY for the sheet strings, which have no ids there (a `tr()` key is its own
    English): every other string of the Godot template (godot/addons/scfg/translations/scfg.pot)
    is the English of some RM text, under any id, so the engines show the same words.
  - Unity, BY ID: the table in unity/Packages/com.repulse.scfg/Runtime/Unity/UI/ScfgUiTexts.cs
    (its own entries; the checkout and restore messages it adds come from the C# core, whose
    tests compare them with messages.json and restore_flow.py) holds exactly the RM ids that are
    not checkout or restore messages, each with the RM English word for word, and every id the
    Unity code passes to `ScfgUiTexts.Get("...")` / `ScfgUiTexts.Format("...")` (string
    literals in the call, comparisons such as `== "solana"` aside) exists.
`%d` and `%s` count as the same.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from make_pot import restore_messages  # noqa: E402  (the reader the .pot itself is made with)

RM_SRC = ROOT / "rpgmaker" / "src" / "mz"
DUMP = RM_SRC / "texts_dump.ts"
MESSAGES_JSON = ROOT / "conformance" / "vectors" / "messages.json"
POT = ROOT / "godot" / "addons" / "scfg" / "translations" / "scfg.pot"
RESTORE_FLOW = ROOT / "godot" / "addons" / "scfg" / "core" / "restore_flow.gd"
# the RM id of the Godot restore screen's message <code> (texts.ts's ID SCHEME)
RESTORE_PREFIX = "restore_"
UNITY_SRC = ROOT / "unity" / "Packages" / "com.repulse.scfg" / "Runtime" / "Unity"
UNITY_TEXTS = UNITY_SRC / "UI" / "ScfgUiTexts.cs"
UNITY_ENTRY = re.compile(r'^\s*\{ "([a-z][a-z0-9_]*)", "((?:[^"\\]|\\.)*)" \},\s*$', re.M)
UNITY_CALL = re.compile(r"ScfgUiTexts\.(?:Get|Format)\(")
UNITY_LITERAL = re.compile(r'"([^"\\\n]*)"')
UNITY_COMPARISON = re.compile(r'[!=]=\s*"[^"\\\n]*"')
TEXT_ID = re.compile(r"^[a-z][a-z0-9_]*$")

CALL = re.compile(r'(?<![\w$])(?:text|_notify)\(\s*"([^"\\\n]+)"')
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"^\s*//.*$", re.M)
PO_LINE = re.compile(r'^(msgid|msgstr)?\s*"((?:[^"\\]|\\.)*)"\s*$')
PO_ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}


def default_texts() -> dict[str, str]:
    out = subprocess.run(
        ["node", str(DUMP)], cwd=DUMP.parent, check=True,
        capture_output=True, text=True, encoding="utf-8")
    return json.loads(out.stdout)


def used_ids(src: Path = RM_SRC) -> dict[str, str]:
    """Id -> a file that asks for it, from the non-test .ts sources under `src`."""
    found: dict[str, str] = {}
    for path in sorted(src.rglob("*.ts")):
        if path.name.endswith(".test.ts"):
            continue
        code = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", path.read_text(encoding="utf-8")))
        for m in CALL.finditer(code):
            found.setdefault(m.group(1), path.name)
    return found


def pot_msgids(pot: Path = POT) -> list[str]:
    ids: list[str] = []
    current: list[str] | None = None
    for line in pot.read_text(encoding="utf-8").splitlines():
        m = PO_LINE.match(line)
        if m and m.group(1) == "msgid":
            current = [m.group(2)]
        elif m and m.group(1) == "msgstr":
            if current is not None:
                raw = "".join(current)
                ids.append(re.sub(r"\\(.)", lambda e: PO_ESCAPES.get(e.group(1), e.group(1)), raw))
            current = None
        elif m and current is not None:
            current.append(m.group(2))
    return [i for i in ids if i]


def unity_texts(path: Path = UNITY_TEXTS) -> dict[str, str]:
    """The Unity table's own entries (id -> English), in C# string syntax decoded as JSON."""
    out: dict[str, str] = {}
    for m in UNITY_ENTRY.finditer(path.read_text(encoding="utf-8")):
        out[m.group(1)] = json.loads(f'"{m.group(2)}"')
    return out


def _call_args(code: str, start: int) -> str:
    """The text of a call's arguments, from just after its "(" to the matching ")"."""
    depth, i, in_str = 1, start, False
    while i < len(code) and depth:
        c = code[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return code[start:i - 1]


def unity_used_ids(src: Path = UNITY_SRC) -> dict[str, str]:
    """Id -> a file that asks for it, from the .cs files under `src`."""
    found: dict[str, str] = {}
    for path in sorted(src.rglob("*.cs")):
        code = path.read_text(encoding="utf-8")
        code = re.sub(r"^\s*//.*$", "", code, flags=re.M)
        for m in UNITY_CALL.finditer(code):
            args = UNITY_COMPARISON.sub("", _call_args(code, m.end()))
            for lit in UNITY_LITERAL.finditer(args):
                if TEXT_ID.match(lit.group(1)):
                    found.setdefault(lit.group(1), path.name)
    return found


def unity_problems(texts: dict[str, str], vectors: dict[str, str],
                   table: Path = UNITY_TEXTS, src: Path = UNITY_SRC) -> list[str]:
    if not table.exists():
        return [f"missing {table}"]
    own = unity_texts(table)
    restore_ids = {RESTORE_PREFIX + code for code, _ in
                   restore_messages(RESTORE_FLOW.read_text(encoding="utf-8"), RESTORE_FLOW.name)}
    found: list[str] = []
    for tid, english in sorted(own.items()):
        if tid in vectors or tid in restore_ids:
            found.append(f"Unity text {tid!r} is a message id: it comes from the C# core, "
                         "not the table")
        elif tid not in texts:
            found.append(f"Unity text {tid!r} has no RM text")
        elif texts[tid] != english:
            found.append(f"Unity text {tid!r} is {english!r}, RM says {texts[tid]!r}")
    for tid in sorted(texts):
        if tid not in vectors and tid not in restore_ids and tid not in own:
            found.append(f"RM text {tid!r} has no Unity text")
    known = set(own) | set(vectors) | restore_ids
    for tid, where in sorted(unity_used_ids(src).items()):
        if tid not in known:
            found.append(f"{where} asks for Unity text id {tid!r}, "
                         "which the table does not define")
    return found


def _norm(s: str) -> str:
    return s.replace("%d", "%s")


def problems(texts: dict[str, str] | None = None, src: Path = RM_SRC) -> list[str]:
    """Every problem. Given `texts` (a test's stand-in for the RM table), the Unity table is not
    compared: it is checked against the real RM table only (unity_problems)."""
    real = texts is None
    texts = default_texts() if texts is None else texts
    found: list[str] = []
    vectors = json.loads(MESSAGES_JSON.read_text(encoding="utf-8"))["messages"]
    for mid, english in sorted(vectors.items()):
        if mid not in texts:
            found.append(f"messages.json id {mid!r} has no RM default text")
        elif texts[mid] != english:
            found.append(f"RM text {mid!r} is {texts[mid]!r}, messages.json says {english!r}")
    for tid, where in sorted(used_ids(src).items()):
        if tid not in texts:
            found.append(f"{where} asks for text id {tid!r}, which has no RM default text")
    by_id = set(vectors.values())
    if RESTORE_FLOW.exists():
        restore = restore_messages(RESTORE_FLOW.read_text(encoding="utf-8"), RESTORE_FLOW.name)
        for code, english in restore:
            rid = RESTORE_PREFIX + code
            by_id.add(english)
            if rid not in texts:
                found.append(f"Godot restore message {code!r} has no RM text {rid!r}")
            elif _norm(texts[rid]) != _norm(english):
                found.append(f"RM text {rid!r} is {texts[rid]!r}, "
                             f"Godot's restore message {code!r} says {english!r}")
    if real:
        found.extend(unity_problems(texts, vectors))
    have = {_norm(v) for v in texts.values()}
    if POT.exists():
        for msgid in pot_msgids():
            if msgid not in by_id and _norm(msgid) not in have:
                found.append(f"Godot sheet string {msgid!r} has no RM text "
                             "(compared by value: it has no id)")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="exit 1 on problems (the default)")
    parser.parse_args(argv)
    found = problems()
    for line in found:
        print(line, file=sys.stderr)
    if found:
        return 1
    print("RM and Unity text ids are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
