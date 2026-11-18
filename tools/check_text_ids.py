#!/usr/bin/env python3
"""Check the engines' text ids (RPG Maker, Unity, Unreal) against the shared messages and Godot.

THE RPG MAKER BRANCH checks the RPG Maker plugin's player-facing text ids
(rpgmaker/src/mz/texts.ts), and the Unity table against them.

    uv run python tools/check_text_ids.py --check    # exit 1 and list the problems, else 0
    uv run python tools/check_text_ids.py --check --engine unreal   # the Unreal ids only, no Node

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

THE UNREAL BRANCH (`--engine unreal`) reads the table between `SCFG-TEXTS-BEGIN` and
`SCFG-TEXTS-END` in ScfgTexts.cpp (one `{TEXT("id"), TEXT("English")},` per line) and needs no
Node. The ids are RPG Maker's id scheme (texts.ts). Checked:
  - every messages.json id is there with the same English;
  - every restore message of Godot's restore_flow.gd is there as `restore_<code>`, word for word;
  - every id the Unreal code asks for through `ScfgTexts::Get(TEXT("..."))` or
    `ScfgTexts::Format(TEXT("..."), ...)` (the .cpp/.h files of the Scfg module) exists;
  - BY VALUE, every other text (the `dev_` developer lines aside: Godot does not translate those)
    is a string of the Godot template (scfg.pot), so the engines show the same words;
  - when Node is installed, BY ID: every id is an RM id with the same English. Without Node (or
    with one that cannot run texts_dump.ts) this part is skipped, with the reason printed.

Both branches run by default (`--engine all`, Node 22.6+ needed: it strips the .ts file's types,
no node_modules); without Node, or with one too old to run the .ts file, the RPG Maker branch fails
with that reason (one line, no traceback). `--engine rpgmaker` runs the RPG Maker branch alone, and
fails the same way. Each run starts Node once.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
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
UNREAL_MODULE = ROOT / "unreal" / "Plugins" / "Scfg" / "Source" / "Scfg"
UNREAL_TEXTS = UNREAL_MODULE / "Private" / "UI" / "ScfgTexts.cpp"
# the developer lines (test play only): no Godot string to compare with by value
UNREAL_DEV_PREFIX = "dev_"
NO_NODE = "node is not installed"
_CSTR = r'"((?:[^"\\\n]|\\.)*)"'
UNREAL_ENTRY = re.compile(r"^\s*\{\s*TEXT\(" + _CSTR + r"\)\s*,\s*TEXT\(" + _CSTR + r"\)\s*\}\s*,\s*(?://.*)?$")
UNREAL_ENTRY_START = re.compile(r"\{\s*TEXT\(")
UNREAL_CALL = re.compile(r'\bScfgTexts::(?:Get|Format)\(\s*TEXT\("([^"\\\n]+)"\)')
UNREAL_BEGIN = "SCFG-TEXTS-BEGIN"
UNREAL_END = "SCFG-TEXTS-END"
C_ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}


class TextTableError(Exception):
    """ScfgTexts.cpp's table cannot be read completely."""


class NodeMissing(Exception):
    """The RPG Maker texts need Node."""


def node_available() -> bool:
    return shutil.which("node") is not None


def default_texts() -> dict[str, str]:
    """The RM default texts (id -> English), from `node texts_dump.ts`."""
    if not node_available():
        raise NodeMissing(
            f"{NO_NODE}: the RPG Maker texts come from `node {DUMP.relative_to(ROOT)}`")
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
    restore_ids = {RESTORE_PREFIX + code for code, _ in _restore()}
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


def _vectors() -> dict[str, str]:
    return json.loads(MESSAGES_JSON.read_text(encoding="utf-8"))["messages"]


def _restore() -> list:
    if not RESTORE_FLOW.exists():
        return []
    return restore_messages(RESTORE_FLOW.read_text(encoding="utf-8"), RESTORE_FLOW.name)


