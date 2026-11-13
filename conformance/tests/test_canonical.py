"""Canonical serializer: the rules every vector file obeys."""

from __future__ import annotations

import dataclasses
from decimal import Decimal

import pytest

from scfg_conformance import canonical


def test_serializer_rejects_unsafe_ints():
    with pytest.raises(ValueError, match=r"a\[0\]"):
        canonical.to_jsonable({"a": [2**53]})
    with pytest.raises(ValueError, match=r"a\[0\]"):
        canonical.to_jsonable({"a": [-(2**53)]})
    assert canonical.to_jsonable({"a": [2**53 - 1]}) == {"a": [2**53 - 1]}


def test_serializer_rejects_fractional_floats():
    with pytest.raises(ValueError, match="x"):
        canonical.to_jsonable({"x": 1.5})
    with pytest.raises(ValueError):
        canonical.to_jsonable(float("nan"))
    with pytest.raises(ValueError):
        canonical.to_jsonable(float("inf"))
    out = canonical.to_jsonable({"x": 3.0})
    assert out == {"x": 3} and type(out["x"]) is int
    with pytest.raises(ValueError):
        canonical.to_jsonable(float(2**60))


def test_decimal_to_string():
    assert canonical.to_jsonable(Decimal("4.990000")) == "4.990000"
    # str(Decimal) is what the vectors pin; zero's form depends on the exponent.
    assert canonical.to_jsonable(Decimal(0).scaleb(-6)) == "0.000000"
    assert canonical.to_jsonable(Decimal(0).scaleb(-18)) == "0E-18"


def test_bytes_raise():
    with pytest.raises(TypeError):
        canonical.to_jsonable({"k": b"\x00"})


def test_dict_keys_must_be_str():
    with pytest.raises(TypeError):
        canonical.to_jsonable({1: "a"})


def test_containers_and_dataclasses():
    @dataclasses.dataclass
    class Row:
        n: int
        tags: tuple

    assert canonical.to_jsonable(Row(1, (1, 2))) == {"n": 1, "tags": [1, 2]}
    assert canonical.to_jsonable({"s": {3, 1, 2}}) == {"s": [1, 2, 3]}
    assert canonical.to_jsonable(frozenset({"b", "a"})) == ["a", "b"]
    assert canonical.to_jsonable([None, True, "x"]) == [None, True, "x"]


def test_unsupported_type_raises():
    with pytest.raises(TypeError):
        canonical.to_jsonable({"o": object()})


def test_dumps_format():
    text = canonical.dumps({"b": 1, "a": ["é", {"z": 0, "y": Decimal("1.0")}]})
    assert text == (
        '{\n "a": [\n  "é",\n  {\n   "y": "1.0",\n   "z": 0\n  }\n ],\n "b": 1\n}\n'
    )
    assert text.endswith("}\n") and not text.endswith("\n\n")
