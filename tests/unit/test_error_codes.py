import ast
import re
from pathlib import Path

import pytest

from scfg_core.config import build_config
from scfg_core.errors import ConfigError

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "plugin" / "game" / "python-packages" / "scfg_core"
API = ROOT / "plugin" / "game" / "scfg" / "scfg_api.rpy"
CODE_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")
VALID = "0x52908400098527886E0F7030069857D2E4169EE7"


def _is_config_error_raise(node):
    if not isinstance(node, ast.Raise) or not isinstance(node.exc, ast.Call):
        return False
    func = node.exc.func
    return (isinstance(func, ast.Name) and func.id == "ConfigError") or (
        isinstance(func, ast.Attribute) and func.attr == "ConfigError")


def test_every_config_error_raise_has_a_code():
    found = 0
    for path in sorted(CORE.rglob("*.py")):
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if not _is_config_error_raise(node):
                continue
            found += 1
            where = f"{path.name}:{node.lineno}"
            kws = {k.arg: k.value for k in node.exc.keywords}
            assert "code" in kws, f"{where}: raise ConfigError without code="
            value = kws["code"]
            assert isinstance(value, ast.Constant) and isinstance(value.value, str), where
            assert CODE_RE.match(value.value), f"{where}: bad code {value.value!r}"
    assert found >= 57

    source = API.read_text(encoding="utf-8")
    raises = re.findall(r"raise ConfigError\((.*?)\)\n", source, re.S)
    assert len(raises) == 4
    for call in raises:
        m = re.search(r'code="([^"]+)"$', call)
        assert m and CODE_RE.match(m.group(1)), call


def test_config_error_keeps_message_and_code():
    e = ConfigError("x", code="a.b")
    assert str(e) == "x"
    assert e.code == "a.b"
    assert isinstance(e, ValueError)


def test_config_error_requires_code():
    with pytest.raises(TypeError):
        ConfigError("x")


@pytest.mark.parametrize("kwargs, code", [
    ({}, "wallet.missing"),
    ({"wallet": VALID, "chains": ()}, "chains.empty"),
    ({"wallet": VALID, "chains": ("nope",)}, "chains.unknown"),
])
def test_known_codes(kwargs, code):
    with pytest.raises(ConfigError) as exc:
        build_config(**kwargs)
    assert exc.value.code == code
