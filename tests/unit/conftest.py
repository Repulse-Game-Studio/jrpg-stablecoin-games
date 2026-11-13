"""Fixtures shared by the unit tests."""
import sys

import pytest


@pytest.fixture(params=["interpreter limit", "no interpreter limit"])
def interpreter(request):
    """Run each test as is and with the interpreter's own int digit limit switched off (0 =
    unlimited), which is what an interpreter without the limit does (the core must
    behave alike on both)."""
    if request.param == "no interpreter limit":
        if not hasattr(sys, "set_int_max_str_digits"):
            pytest.skip("this interpreter has no int digit limit to switch off")
        before = sys.get_int_max_str_digits()
        sys.set_int_max_str_digits(0)
        yield request.param
        sys.set_int_max_str_digits(before)
    else:
        yield request.param


@pytest.fixture
def int_texts(monkeypatch):
    """spy(*modules) makes the modules' int() record the length of every str it converts and
    returns that list: a structural check that no long digit text is converted (the cost is
    quadratic), independent of the machine's speed."""
    lengths: list[int] = []

    def spy(x=0, *args):
        if isinstance(x, str):
            lengths.append(len(x))
        return int(x, *args)

    def install(*modules):
        for module in modules:
            monkeypatch.setattr(module, "int", spy, raising=False)
        return lengths
    return install
