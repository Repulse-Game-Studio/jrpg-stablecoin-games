import importlib.util
import json
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")


def _load():
    spec = importlib.util.spec_from_file_location("check_text_ids", ROOT / "tools" / "check_text_ids.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def tool():
    return _load()


def test_the_repository_passes(tool):
    assert tool.problems() == []


def test_default_ids_come_from_the_machine_readable_dump(tool):
    texts = tool.default_texts()
    assert texts["cancel"] == "Cancel"
    vectors = json.loads((ROOT / "conformance" / "vectors" / "messages.json").read_text(encoding="utf-8"))["messages"]
    assert set(vectors) <= set(texts)


def test_a_missing_vector_id_is_reported(tool):
    texts = dict(tool.default_texts())
    del texts["tx_reverted"]
    found = tool.problems(texts=texts)
    assert any("tx_reverted" in p for p in found)


def test_an_id_used_by_the_sheet_code_but_not_defined_is_reported(tool, tmp_path):
    (tmp_path / "sheet.ts").write_text(
        'const a = scfg.text("cancel");\nconst b = text("no_such_id_xyz", 1);\nconst c = this.text(variable);\n'
        '// text("in_a_comment")\n',
        encoding="utf-8")
    found = tool.problems(src=tmp_path)
    assert any("no_such_id_xyz" in p for p in found)
    assert not any("in_a_comment" in p or "variable" in p or "cancel" in p for p in found)


def test_a_godot_string_without_an_rm_text_is_reported(tool):
    texts = {k: v for k, v in tool.default_texts().items() if v != "Cancel"}
    found = tool.problems(texts=texts)
    assert any("Cancel" in p for p in found)


def test_ids_are_unique_values_may_repeat(tool):
    assert tool.problems(texts={**tool.default_texts(), "restore_extra": "Cancel"}) == []
