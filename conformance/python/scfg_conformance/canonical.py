"""Canonical JSON for conformance vectors.

Godot and JS read every JSON number as a float, so integers at or beyond 2**53 must never
reach a vector file as numbers; the serializer raises instead of silently losing precision.
"""

from __future__ import annotations

import dataclasses
import json
from decimal import Decimal
from typing import Any

MAX_SAFE = 2**53


def _where(path: str) -> str:
    return path or "<root>"


def _child(path: str, key: str) -> str:
    return f"{path}.{key}" if path else key


def _check_int(n: int, path: str) -> int:
    if abs(n) >= MAX_SAFE:
        raise ValueError(
            f"integer {n} at {_where(path)} is not safe in JSON (|n| >= 2**53); "
            "write it as a string"
        )
    return n


def _walk(obj: Any, path: str) -> Any:
    if obj is None or isinstance(obj, (bool, str)):
        return obj
    if isinstance(obj, int):
        return _check_int(int(obj), path)
    if isinstance(obj, float):
        if obj != obj or obj in (float("inf"), float("-inf")) or not obj.is_integer():
            raise ValueError(f"non-integral float {obj!r} at {_where(path)}")
        return _check_int(int(obj), path)
    if isinstance(obj, Decimal):
        return str(obj)
    if isinstance(obj, (bytes, bytearray, memoryview)):
        raise TypeError(f"bytes at {_where(path)}: hex-encode into a *_hex field")
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return {
            f.name: _walk(getattr(obj, f.name), _child(path, f.name))
            for f in dataclasses.fields(obj)
        }
    if isinstance(obj, dict):
        out = {}
        for key, value in obj.items():
            if not isinstance(key, str):
                raise TypeError(f"non-string dict key {key!r} at {_where(path)}")
            out[key] = _walk(value, _child(path, key))
        return out
    if isinstance(obj, (list, tuple)):
        return [_walk(v, f"{path}[{i}]") for i, v in enumerate(obj)]
    if isinstance(obj, (set, frozenset)):
        items = [_walk(v, f"{path}[]") for v in obj]
        return sorted(items)
    raise TypeError(f"unsupported type {type(obj).__name__} at {_where(path)}")


def to_jsonable(obj: Any) -> Any:
    """Convert obj to plain JSON types, enforcing the vector rules."""
    return _walk(obj, "")


def dumps(obj: Any) -> str:
    """The one on-disk format: sorted keys, indent 1, non-ASCII kept, trailing newline."""
    return json.dumps(to_jsonable(obj), sort_keys=True, indent=1, ensure_ascii=False) + "\n"
