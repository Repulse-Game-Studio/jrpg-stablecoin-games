import importlib.util
import os
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def sync(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("sync_web_page", ROOT / "tools" / "sync_web_page.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    src = tmp_path / "src"
    src.mkdir()
    (src / "index.html").write_bytes(b"<html>\r\n</html>\n")
    (src / "app.js").write_bytes(b"let a = 1;\n")
    target = tmp_path / "target"
    monkeypatch.setattr(module, "SRC", src)
    monkeypatch.setattr(module, "TARGETS", [target])
    return module, src, target


def test_sync_copies_bytes(sync):
    module, src, target = sync
    assert module.main([]) == 0
    for name in ("index.html", "app.js"):
        assert (target / name).read_bytes() == (src / name).read_bytes()
    assert module.main(["--check"]) == 0


def test_sync_removes_extra_files(sync):
    module, _, target = sync
    target.mkdir()
    (target / "old.js").write_bytes(b"x")
    assert module.main([]) == 0
    assert not (target / "old.js").exists()


def test_check_detects_drift_missing_extra(sync, capsys):
    module, src, target = sync
    module.main([])
    (target / "app.js").write_bytes(b"let a = 2;\n")
    (target / "index.html").unlink()
    (target / "extra.js").write_bytes(b"x")
    capsys.readouterr()
    assert module.main(["--check"]) == 1
    captured = capsys.readouterr()
    assert "app.js" in captured.out and "drifted" in captured.out
    assert "index.html" in captured.out and "missing" in captured.out
    assert "extra.js" in captured.out and "extra" in captured.out
    assert "tools/sync_web_page.py" in captured.err


def test_check_missing_target_dir(sync):
    module, _, _ = sync
    assert module.main(["--check"]) == 1


def test_check_reports_source_without_files(sync, capsys):
    module, src, _ = sync
    for p in src.iterdir():
        p.unlink()
    assert module.main(["--check"]) == 1
    assert "source has no files" in capsys.readouterr().out


def test_sync_never_writes_through_a_symlink(sync, tmp_path):
    module, src, target = sync
    target.mkdir()
    outside = tmp_path / "outside.js"
    outside.write_bytes(b"keep me")
    (target / "app.js").symlink_to(outside)
    assert module.main([]) == 0
    assert outside.read_bytes() == b"keep me"
    assert not (target / "app.js").is_symlink()
    assert (target / "app.js").read_bytes() == (src / "app.js").read_bytes()
    assert module.main(["--check"]) == 0


def test_sync_removes_an_extra_symlink_not_its_target(sync, tmp_path):
    module, _, target = sync
    target.mkdir()
    outside = tmp_path / "outside.js"
    outside.write_bytes(b"keep me")
    (target / "old.js").symlink_to(outside)
    assert module.main([]) == 0
    assert not (target / "old.js").is_symlink() and outside.read_bytes() == b"keep me"


def test_sync_refuses_a_directory_in_the_target(sync, capsys):
    module, _, target = sync
    (target / "app.js").mkdir(parents=True)
    (target / "app.js" / "inner").write_bytes(b"x")
    assert module.main([]) == 1
    assert (target / "app.js" / "inner").read_bytes() == b"x"
    assert "app.js" in capsys.readouterr().err


def test_sync_refuses_a_symlinked_target_dir(sync, tmp_path):
    module, _, target = sync
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    target.symlink_to(elsewhere, target_is_directory=True)
    assert module.main([]) == 1
    assert list(elsewhere.iterdir()) == []


def test_check_reports_symlinks_and_directories(sync, capsys, tmp_path):
    module, src, target = sync
    module.main([])
    (target / "app.js").unlink()
    (target / "app.js").symlink_to(src / "app.js")  # same bytes, still a problem
    (target / "index.html").unlink()
    (target / "index.html").mkdir()
    (target / "sub").mkdir()
    capsys.readouterr()
    assert module.main(["--check"]) == 1
    out = capsys.readouterr().out
    assert "symlink" in out and "app.js" in out
    assert "not a file" in out and "index.html" in out
    assert "subdirectory" in out and "sub" in out


def test_check_reports_a_symlinked_target_dir(sync, capsys, tmp_path):
    module, src, target = sync
    module.main([])
    real = tmp_path / "real"
    target.rename(real)
    target.symlink_to(real, target_is_directory=True)
    capsys.readouterr()
    assert module.main(["--check"]) == 1
    assert "symlink" in capsys.readouterr().out


def test_sync_never_writes_through_a_hard_link(sync, tmp_path):
    module, src, target = sync
    target.mkdir()
    outside = tmp_path / "outside.js"
    outside.write_bytes(b"keep me")
    os.link(outside, target / "app.js")
    assert module.main([]) == 0
    assert outside.read_bytes() == b"keep me"
    assert (target / "app.js").stat().st_nlink == 1
    assert (target / "app.js").read_bytes() == (src / "app.js").read_bytes()
    assert module.main(["--check"]) == 0


def test_check_reports_a_hard_linked_copy(sync, capsys, tmp_path):
    module, src, target = sync
    module.main([])
    (target / "app.js").unlink()
    os.link(src / "app.js", target / "app.js")  # same bytes, and a sync would write the source
    capsys.readouterr()
    assert module.main(["--check"]) == 1
    out = capsys.readouterr().out
    assert "hard link" in out and "app.js" in out
