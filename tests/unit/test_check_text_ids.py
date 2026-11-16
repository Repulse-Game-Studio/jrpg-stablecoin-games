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


def test_godot_messages_are_compared_by_id(tool):
    # the restore screen's messages have ids in Godot (restore_flow.gd's MESSAGES): two RM texts
    # swapped keep every English string somewhere, but each id now says the other's words
    texts = dict(tool.default_texts())
    texts["restore_tx_reverted"], texts["restore_tx_not_found"] = texts["restore_tx_not_found"], texts["restore_tx_reverted"]
    found = tool.problems(texts=texts)
    assert any("restore_tx_reverted" in p for p in found), found
    assert any("restore_tx_not_found" in p for p in found), found


def test_godot_sheet_strings_are_compared_by_value_only(tool, tmp_path):
    # a sheet string has no id in Godot (its English is the tr() key): any RM text with that
    # English will do, under whichever id (tmp_path: no sheet code asking for "cancel" by id)
    texts = dict(tool.default_texts())
    texts["extra_cancel_copy"] = texts.pop("cancel")
    assert tool.problems(texts=texts, src=tmp_path) == []


def test_the_unity_table_matches_rm_by_id(tool, tmp_path):
    table = tmp_path / "ScfgUiTexts.cs"
    original = tool.UNITY_TEXTS.read_text(encoding="utf-8")
    table.write_text(original.replace('{ "cancel", "Cancel" },', '{ "cancel", "Abort" },'), encoding="utf-8")
    texts = tool.default_texts()
    vectors = json.loads((ROOT / "conformance" / "vectors" / "messages.json").read_text(encoding="utf-8"))["messages"]
    found = tool.unity_problems(texts, vectors, table=table)
    assert any("'cancel'" in p and "Abort" in p for p in found), found
    table.write_text(original.replace('                { "close", "Close" },\n', ""), encoding="utf-8")
    found = tool.unity_problems(texts, vectors, table=table)
    assert any("'close' has no Unity text" in p for p in found), found


def test_an_id_the_unity_code_asks_for_must_exist(tool, tmp_path):
    (tmp_path / "View.cs").write_text(
        'var a = ScfgUiTexts.Get(x == "solana" ? "copy_hash" : "no_such_unity_id");\n'
        'var b = ScfgUiTexts.Format("which_network", token);\n'
        '// ScfgUiTexts.Get("in_a_comment")\n',
        encoding="utf-8")
    texts = tool.default_texts()
    vectors = json.loads((ROOT / "conformance" / "vectors" / "messages.json").read_text(encoding="utf-8"))["messages"]
    found = tool.unity_problems(texts, vectors, src=tmp_path)
    assert [p for p in found if "asks for" in p] == ["View.cs asks for Unity text id 'no_such_unity_id', which the table does not define"]
    assert tool.unity_used_ids(tmp_path).keys() == {"copy_hash", "no_such_unity_id", "which_network"}
