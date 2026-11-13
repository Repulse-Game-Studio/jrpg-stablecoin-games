"""Python's int() for text the core reads from storage, a page or an RPC, with CPython's limit on
the number of decimal digits enforced explicitly (independent of the interpreter version: the
limit arrived in CPython 3.11 and in the 3.10.7 / 3.9.14 security releases, and older embedded
interpreters, such as some Ren'Py builds, lack it). Converting a very long digit string costs
quadratic time; the TypeScript and GDScript cores apply the same limit (pystr.ts, pystr.gd).
Also: json_loads (an RPC answer's integer literals through int_from) and digit_run_value (a
number in a node's error message, at most 18 significant digits), Ruling 6."""

from __future__ import annotations

import json

# CPython's default sys.get_int_max_str_digits().
INT_MAX_STR_DIGITS = 4300
# A digit run read out of a node's error message (an EVM range limit, a Solana transaction
# version) is a number only up to this many significant digits (Ruling 6): every range or version
# a request can carry is below 10**18, and the TypeScript and GDScript cores read the same.
DIGIT_RUN_LIMIT = 18


def int_from(value) -> int:
    """int(value), refusing a str with more than INT_MAX_STR_DIGITS decimal digits as CPython
    does (ValueError, with CPython's message) before any conversion."""
    if isinstance(value, str) and len(value) > INT_MAX_STR_DIGITS:
        digits = sum(1 for ch in value if ch.isdecimal())
        if digits > INT_MAX_STR_DIGITS:
            raise ValueError(f"Exceeds the limit ({INT_MAX_STR_DIGITS} digits) for integer string "
                             f"conversion: value has {digits} digits; use "
                             "sys.set_int_max_str_digits() to increase the limit")
    return int(value)


def digit_run_value(run: str) -> int | None:
    """int(run) for a run of Unicode decimal digits (what re's \\d+ matches), or None when it has
    more than DIGIT_RUN_LIMIT significant digits. Linear: leading zeros (of any script) are
    stripped first, so no long text is converted."""
    if len(run) > DIGIT_RUN_LIMIT:
        # every decimal digit is its script's zero plus its value (Nd digits come in runs of ten)
        run = run.lstrip("".join({chr(ord(c) - int(c)) for c in set(run)}))
        if len(run) > DIGIT_RUN_LIMIT:
            return None
    return int(run) if run else 0


def json_loads(text):
    """json.loads(text), its integer literals read with int_from: one of more than
    INT_MAX_STR_DIGITS digits is a ValueError on every interpreter (CPython 3.9.25 and 3.11+
    refuse it themselves; an interpreter without the limit would convert it in quadratic
    time). Fractions and exponents are floats, as json makes them."""
    return json.loads(text, parse_int=int_from)
