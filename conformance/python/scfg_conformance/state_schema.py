"""Check an Engine snapshot against the frozen state format (conformance/schema/state.json).

The schema is deliberately tiny so every port can check it without a library:

    {"receipt": {field: spec}, "pending": {field: spec}, "pending_optional": {field: spec},
     "match": {field: spec}, "match_optional": {field: spec}}

`spec` is "str" | "int" | "bool" | "number" (int or float) | one of those + "|null" |
"match|null" (null or an object checked against "match"/"match_optional") |
["enum", value, ...]. Booleans are never ints. Fields of "pending" and "match" are required,
"*_optional" fields may be absent (Solana sessions have them, EVM ones don't; EVM matches have
block_hash), and any other field is an error.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

SCHEMA_PATH = Path(__file__).resolve().parents[2] / "schema" / "state.json"
SNAPSHOT_FIELDS = ("pending", "receipts", "owned", "tip_total", "unready")
_TYPES = {
    "str": lambda v: isinstance(v, str),
    "int": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "bool": lambda v: isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
}


def load() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def _value(value: Any, spec, path: str, schema: dict) -> list[str]:
    if isinstance(spec, list):
        return [] if value in spec[1:] else [f"{path}: expected one of {spec[1:]}, got {value!r}"]
    if spec == "match|null":
        if value is None:
            return []
        if not isinstance(value, dict):
            return [f"{path}: expected object or null, got {value!r}"]
        return _record(value, schema["match"], schema["match_optional"], path, schema)
    base, _, null = spec.partition("|")
    if null and value is None:
        return []
    return [] if _TYPES[base](value) else [f"{path}: expected {spec}, got {value!r}"]


def _record(rec: Any, required: dict, optional: dict, path: str, schema: dict) -> list[str]:
    if not isinstance(rec, dict):
        return [f"{path}: expected object"]
    problems = [f"{path}: missing {k!r}" for k in required if k not in rec]
    for key, value in rec.items():
        spec = required.get(key, optional.get(key))
        if spec is None:
            problems.append(f"{path}: unknown {key!r}")
        else:
            problems.extend(_value(value, spec, f"{path}.{key}", schema))
    return problems


def _receipt_key(r: dict) -> str:
    tx = r["tx_hash"].lower() if r["tx_hash"].startswith("0x") else r["tx_hash"]
    return f"{r['chain']}:{tx}:{r['log_index']}"


def check(snapshot: dict, schema: dict | None = None) -> list[str]:
    """Problems with a snapshot ({"pending", "receipts", "owned", "tip_total", "unready"});
    [] when it conforms."""
    schema = schema or load()
    problems = [f"snapshot: missing {k!r}" for k in SNAPSHOT_FIELDS if k not in snapshot]
    problems += [f"snapshot: unknown {k!r}" for k in snapshot if k not in SNAPSHOT_FIELDS]
    for sid, rec in (snapshot.get("pending") or {}).items():
        path = f"pending[{sid}]"
        found = _record(rec, schema["pending"], schema["pending_optional"], path, schema)
        if not found and rec["id"] != sid:
            found.append(f"{path}: id does not match its key")
        problems += found
    for key, r in (snapshot.get("receipts") or {}).items():
        path = f"receipts[{key}]"
        found = _record(r, schema["receipt"], {}, path, schema)
        if not found and _receipt_key(r) != key:
            found.append(f"{path}: key does not match chain:tx_hash:log_index")
        problems += found
    owned = snapshot.get("owned", [])
    if not isinstance(owned, list) or not all(isinstance(i, str) for i in owned):
        problems.append("owned: expected a list of item ids")
    elif owned != sorted(owned):
        problems.append("owned: not sorted")
    if "tip_total" in snapshot and not isinstance(snapshot["tip_total"], str):
        problems.append(f"tip_total: expected str, got {snapshot['tip_total']!r}")
    unready = snapshot.get("unready", [])
    if not isinstance(unready, list):
        problems.append("unready: expected a list")
    else:
        bad = [f"unready[{i}]: expected [chain, token], got {pair!r}"
               for i, pair in enumerate(unready)
               if not (isinstance(pair, list) and len(pair) == 2
                       and all(isinstance(p, str) for p in pair))]
        problems += bad
        if not bad and unready != sorted(unready):
            problems.append("unready: not sorted")
    return problems
