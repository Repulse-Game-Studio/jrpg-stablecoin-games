from pathlib import Path

import pytest

from scfg_renpy import cli

SOURCE = Path(__file__).resolve().parents[2] / "plugin" / "game"
VERSION = cli.read_version(SOURCE)


@pytest.fixture
def project(tmp_path):
    (tmp_path / "game").mkdir()
    (tmp_path / "game" / "script.rpy").write_text("label start:\n    return\n")
    return tmp_path


def test_bundled_game_dir_falls_back_to_the_repository():
    assert cli.bundled_game_dir() == SOURCE


def test_install_copies_both_parts(project):
    msg = cli.install(project, SOURCE)
    game = project / "game"
    assert (game / "scfg" / "scfg_api.rpy").is_file()
    assert (game / "python-packages" / "scfg_core" / "__init__.py").is_file()
    assert cli.read_version(game) == VERSION
    assert VERSION in msg
    assert not list(game.rglob("__pycache__")) and not list(game.rglob("*.rpyc"))


def test_install_accepts_the_game_folder_itself(project):
    cli.install(project / "game", SOURCE)
    assert (project / "game" / "scfg").is_dir()


def test_install_refuses_a_folder_that_is_not_a_renpy_project(tmp_path):
    with pytest.raises(cli.InstallError, match="doesn't look like a Ren'Py project"):
        cli.install(tmp_path, SOURCE)


def test_install_refuses_when_already_installed(project):
    cli.install(project, SOURCE)
    with pytest.raises(cli.InstallError, match="scfg-renpy update"):
        cli.install(project, SOURCE)


def test_update_replaces_and_drops_stale_files(project):
    cli.install(project, SOURCE)
    game = project / "game"
    stale = game / "scfg" / "removed_in_new_version.rpy"
    stale.write_text("")
    init = game / "python-packages" / "scfg_core" / "__init__.py"
    init.write_text(init.read_text().replace(VERSION, "0.0.1"))
    msg = cli.update(project, SOURCE)
    assert msg.startswith(f"Updated Stablecoin for Games 0.0.1 -> {VERSION}")
    assert not stale.exists()
    assert cli.read_version(game) == VERSION


def test_update_leaves_the_developers_files_alone(project):
    other = project / "game" / "python-packages" / "other_lib"
    other.mkdir(parents=True)
    (other / "__init__.py").write_text("")
    cli.update(project, SOURCE)
    assert (other / "__init__.py").is_file()
    assert (project / "game" / "script.rpy").is_file()


def test_update_installs_when_missing(project):
    assert cli.update(project, SOURCE).startswith("Installed")


def test_main_reports_errors_without_a_traceback(tmp_path, capsys):
    assert cli.main(["install", str(tmp_path)]) == 1
    assert "scfg-renpy:" in capsys.readouterr().err


def test_main_install_and_version(project, capsys):
    assert cli.main(["install", str(project)]) == 0
    assert "Next: add scfg.configure" in capsys.readouterr().out
    with pytest.raises(SystemExit):
        cli.main(["--version"])
    assert VERSION in capsys.readouterr().out


def test_npm_package_version_matches_the_plugin():
    # npm/scripts/bundle.js refuses to pack a mismatch; catch it at bump time instead.
    import json
    pkg = json.loads((SOURCE.parents[1] / "npm" / "package.json").read_text())
    assert pkg["version"] == VERSION