def problems(texts: dict[str, str] | None = None, src: Path = RM_SRC,
             unity: bool = False) -> list[str]:
    """The RPG Maker branch's problems, for the RM table `texts` (None: the real one, through
    Node). With `unity`, the Unity table is compared with `texts` too (unity_problems): main()
    passes the real table, a test passes its own stand-in without it."""
    texts = default_texts() if texts is None else texts
    found: list[str] = []
    vectors = _vectors()
    for mid, english in sorted(vectors.items()):
        if mid not in texts:
            found.append(f"messages.json id {mid!r} has no RM default text")
        elif texts[mid] != english:
            found.append(f"RM text {mid!r} is {texts[mid]!r}, messages.json says {english!r}")
    for tid, where in sorted(used_ids(src).items()):
        if tid not in texts:
            found.append(f"{where} asks for text id {tid!r}, which has no RM default text")
    by_id = set(vectors.values())
    for code, english in _restore():
        rid = RESTORE_PREFIX + code
        by_id.add(english)
        if rid not in texts:
            found.append(f"Godot restore message {code!r} has no RM text {rid!r}")
        elif _norm(texts[rid]) != _norm(english):
            found.append(f"RM text {rid!r} is {texts[rid]!r}, "
                         f"Godot's restore message {code!r} says {english!r}")
    if unity:
        found.extend(unity_problems(texts, vectors))
    have = {_norm(v) for v in texts.values()}
    if POT.exists():
        for msgid in pot_msgids():
            if msgid not in by_id and _norm(msgid) not in have:
                found.append(f"Godot sheet string {msgid!r} has no RM text "
                             "(compared by value: it has no id)")
    return found


# --- the Unreal branch -------------------------------------------------------------------------


def _c_unescape(s: str) -> str:
    return re.sub(r"\\(.)", lambda e: C_ESCAPES.get(e.group(1), "\\" + e.group(1)), s)


def unreal_texts(path: Path = UNREAL_TEXTS) -> dict[str, str]:
    """Id -> English of ScfgTexts.cpp's table. Raises TextTableError when the markers are missing,
    an entry is not on a line of its own (counted by its `{TEXT(`), or an id repeats, so none is
    dropped silently."""
    lines = path.read_text(encoding="utf-8").splitlines()
    begin = [i for i, line in enumerate(lines) if line.strip() == f"// {UNREAL_BEGIN}"]
    end = [i for i, line in enumerate(lines) if line.strip() == f"// {UNREAL_END}"]
    if len(begin) != 1 or len(end) != 1 or end[0] < begin[0]:
        raise TextTableError(f"{path.name}: needs one `// {UNREAL_BEGIN}` line and, after it, one "
                             f"`// {UNREAL_END}` line around the table")
    out: dict[str, str] = {}
    starts = 0
    for line in lines[begin[0] + 1:end[0]]:
        code = line.split("//", 1)[0] if line.lstrip().startswith("//") else line
        starts += len(UNREAL_ENTRY_START.findall(code))
        m = UNREAL_ENTRY.match(line)
        if not m:
            continue
        tid, english = _c_unescape(m.group(1)), _c_unescape(m.group(2))
        if tid in out:
            raise TextTableError(f"{path.name}: the text id {tid!r} is defined twice")
        out[tid] = english
    if starts != len(out):
        raise TextTableError(f"{path.name}: read {len(out)} of the {starts} table entries; write "
                             'each as `{TEXT("id"), TEXT("English")},` on one line (one literal '
                             "each)")
    return out


def unreal_used_ids(src: Path = UNREAL_MODULE) -> dict[str, str]:
    """Id -> a file that asks for it, from the .cpp/.h sources under `src`."""
    found: dict[str, str] = {}
    for path in sorted([*src.rglob("*.cpp"), *src.rglob("*.h")]):
        code = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", path.read_text(encoding="utf-8")))
        for m in UNREAL_CALL.finditer(code):
            found.setdefault(m.group(1), path.name)
    return found


