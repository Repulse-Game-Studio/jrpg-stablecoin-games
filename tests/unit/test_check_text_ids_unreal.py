"""The Unreal branch of tools/check_text_ids.py: the Unreal screens' text table
(unreal/Plugins/Scfg/Source/Scfg/Private/UI/ScfgTexts.cpp) against messages.json, Godot's restore
messages and template, and RPG Maker's ids. None of this needs Node (test_check_text_ids.py covers
the RPG Maker branch, which does)."""

import importlib.util
import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("check_text_ids", ROOT / "tools" / "check_text_ids.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def tool():
    return _load()


def _table(entries, before="", after=""):
    lines = ["// SCFG-TEXTS-BEGIN", *entries, "// SCFG-TEXTS-END"]
    return before + "\n".join(lines) + "\n" + after


def test_the_repository_passes_without_rpg_maker(tool):
    assert tool.unreal_problems(None) == []


def test_the_table_is_read_with_its_escapes(tool, tmp_path):
    path = tmp_path / "ScfgTexts.cpp"
    path.write_text(_table([
        '\t{TEXT("cancel"), TEXT("Cancel")},',
        '\t// a comment line',
        '\t{TEXT("quote"), TEXT("say \\"hi\\" \\\\ there")},  // a trailing comment',
    ], before='static const FEntry GEntries[] = {\n', after='};\n{TEXT("outside"), TEXT("ignored")},\n'),
        encoding="utf-8")
    assert tool.unreal_texts(path) == {"cancel": "Cancel", "quote": 'say "hi" \\ there'}


def test_the_repository_table_has_the_shared_messages(tool):
    texts = tool.unreal_texts()
    assert texts["cancel"] == "Cancel"
    assert texts["restore_tx_not_found"] == "Transaction not found. Check the hash, or try again later."
    assert set(tool._vectors()) <= set(texts)


@pytest.mark.parametrize("entries", [
    ['\t{TEXT("a"), TEXT("A")}, {TEXT("b"), TEXT("B")},'],         # two on one line
    ['\t{TEXT("a"),', '\t TEXT("A")},'],                            # one over two lines
    ['\t{TEXT("a"), TEXT("A" "B")},'],                              # concatenated literals
])
def test_an_entry_the_reader_cannot_take_whole_is_an_error(tool, tmp_path, entries):
    path = tmp_path / "ScfgTexts.cpp"
    path.write_text(_table(entries), encoding="utf-8")
    with pytest.raises(tool.TextTableError, match="table entries"):
        tool.unreal_texts(path)


def test_a_repeated_id_or_missing_markers_are_errors(tool, tmp_path):
    path = tmp_path / "ScfgTexts.cpp"
    path.write_text(_table(['\t{TEXT("a"), TEXT("A")},', '\t{TEXT("a"), TEXT("B")},']), encoding="utf-8")
    with pytest.raises(tool.TextTableError, match="defined twice"):
        tool.unreal_texts(path)
    path.write_text('\t{TEXT("a"), TEXT("A")},\n', encoding="utf-8")
    with pytest.raises(tool.TextTableError, match="SCFG-TEXTS-BEGIN"):
        tool.unreal_texts(path)


def test_a_missing_or_changed_vector_message_is_reported(tool):
    texts = dict(tool.unreal_texts())
    del texts["tx_reverted"]
    texts["invalid_hash"] = "Not a hash."
    found = tool.unreal_problems(None, unreal=texts)
    assert any("tx_reverted" in p and "no Unreal text" in p for p in found), found
    assert any("invalid_hash" in p and "messages.json says" in p for p in found), found


def test_restore_messages_are_compared_by_id(tool):
    texts = dict(tool.unreal_texts())
    texts["restore_tx_reverted"], texts["restore_tx_not_found"] = texts["restore_tx_not_found"], texts["restore_tx_reverted"]
    found = tool.unreal_problems(None, unreal=texts)
    assert any("restore_tx_reverted" in p for p in found), found
    assert any("restore_tx_not_found" in p for p in found), found


def test_an_id_the_code_asks_for_but_not_defined_is_reported(tool, tmp_path):
    (tmp_path / "Sheet.cpp").write_text(
        'Label(ScfgTexts::Get(TEXT("cancel")));\n'
        'Label(ScfgTexts::Format(TEXT("no_such_id_xyz"), {Chain}));\n'
        'Label(ScfgTexts::Get(Variable));\n'
        '// ScfgTexts::Get(TEXT("in_a_comment"))\n'
        '/* ScfgTexts::Get(TEXT("in_a_block")) */\n',
        encoding="utf-8")
    (tmp_path / "Sheet.h").write_text('auto X = ScfgTexts::Get( TEXT("from_a_header") );\n', encoding="utf-8")
    found = tool.unreal_problems(None, src=tmp_path)
    assert any("no_such_id_xyz" in p and "Sheet.cpp" in p for p in found), found
    assert any("from_a_header" in p and "Sheet.h" in p for p in found), found
    assert not any("in_a_comment" in p or "in_a_block" in p or "Variable" in p or "'cancel'" in p for p in found)


def test_the_ids_the_unreal_code_asks_for_are_found(tool):
    # the facade's notifications at least; the screens' ids too once they exist
    used = tool.unreal_used_ids()
    assert {"payment_confirmed_item", "payment_confirmed", "purchases_restored"} <= set(used), sorted(used)


def test_a_sheet_text_godot_does_not_show_is_reported_by_value(tool):
    texts = {**tool.unreal_texts(), "cancel": "Abort", "dev_only_line": "DEV: anything"}
    found = tool.unreal_problems(None, unreal=texts)
    assert any("'cancel'" in p and "Abort" in p for p in found), found
    assert not any("dev_only_line" in p for p in found), "the developer lines have no Godot string"


def test_with_rpg_maker_texts_every_id_is_compared_by_id(tool):
    texts = tool.unreal_texts()
    rm = dict(texts)
    rm.pop("check")
    rm["cancel"] = "Stop"
    found = tool.unreal_problems(rm)
    assert any("'check'" in p and "not an RM text id" in p for p in found), found
    assert any("'cancel'" in p and "RM's says 'Stop'" in p for p in found), found
    assert tool.unreal_problems(dict(texts)) == []


def test_the_unreal_branch_runs_without_node(tool, monkeypatch, capsys):
    monkeypatch.setattr(tool.shutil, "which", lambda name: None)
    assert tool.main(["--check", "--engine", "unreal"]) == 0
    out = capsys.readouterr()
    assert "skipped: node is not installed" in out.out
    assert "Unreal text ids are consistent" in out.out
    # the RPG Maker branch still fails closed, with the reason and no traceback
    assert tool.main(["--check"]) == 1
    err = capsys.readouterr().err
    assert "the RPG Maker branch cannot run: node is not installed" in err
    assert "Traceback" not in err


def test_a_node_too_old_for_the_ts_file_skips_only_the_rpg_maker_comparison(tool, monkeypatch, capsys):
    # Node 18 / 20 cannot run texts_dump.ts: exit 1 with a SyntaxError on stderr
    def old_node(*args, **kwargs):
        raise subprocess.CalledProcessError(1, ["node"], output="", stderr="SyntaxError: Unexpected token ':'\n")
    monkeypatch.setattr(tool.shutil, "which", lambda name: "/usr/bin/node")
    monkeypatch.setattr(tool.subprocess, "run", old_node)
    assert tool.main(["--check", "--engine", "unreal"]) == 0
    out = capsys.readouterr()
    assert "skipped: `node rpgmaker/src/mz/texts_dump.ts` exited with 1" in out.out
    assert "SyntaxError" in out.out
    assert tool.main(["--check"]) == 1
    err = capsys.readouterr().err
    assert "the RPG Maker branch cannot run: `node rpgmaker/src/mz/texts_dump.ts` exited with 1" in err
    assert "Traceback" not in err


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_repository_passes_against_rpg_maker(tool):
    assert tool.unreal_problems(tool.default_texts()) == []
    assert tool.main(["--check"]) == 0


@pytest.mark.parametrize("node", ["missing", "too old"])
def test_the_rpg_maker_branch_alone_fails_in_one_line(tool, monkeypatch, capsys, node):
    # phase 2 final re-review N1: `--engine rpgmaker` ended in a NodeMissing traceback
    def old_node(*args, **kwargs):
        raise subprocess.CalledProcessError(1, ["node"], output="", stderr="SyntaxError: Unexpected token ':'\n")
    monkeypatch.setattr(tool.shutil, "which", lambda name: None if node == "missing" else "/usr/bin/node")
    monkeypatch.setattr(tool.subprocess, "run", old_node)
    assert tool.main(["--check", "--engine", "rpgmaker"]) == 1
    out = capsys.readouterr()
    assert out.err.startswith("the RPG Maker branch cannot run: ")
    assert len(out.err.splitlines()) == 1 and "Traceback" not in out.err
    assert out.out == ""


@pytest.mark.parametrize("engine", ["all", "rpgmaker"])
def test_each_run_starts_node_once(tool, monkeypatch, capsys, engine):
    # phase 2 final re-review N1: `--engine all` ran Node for the Unreal branch and again in problems()
    runs = []

    def dump(*args, **kwargs):
        runs.append(args[0])
        return subprocess.CompletedProcess(args[0], 0, stdout=json.dumps({"cancel": "Cancel"}), stderr="")
    monkeypatch.setattr(tool.shutil, "which", lambda name: "/usr/bin/node")
    monkeypatch.setattr(tool.subprocess, "run", dump)
    assert tool.main(["--check", "--engine", engine]) == 1  # a two-text RM table has problems
    assert len(runs) == 1, runs
    assert "has no RM default text" in capsys.readouterr().err
    assert tool._HANDED_OVER == []  # nothing left for a later default_texts() call
