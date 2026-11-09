import hashlib
import importlib.util
import io
import zipfile
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "fetch_godot.py"
spec = importlib.util.spec_from_file_location("fetch_godot", TOOL)
fetch_godot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_godot)

SUMS = (
    "35802c61a20f30c3956b2753686c622e6c036d65639eb8bddeb3e876a033a3c59eb39a3c69de0e11e07ee8e753e5"
    "f754a7f7aaf0bcb10627e9974ba22540fbf2  Godot_v4.3-stable_linux.arm32.zip\n"
    "FD52BB4BA8ACC30CA5ACCD1C566D470AD7282F891CCC0995DFAFABCF92BCF76280CE182BF9D80EBD885F3ED2165D"
    "01E1FC3F2928436B15498DFBD98656C2A45A *Godot_v4.3-stable_linux.x86_64.zip\n"
    "\n"
    "not a checksum line\n"
)


def test_names_and_urls():
    assert fetch_godot.zip_name("4.3-stable") == "Godot_v4.3-stable_linux.x86_64.zip"
    assert fetch_godot.binary_name("4.3-stable") == "Godot_v4.3-stable_linux.x86_64"
    assert fetch_godot.release_url("4.3-stable", "SHA512-SUMS.txt") == (
        "https://github.com/godotengine/godot/releases/download/4.3-stable/SHA512-SUMS.txt")
    assert fetch_godot.release_url("4.4.1-stable", "Godot_v4.4.1-stable_linux.x86_64.zip") == (
        "https://github.com/godotengine/godot/releases/download/4.4.1-stable/"
        "Godot_v4.4.1-stable_linux.x86_64.zip")


@pytest.mark.parametrize("bad", ["", "4.3", "latest", "../4.3-stable", "4.3-stable/x", "4.3 stable"])
def test_rejects_malformed_versions(bad):
    with pytest.raises(ValueError):
        fetch_godot.check_version(bad)


@pytest.mark.parametrize("good", ["4.3-stable", "4.4.1-stable", "4.5-rc2"])
def test_accepts_release_tags(good):
    assert fetch_godot.check_version(good) == good


def test_parse_sums_reads_text_and_binary_mode_lines():
    sums = fetch_godot.parse_sums(SUMS)
    assert sorted(sums) == ["Godot_v4.3-stable_linux.arm32.zip", "Godot_v4.3-stable_linux.x86_64.zip"]
    assert sums["Godot_v4.3-stable_linux.x86_64.zip"].startswith("fd52bb4b")  # lowercased
    assert len(sums["Godot_v4.3-stable_linux.x86_64.zip"]) == 128


def test_expected_sha512_requires_the_file():
    sums = fetch_godot.parse_sums(SUMS)
    assert fetch_godot.expected_sha512(sums, "Godot_v4.3-stable_linux.x86_64.zip").startswith("fd52")
    with pytest.raises(ValueError):
        fetch_godot.expected_sha512(sums, "Godot_v4.3-stable_win64.exe.zip")


def test_verify_sha512():
    data = b"godot"
    good = hashlib.sha512(data).hexdigest()
    fetch_godot.verify_sha512(data, good)
    with pytest.raises(ValueError):
        fetch_godot.verify_sha512(data, "0" * 128)


def test_extract_binary_writes_only_the_named_member(tmp_path):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("Godot_v4.3-stable_linux.x86_64", b"\x7fELF fake")
        zf.writestr("../evil", b"no")
    out = fetch_godot.extract_binary(buf.getvalue(), "Godot_v4.3-stable_linux.x86_64", tmp_path)
    assert out == tmp_path / "Godot_v4.3-stable_linux.x86_64"
    assert out.read_bytes() == b"\x7fELF fake"
    assert out.stat().st_mode & 0o111
    assert sorted(p.name for p in tmp_path.iterdir()) == ["Godot_v4.3-stable_linux.x86_64"]
    with pytest.raises(ValueError):
        fetch_godot.extract_binary(buf.getvalue(), "missing", tmp_path)


def test_existing_binary_skips_the_network(tmp_path, monkeypatch, capsys):
    binary = tmp_path / "Godot_v4.3-stable_linux.x86_64"
    binary.write_bytes(b"already here")

    def no_network(url):
        raise AssertionError(f"downloaded {url}")

    monkeypatch.setattr(fetch_godot, "download", no_network)
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path)]) == 0
    assert capsys.readouterr().out.strip() == str(binary)


def _fake_release(monkeypatch, zip_bytes, sums_text):
    files = {
        fetch_godot.release_url("4.3-stable", "SHA512-SUMS.txt"): sums_text.encode(),
        fetch_godot.release_url("4.3-stable", "Godot_v4.3-stable_linux.x86_64.zip"): zip_bytes,
    }
    monkeypatch.setattr(fetch_godot, "download", lambda url: files[url])


def _godot_zip():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("Godot_v4.3-stable_linux.x86_64", b"\x7fELF fake godot")
    return buf.getvalue()


def test_fetch_happy_path(tmp_path, monkeypatch, capsys):
    data = _godot_zip()
    sums = f"{hashlib.sha512(data).hexdigest()}  Godot_v4.3-stable_linux.x86_64.zip\n"
    _fake_release(monkeypatch, data, sums)
    dest = tmp_path / "dl"
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(dest)]) == 0
    binary = dest / "Godot_v4.3-stable_linux.x86_64"
    assert capsys.readouterr().out.strip() == str(binary)
    assert binary.read_bytes() == b"\x7fELF fake godot"
    assert sorted(p.name for p in dest.iterdir()) == [binary.name]


def test_fetch_sha_mismatch_leaves_nothing(tmp_path, monkeypatch, capsys):
    _fake_release(monkeypatch, _godot_zip(), "0" * 128 + "  Godot_v4.3-stable_linux.x86_64.zip\n")
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path)]) == 1
    assert "SHA-512 mismatch" in capsys.readouterr().err
    assert list(tmp_path.iterdir()) == []


def test_fetch_zip_not_in_sums(tmp_path, monkeypatch, capsys):
    _fake_release(monkeypatch, _godot_zip(), "0" * 128 + "  Godot_v4.3-stable_win64.exe.zip\n")
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path)]) == 1
    assert "not listed" in capsys.readouterr().err
    assert list(tmp_path.iterdir()) == []


def test_fetch_reports_network_and_archive_errors(tmp_path, monkeypatch, capsys):
    def offline(url):
        raise fetch_godot.urllib.error.URLError("no route")

    monkeypatch.setattr(fetch_godot, "download", offline)
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path)]) == 1
    assert capsys.readouterr().err.startswith("fetch_godot: ")
    junk = b"not a zip"
    _fake_release(monkeypatch, junk, f"{hashlib.sha512(junk).hexdigest()}  Godot_v4.3-stable_linux.x86_64.zip\n")
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path)]) == 1
    assert capsys.readouterr().err.startswith("fetch_godot: ")
    assert list(tmp_path.iterdir()) == []


def test_extract_binary_removes_the_part_file_on_failure(tmp_path, monkeypatch):
    def fail_replace(src, dst):
        raise OSError("disk full")

    monkeypatch.setattr(fetch_godot.os, "replace", fail_replace)
    with pytest.raises(OSError):
        fetch_godot.extract_binary(_godot_zip(), "Godot_v4.3-stable_linux.x86_64", tmp_path)
    assert list(tmp_path.iterdir()) == []