def unreal_problems(rm_texts: dict[str, str] | None, unreal: dict[str, str] | None = None,
                    src: Path = UNREAL_MODULE) -> list[str]:
    """The Unreal branch (see the module docstring). `rm_texts` None: the by-id comparison with RPG
    Maker is skipped (no Node)."""
    try:
        unreal = unreal_texts() if unreal is None else unreal
    except TextTableError as e:
        return [str(e)]
    found: list[str] = []
    vectors = _vectors()
    for mid, english in sorted(vectors.items()):
        if mid not in unreal:
            found.append(f"messages.json id {mid!r} has no Unreal text")
        elif unreal[mid] != english:
            found.append(f"Unreal text {mid!r} is {unreal[mid]!r}, messages.json says {english!r}")
    keyed = set(vectors)
    for code, english in _restore():
        rid = RESTORE_PREFIX + code
        keyed.add(rid)
        if rid not in unreal:
            found.append(f"Godot restore message {code!r} has no Unreal text {rid!r}")
        elif _norm(unreal[rid]) != _norm(english):
            found.append(f"Unreal text {rid!r} is {unreal[rid]!r}, "
                         f"Godot's restore message {code!r} says {english!r}")
    for tid, where in sorted(unreal_used_ids(src).items()):
        if tid not in unreal:
            found.append(f"{where} asks for text id {tid!r}, which has no Unreal text")
    pot = {_norm(m) for m in pot_msgids()} if POT.exists() else set()
    for tid, english in sorted(unreal.items()):
        if tid in keyed or tid.startswith(UNREAL_DEV_PREFIX):
            continue
        if _norm(english) not in pot:
            found.append(f"Unreal text {tid!r} ({english!r}) is not a Godot string "
                         "(compared by value with scfg.pot)")
    if rm_texts is not None:
        for tid, english in sorted(unreal.items()):
            if tid not in rm_texts:
                found.append(f"Unreal text id {tid!r} is not an RM text id")
            elif _norm(rm_texts[tid]) != _norm(english):
                found.append(f"Unreal text {tid!r} is {english!r}, RM's says {rm_texts[tid]!r}")
    return found


def _why(e: Exception) -> str:
    """Why the RPG Maker texts could not be read, in one line."""
    if isinstance(e, subprocess.CalledProcessError):
        detail = (e.stderr or "").strip().splitlines()
        tail = f": {detail[-1]}" if detail else ""
        return (f"`node {DUMP.relative_to(ROOT)}` exited with {e.returncode} "
                f"(Node 22.6+ runs a .ts file){tail}")
    if isinstance(e, ValueError):
        return f"`node {DUMP.relative_to(ROOT)}` printed no JSON ({e})"
    return str(e)


RM_UNREADABLE = (NodeMissing, subprocess.CalledProcessError, OSError, ValueError)


# What a run without problems prints, per --engine.
CONSISTENT = {
    "all": "RPG Maker text ids are consistent with the shared messages, Godot, Unity and Unreal",
    "rpgmaker": "RPG Maker text ids are consistent with the shared messages, Godot and Unity",
    "unreal": "Unreal text ids are consistent",
}


def main(argv: list[str] | None = None) -> int:
    """Reads the RM table once (Node), then runs the RPG Maker branch on it (`all`, `rpgmaker`)
    and the Unreal branch (`all`, `unreal`), with the RM texts when Node gives them."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="exit 1 on problems (the default)")
    parser.add_argument("--engine", choices=["all", "rpgmaker", "unreal"], default="all",
                        help="which branch to run (default: both; see the module docstring)")
    engine = parser.parse_args(argv).engine
    found: list[str] = []
    rm_texts: dict[str, str] | None = None
    try:
        rm_texts = default_texts()
    except RM_UNREADABLE as e:
        reason = _why(e)
        if engine == "unreal":
            print(f"Unreal ids not compared with RPG Maker's (skipped: {reason})")
        else:
            found.append(f"the RPG Maker branch cannot run: {reason}")
    if rm_texts is not None and engine != "unreal":
        found.extend(problems(rm_texts, unity=True))
    if engine != "rpgmaker":
        found.extend(unreal_problems(rm_texts))
    for line in found:
        print(line, file=sys.stderr)
    if found:
        return 1
    print(CONSISTENT[engine])
    return 0


if __name__ == "__main__":
    sys.exit(main())
