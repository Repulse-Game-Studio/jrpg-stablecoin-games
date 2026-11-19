"""The slow suites stay deselected under any `-m` that does not name their marker: a `-m` on the
command line replaces pyproject's addopts, and `-m "not integration"` must not build or start
Unreal, nor build the C++ server driver (the guards in tests/unreal_e2e, tests/unreal_showcase and
tests/server_security's conftests)."""

import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CPP_ID = re.compile(r"\[cpp[\]-]")  # a test id whose server parameter is the C++ one


def collected(*args: str) -> list[str]:
    """The test ids `pytest --collect-only -q <args>` selects."""
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "--collect-only", "-q", "-p", "no:cacheprovider", *args],
        cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    assert done.returncode in (0, 5), done.stdout + done.stderr  # 5: nothing selected
    return [line for line in done.stdout.splitlines() if "::" in line]


@pytest.mark.parametrize("directory", ["tests/unreal_e2e", "tests/unreal_showcase"])
def test_a_negative_marker_expression_selects_no_unreal_test(directory):
    assert collected("-m", "not integration", directory) == []


@pytest.mark.parametrize("directory", ["tests/unreal_e2e", "tests/unreal_showcase"])
def test_naming_the_marker_selects_the_unreal_tests(directory):
    assert collected("-m", Path(directory).name, directory)


def test_a_negative_marker_expression_selects_no_cpp_server_test():
    ids = collected("-m", "not node_server", "tests/server_security")
    assert ids and not [x for x in ids if CPP_ID.search(x)]


def test_naming_cpp_server_selects_only_its_tests():
    ids = collected("-m", "cpp_server", "tests/server_security")
    assert ids and all(CPP_ID.search(x) for x in ids)
