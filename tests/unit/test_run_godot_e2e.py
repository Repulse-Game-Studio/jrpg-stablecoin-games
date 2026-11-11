"""The Godot desktop e2e runner (tools/run_godot_e2e.py), without Godot or a chain."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_godot_e2e as e2e  # noqa: E402


def test_refuses_to_run_over_an_existing_override_cfg(tmp_path, monkeypatch, capsys):
    """The run writes godot/demo/override.cfg and deletes it afterwards: a developer's own file
    there must stop the run before anything happens, and stay as it was."""
    override = tmp_path / "override.cfg"
    override.write_text("; mine\n[display]\n", encoding="utf-8")
    monkeypatch.setattr(e2e, "OVERRIDE", override)
    monkeypatch.setattr(e2e.shutil, "which", lambda name: "/usr/bin/" + name)

    def must_not_run(*a, **kw):
        raise AssertionError("the run started")

    monkeypatch.setattr(e2e, "sync_godot", must_not_run)
    monkeypatch.setattr(e2e.tempfile, "mkdtemp", must_not_run)
    assert e2e.run("godot", "evm") == 2
    assert override.read_text(encoding="utf-8") == "; mine\n[display]\n", "left as it was"
    out = capsys.readouterr().out
    assert str(override) in out and "override.cfg" in out, out


UID_LINES = (
    'WARNING: Missing .uid file for path "res://addons/scfg/core/evm.gd". The file was re-created from cache.\n'
    "     at: _process_file_system (editor/file_system/editor_file_system.cpp:1391)\n"
)


def test_import_problems_let_only_the_missing_uid_warning_through():
    """Godot 4.4+ warns about each script of the fresh addon copy (it has no .uid files); every
    other warning or error of the import step is a problem."""
    assert e2e.import_problems("Godot Engine v4.7.2\n" + UID_LINES * 3) == []
    other = "WARNING: res://addons/scfg/scfg.gd:10 - something else\n     at: somewhere (x.cpp:1)\n"
    errors = "ERROR: Failed to load script\nSCRIPT ERROR: Parse Error: x\n"
    assert e2e.import_problems(UID_LINES + other + errors) == [
        "WARNING: res://addons/scfg/scfg.gd:10 - something else",
        "ERROR: Failed to load script",
        "SCRIPT ERROR: Parse Error: x",
    ]
    # the "at:" line is skipped only right under the .uid warning
    assert (
        e2e.import_problems('WARNING: Missing .uid file for path "res://x.gd". Something else.\n')
        != []
    )


def _fake_godot(tmp_path, script: str) -> str:
    """An executable standing in for Godot: a Python script that ignores its arguments."""
    exe = tmp_path / "godot"
    exe.write_text("#!" + sys.executable + "\nimport sys\n" + script)
    exe.chmod(0o755)
    return str(exe)


def test_a_game_quitting_on_its_own_gets_its_stdin_closed(tmp_path):
    """The driver's stdin reader is joined at exit: a game that quits on its own (a failed run)
    prints QUITTING and waits until the runner closes its stdin. The runner does, so the game
    exits by itself with its own code (it is not killed)."""
    godot = _fake_godot(
        tmp_path,
        (
            "print('SCFG-QUITTING', flush=True)\n"
            "sys.stdin.read()  # the reader thread, until the runner closes stdin\n"
            "print('joined', flush=True)\n"
            "sys.exit(1)\n"
        ),
    )
    game = e2e.Game(godot, {}, tmp_path / "game.log")
    try:
        assert game.wait_exit(10) == 1
        assert "joined" in game.output()
    finally:
        game.close()


def test_a_call_to_a_game_that_quits_on_its_own_fails_at_once(tmp_path):
    godot = _fake_godot(
        tmp_path,
        (
            "sys.stdin.readline()\n"
            "print('SCFG-QUITTING', flush=True)\n"
            "sys.stdin.read()\n"
            "sys.exit(1)\n"
        ),
    )
    game = e2e.Game(godot, {}, tmp_path / "game.log")
    try:
        start = e2e.time.monotonic()
        try:
            game.call("snapshot")
            raise AssertionError("no error")
        except e2e.E2EError as e:
            assert "no reply to snapshot" in str(e)
        assert e2e.time.monotonic() - start < e2e.REPLY_TIMEOUT / 2, "not after the reply timeout"
        assert game.proc.wait(10) == 1
    finally:
        game.close()


def test_close_lets_a_game_waiting_on_stdin_exit_before_killing_it(tmp_path):
    godot = _fake_godot(tmp_path, "sys.stdin.read()\nsys.exit(3)\n")
    game = e2e.Game(godot, {}, tmp_path / "game.log")
    game.close()
    assert game.proc.returncode == 3, "it exited by itself once stdin was closed"
