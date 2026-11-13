import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("make_pot", ROOT / "tools" / "make_pot.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def pot(tmp_path, monkeypatch):
    module = _load()
    addon = tmp_path / "godot" / "addons" / "scfg"
    (addon / "ui").mkdir(parents=True)
    (addon / "core").mkdir()
    (addon / "ui" / "sheet.gd").write_text(
        'func _build() -> void:\n'
        '\tlabel.text = tr("Close")\n'
        '\tnote.text = tr("Say \\"hi\\"\\nnow") % name\n'
        '\tother.text = tr(dynamic_text)  # not a literal: not extracted\n'
        '\t# tr("in a comment") is not extracted\n'
        '\tb.text = tr(\'Single\')\n',
        encoding="utf-8")
    (addon / "ui" / "sheet.tscn").write_text(
        '[node name="Button" type="Button"]\ntext = "Pinned"\ntooltip_text = "Back"\n', encoding="utf-8")
    (addon / "core" / "restore_flow.gd").write_text(
        'const MESSAGES := {\n\t"tx_not_found": "Transaction not found.",\n\t"failed": "Something went wrong.",\n}\n',
        encoding="utf-8")
    messages = tmp_path / "messages.json"
    messages.write_text(json.dumps({"messages": {"invalid_hash": "That doesn't look like a hash.", "dup": "Close"}}),
                        encoding="utf-8")
    monkeypatch.setattr(module, "ROOT", tmp_path)
    monkeypatch.setattr(module, "GODOT", tmp_path / "godot")
    monkeypatch.setattr(module, "ADDON", addon)
    monkeypatch.setattr(module, "POT", addon / "translations" / "scfg.pot")
    monkeypatch.setattr(module, "MESSAGES_JSON", messages)
    monkeypatch.setattr(module, "RESTORE_FLOW", addon / "core" / "restore_flow.gd")
    return module, addon / "translations" / "scfg.pot"


def _msgids(text):
    return [line[len('msgid "'):-1] for line in text.splitlines() if line.startswith('msgid "') and line != 'msgid ""']


def test_make_pot_check():
    """The committed scfg.pot is what the tool generates from the addon today."""
    module = _load()
    assert module.POT.is_file(), "godot/addons/scfg/translations/scfg.pot is missing"
    assert module.main(["--check"]) == 0


def test_make_pot_collects_tr_calls_scenes_and_core_messages(pot):
    module, path = pot
    assert module.main([]) == 0
    text = path.read_text(encoding="utf-8")
    ids = _msgids(text)
    assert ids == sorted(set(ids)), "sorted, each once"
    assert set(ids) == {"Close", 'Say \\"hi\\"\\nnow', "Single", "Pinned", "Back", "That doesn't look like a hash.",
                        "Transaction not found.", "Something went wrong."}
    assert 'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"' in text
    # each entry names where it comes from; a string used twice lists both places
    block = text.split('msgid "Close"')[0].rsplit("\n\n", 1)[1]
    assert "#: addons/scfg/ui/sheet.gd" in block and "#. core message: dup" in block
    assert "#. core message: tx_not_found" in text and "#: addons/scfg/ui/sheet.tscn" in text
    assert module.main(["--check"]) == 0


def test_make_pot_check_fails_on_drift(pot, capsys):
    module, path = pot
    assert module.main(["--check"]) == 1, "missing"
    assert "scfg.pot" in capsys.readouterr().out
    assert module.main([]) == 0
    sheet = module.ADDON / "ui" / "sheet.gd"
    sheet.write_text(sheet.read_text(encoding="utf-8") + '\tx.text = tr("New text")\n', encoding="utf-8")
    assert module.main(["--check"]) == 1, "a new tr() string"
    assert module.main([]) == 0
    assert "New text" in _msgids(path.read_text(encoding="utf-8"))
    path.write_text(path.read_text(encoding="utf-8") + "\n# edited\n", encoding="utf-8")
    assert module.main(["--check"]) == 1, "a hand edit"


@pytest.mark.parametrize("body", [
    '\t"tx_not_found":\n\t\t"Transaction not found.",\n',  # the value on its own line
    '\t"tx_not_found": "Transaction not found.", "failed": "Something went wrong.",\n',  # two on a line
    '\t"tx_not_found": "Transaction " + "not found.",\n',  # not one literal
    '\t"failed": "Something went wrong."  # a comment\n\t"tx_not_found": tr_later("x"),\n',  # one unreadable
])
def test_make_pot_fails_loudly_on_restore_messages_it_cannot_read(pot, body, capsys):
    """An entry of the restore screen's MESSAGES that the line reader does not understand stops
    the tool (a silently dropped entry would leave a player text untranslatable)."""
    module, path = pot
    module.RESTORE_FLOW.write_text("const MESSAGES := {\n" + body + "}\n", encoding="utf-8")
    assert module.main([]) == 1
    out = capsys.readouterr().out
    assert "MESSAGES" in out and "restore_flow.gd" in out, out
    assert not path.exists(), "nothing written"
    assert module.main(["--check"]) == 1


def test_make_pot_fails_when_the_restore_messages_are_missing(pot, capsys):
    module, path = pot
    module.RESTORE_FLOW.write_text("const OTHER := {}\n", encoding="utf-8")
    assert module.main([]) == 1
    assert "MESSAGES" in capsys.readouterr().out
