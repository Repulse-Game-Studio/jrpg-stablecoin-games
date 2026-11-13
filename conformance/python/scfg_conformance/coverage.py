"""The coverage manifest (conformance/coverage.json): which vectors twin each Python unit test.

    {"files": ["tests/unit/test_x.py", ...],
     "tests": {"tests/unit/test_x.py::test_y": ["tier1/<topic>" | "tier2/<scenario id>", ...]
                                               | {"python_only": "why no port can share it"}}}

A vector id is the vector's path under conformance/vectors/ without ".json". Every test in
every listed file must be mapped, every mapped test and vector must exist, and each Tier 2
vector's `covers` must agree with the manifest in both directions.
"""

from __future__ import annotations

import ast
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

REQUIRED_FILES: tuple[str, ...] = tuple(f"tests/unit/test_{name}.py" for name in (
    "engine", "engine_solana", "watcher_awaiting", "watcher_confirming", "restore", "reverify",
    "solana_backend", "solana_watch", "solana_build", "solana_rpc", "arc_logs", "backends",
    "rpc_client", "evm_logs",
    "receipts", "sessions", "payments", "transfers", "amounts", "paylinks", "solana_paylinks",
    "solana_tx", "solana_pda", "solana_base58", "keccak", "evm_addresses", "page_messages",
    "config", "config_solana", "custom_chains", "chains",
))

_TEST_FUNCS = (ast.FunctionDef, ast.AsyncFunctionDef)
VECTOR_ID = re.compile(r"^tier[12]/[a-z0-9_/]+$")


def test_ids(path, *, root: Path | None = None) -> list[str]:
    """Pytest ids of the tests in one file, in source order: top-level test_* functions and
    test_* methods of top-level Test* classes. A parametrized test counts once."""
    root = Path(root or ROOT)
    path = Path(path)
    if not path.is_absolute():
        path = root / path
    rel = path.resolve().relative_to(root.resolve()).as_posix()
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    ids = []
    for node in tree.body:
        if isinstance(node, _TEST_FUNCS) and node.name.startswith("test_"):
            ids.append(f"{rel}::{node.name}")
        elif isinstance(node, ast.ClassDef) and node.name.startswith("Test"):
            ids.extend(f"{rel}::{node.name}::{m.name}" for m in node.body
                       if isinstance(m, _TEST_FUNCS) and m.name.startswith("test_"))
    return ids


def unlisted(manifest: dict) -> list[str]:
    """Required files the manifest does not list yet."""
    return [f for f in REQUIRED_FILES if f not in manifest.get("files", [])]


def _tier2_covers(vectors_dir: Path) -> dict[str, list[str]]:
    out = {}
    tier2 = vectors_dir / "tier2"
    for path in sorted(tier2.rglob("*.json")) if tier2.is_dir() else []:
        vid = path.relative_to(vectors_dir).with_suffix("").as_posix()
        out[vid] = json.loads(path.read_text(encoding="utf-8")).get("covers", [])
    return out


def check(manifest: dict, vectors_dir: Path, *, root: Path | None = None) -> list[str]:
    """Problems with the manifest, sorted; [] when it is consistent. `root` is the repository
    root the test paths are relative to (default: this checkout)."""
    root = Path(root or ROOT)
    vectors_dir = Path(vectors_dir)
    tests: dict = manifest.get("tests", {})
    problems = []

    known: dict[str, set[str]] = {}  # file -> its test ids (only files that exist)

    def ids_of(rel: str) -> set[str]:
        if rel not in known:
            path = root / rel
            known[rel] = set(test_ids(path, root=root)) if path.is_file() else set()
        return known[rel]

    for rel in manifest.get("files", []):
        if rel not in REQUIRED_FILES:
            problems.append(f"not a required file: {rel}")
            continue
        if not (root / rel).is_file():
            problems.append(f"missing file: {rel}")
            continue
        problems += [f"unmapped: {t}" for t in sorted(ids_of(rel)) if t not in tests]

    for test, mapping in tests.items():
        if test not in ids_of(test.split("::", 1)[0]):
            problems.append(f"unknown test: {test}")
        if isinstance(mapping, dict):
            reason = mapping.get("python_only")
            if set(mapping) != {"python_only"} or not isinstance(reason, str) or not reason.strip():
                problems.append(f"bad mapping for {test}")
        elif not isinstance(mapping, list):
            problems.append(f"bad mapping for {test}")
        elif not mapping:
            problems.append(f"empty mapping for {test}")
        else:
            for vid in mapping:
                if not (isinstance(vid, str) and VECTOR_ID.match(vid)
                        and (vectors_dir / f"{vid}.json").is_file()):
                    problems.append(f"unknown vector {vid} for {test}")

    covers = _tier2_covers(vectors_dir)
    for vid, tests_covered in covers.items():
        for test in tests_covered:
            mapping = tests.get(test)
            if not isinstance(mapping, list) or vid not in mapping:
                problems.append(f"{vid} covers {test} but the manifest does not map it there")
    for test, mapping in tests.items():
        for vid in mapping if isinstance(mapping, list) else []:
            if vid in covers and test not in covers[vid]:
                problems.append(f"manifest maps {test} to {vid}, whose covers lack it")
    return sorted(problems)
