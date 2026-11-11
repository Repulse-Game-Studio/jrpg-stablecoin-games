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

@pytest.fixture(autouse=True)
def unpinned(monkeypatch):
    """Most tests use 4.3-stable as an unpinned version (checked against a fake SHA512-SUMS.txt);
    the pinned tests set PINNED themselves."""
    monkeypatch.setattr(fetch_godot, "PINNED", {})


REAL_PINS = fetch_godot.PINNED  # the module's own table, read before any test replaces it

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


# --- export templates (--templates) ---------------------------------------------------------

TPZ = "Godot_v4.3-stable_export_templates.tpz"


def _tpz(version_txt=b"4.3.stable\n", extra=()):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("templates/version.txt", version_txt)
        zf.writestr("templates/web_nothreads_release.zip", b"web release")
        zf.writestr("templates/web_debug.zip", b"web debug")
        zf.writestr("templates/linux_release.x86_64", b"not wanted")
        zf.writestr("templates/windows_release_x86_64.exe", b"not wanted")
        for name, data in extra:
            zf.writestr(name, data)
    return buf.getvalue()


def _fake_templates_release(monkeypatch, tpz_bytes, sums_text=None):
    """The release's SHA512-SUMS.txt (download) and the tpz, streamed (open_url). Returns
    the list of URLs that were streamed."""
    if sums_text is None:
        sums_text = f"{hashlib.sha512(tpz_bytes).hexdigest()}  {TPZ}\n"
    sums_url = fetch_godot.release_url("4.3-stable", "SHA512-SUMS.txt")
    monkeypatch.setattr(fetch_godot, "download", lambda url: {sums_url: sums_text.encode()}[url])
    streamed = []

    def open_url(url):
        assert url == fetch_godot.release_url("4.3-stable", TPZ)
        streamed.append(url)
        return io.BytesIO(tpz_bytes)

    monkeypatch.setattr(fetch_godot, "open_url", open_url)
    monkeypatch.setattr(fetch_godot, "CHUNK", 4)  # many chunks: the hash is built while streaming
    return streamed


def test_templates_name_and_url():
    assert fetch_godot.templates_name("4.3-stable") == TPZ
    assert fetch_godot.release_url("4.3-stable", TPZ).endswith("/4.3-stable/" + TPZ)


def test_data_dir_prefers_the_flag_then_xdg_then_home(tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "xdg"))
    assert fetch_godot.data_dir(tmp_path / "flag") == tmp_path / "flag"
    assert fetch_godot.data_dir() == tmp_path / "xdg"
    monkeypatch.delenv("XDG_DATA_HOME")
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert fetch_godot.data_dir() == tmp_path / "home" / ".local" / "share"


def test_templates_unpack_only_web_and_version(tmp_path, monkeypatch, capsys):
    streamed = _fake_templates_release(monkeypatch, _tpz(extra=[("templates/../evil", b"no")]))
    cache, data = tmp_path / "cache", tmp_path / "data"
    assert fetch_godot.main(["--version", "4.3-stable", "--templates", "--templates-dir",
                             str(data), "--cache", str(cache)]) == 0
    out = data / "godot" / "export_templates" / "4.3.stable"
    assert capsys.readouterr().out.strip() == str(out)
    assert sorted(p.name for p in out.iterdir()) == [
        "version.txt", "web_debug.zip", "web_nothreads_release.zip"]
    assert (out / "web_nothreads_release.zip").read_bytes() == b"web release"
    # kept for the next run, with the SHA-512 it was verified against (for an offline run)
    assert sorted(p.name for p in cache.iterdir()) == [TPZ, TPZ + ".sha512"]
    assert (cache / (TPZ + ".sha512")).read_text().strip() == hashlib.sha512(_tpz(
        extra=[("templates/../evil", b"no")])).hexdigest()
    assert not (tmp_path / "evil").exists() and not (data / "godot" / "evil").exists()
    assert len(streamed) == 1


def test_templates_reuse_a_verified_cached_archive(tmp_path, monkeypatch):
    data = _tpz()
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / TPZ).write_bytes(data)
    streamed = _fake_templates_release(monkeypatch, data)
    out = fetch_godot.fetch_templates("4.3-stable", tmp_path / "data", cache)
    assert (out / "version.txt").is_file()
    assert streamed == []


def _offline(monkeypatch):
    """SHA512-SUMS.txt cannot be downloaded (no network), and nothing may be streamed."""
    def download(url):
        raise fetch_godot.urllib.error.URLError("no network")

    def open_url(url):
        raise AssertionError(f"streamed {url} offline")

    monkeypatch.setattr(fetch_godot, "download", download)
    monkeypatch.setattr(fetch_godot, "open_url", open_url)


def test_templates_offline_reuse_a_cached_archive_verified_earlier(tmp_path, monkeypatch, capsys):
    data = _tpz()
    cache = tmp_path / "cache"
    _fake_templates_release(monkeypatch, data)
    fetch_godot.fetch_templates("4.3-stable", tmp_path / "first", cache)  # online: verified, recorded
    _offline(monkeypatch)
    assert fetch_godot.main(["--version", "4.3-stable", "--templates", "--templates-dir",
                             str(tmp_path / "data"), "--cache", str(cache)]) == 0
    out = tmp_path / "data" / "godot" / "export_templates" / "4.3.stable"
    captured = capsys.readouterr()
    assert captured.out.strip().splitlines()[-1] == str(out)
    assert (out / "web_nothreads_release.zip").read_bytes() == b"web release"
    assert "SHA512-SUMS.txt could not be downloaded" in captured.err and "no network" in captured.err
    assert "not a tampered one" in captured.err  # an unpinned version: corruption only


def test_templates_offline_cache_is_reused_only_when_its_recorded_sha_matches(tmp_path, monkeypatch, capsys):
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / TPZ).write_bytes(_tpz())  # a cached archive with no recorded SHA-512 (never verified)
    _offline(monkeypatch)
    args = ["--version", "4.3-stable", "--templates", "--templates-dir", str(tmp_path / "data"),
            "--cache", str(cache)]
    assert fetch_godot.main(args) == 1
    assert "no network" in capsys.readouterr().err
    (cache / (TPZ + ".sha512")).write_text(hashlib.sha512(b"another archive").hexdigest() + "\n")
    assert fetch_godot.main(args) == 1  # the archive changed since it was verified
    assert "no network" in capsys.readouterr().err
    assert not (tmp_path / "data").exists()


def test_templates_replace_a_cached_archive_that_fails_the_check(tmp_path, monkeypatch):
    data = _tpz()
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / TPZ).write_bytes(b"truncated")
    streamed = _fake_templates_release(monkeypatch, data)
    fetch_godot.fetch_templates("4.3-stable", tmp_path / "data", cache)
    assert len(streamed) == 1
    assert (cache / TPZ).read_bytes() == data


def test_templates_sha_mismatch_keeps_nothing(tmp_path, monkeypatch, capsys):
    _fake_templates_release(monkeypatch, _tpz(), "0" * 128 + f"  {TPZ}\n")
    cache, data = tmp_path / "cache", tmp_path / "data"
    assert fetch_godot.main(["--version", "4.3-stable", "--templates", "--templates-dir",
                             str(data), "--cache", str(cache)]) == 1
    assert "SHA-512 mismatch" in capsys.readouterr().err
    assert list(cache.iterdir()) == []  # no .part, no archive
    assert not data.exists()


def test_templates_interrupted_stream_leaves_no_part_file(tmp_path, monkeypatch):
    _fake_templates_release(monkeypatch, _tpz())

    class Broken(io.BytesIO):
        def read(self, n=-1):
            raise OSError("connection reset")

    monkeypatch.setattr(fetch_godot, "open_url", lambda url: Broken())
    with pytest.raises(OSError):
        fetch_godot.fetch_templates("4.3-stable", tmp_path / "data", tmp_path / "cache")
    assert list((tmp_path / "cache").iterdir()) == []


@pytest.mark.parametrize("version_txt", [b"../../x", b"4.3", b""])
def test_templates_reject_a_bad_version_txt(tmp_path, monkeypatch, version_txt):
    _fake_templates_release(monkeypatch, _tpz(version_txt))
    with pytest.raises(ValueError, match="version.txt"):
        fetch_godot.fetch_templates("4.3-stable", tmp_path / "data", tmp_path / "cache")
    assert not (tmp_path / "data").exists()


def test_templates_need_web_templates(tmp_path):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("templates/version.txt", b"4.3.stable")
        zf.writestr("templates/linux_release.x86_64", b"x")
    tpz = tmp_path / TPZ
    tpz.write_bytes(buf.getvalue())
    with pytest.raises(ValueError, match="no web templates"):
        fetch_godot.unpack_web_templates(tpz, tmp_path / "data")


def test_dest_is_required_without_templates(capsys):
    with pytest.raises(SystemExit):
        fetch_godot.main(["--version", "4.3-stable"])
    assert "--dest is required" in capsys.readouterr().err


# --- pinned versions -------------------------------------------------------------------------


def test_the_supported_versions_are_pinned():
    assert sorted(REAL_PINS) == ["4.3-stable", "4.7.2-stable"]
    for version, pins in REAL_PINS.items():
        assert sorted(pins) == sorted([fetch_godot.zip_name(version), fetch_godot.templates_name(version)])
        for digest in pins.values():
            assert len(digest) == 128 and digest == digest.lower() and int(digest, 16) >= 0
    # the 4.3 zip's pin is what that release's SHA512-SUMS.txt lists (the SUMS fixture above)
    assert REAL_PINS["4.3-stable"]["Godot_v4.3-stable_linux.x86_64.zip"] == fetch_godot.parse_sums(SUMS)[
        "Godot_v4.3-stable_linux.x86_64.zip"]


def _pin(monkeypatch, filename, digest):
    monkeypatch.setattr(fetch_godot, "PINNED", {"4.3-stable": {filename: digest}})


def _serve(monkeypatch, files):
    """download() serves only `files`: SHA512-SUMS.txt is never asked for a pinned version."""
    def download(url):
        assert not url.endswith("SHA512-SUMS.txt"), "a pinned version needs no SHA512-SUMS.txt"
        return files[url]

    monkeypatch.setattr(fetch_godot, "download", download)


def test_a_pinned_binary_is_checked_against_the_pin_only(tmp_path, monkeypatch, capsys):
    data = _godot_zip()
    name = "Godot_v4.3-stable_linux.x86_64.zip"
    _pin(monkeypatch, name, hashlib.sha512(data).hexdigest())
    _serve(monkeypatch, {fetch_godot.release_url("4.3-stable", name): data})
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path / "ok")]) == 0
    assert (tmp_path / "ok" / "Godot_v4.3-stable_linux.x86_64").read_bytes() == b"\x7fELF fake godot"
    _pin(monkeypatch, name, "0" * 128)  # a download that is not the pinned file
    assert fetch_godot.main(["--version", "4.3-stable", "--dest", str(tmp_path / "bad")]) == 1
    assert "SHA-512 mismatch" in capsys.readouterr().err
    assert list((tmp_path / "bad").iterdir()) == []


def test_a_pinned_template_archive_is_reused_offline_and_refetched_when_it_differs(tmp_path, monkeypatch):
    data = _tpz()
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / TPZ).write_bytes(data)
    _pin(monkeypatch, TPZ, hashlib.sha512(data).hexdigest())
    _offline(monkeypatch)  # no SHA512-SUMS.txt, no stream: the pin is enough
    out = fetch_godot.fetch_templates("4.3-stable", tmp_path / "data", cache)
    assert (out / "web_debug.zip").read_bytes() == b"web debug"
    assert sorted(p.name for p in cache.iterdir()) == [TPZ], "no record for a pinned version"
    (cache / TPZ).write_bytes(b"tampered")
    streamed = []

    def open_url(url):
        streamed.append(url)
        return io.BytesIO(data)

    monkeypatch.setattr(fetch_godot, "open_url", open_url)
    fetch_godot.fetch_templates("4.3-stable", tmp_path / "data2", cache)
    assert len(streamed) == 1 and (cache / TPZ).read_bytes() == data


def test_a_pinned_template_download_must_match_the_pin(tmp_path, monkeypatch, capsys):
    data = _tpz()
    _fake_templates_release(monkeypatch, data)  # SHA512-SUMS.txt agrees with the download...
    _pin(monkeypatch, TPZ, "0" * 128)  # ...but the pin does not
    cache = tmp_path / "cache"
    assert fetch_godot.main(["--version", "4.3-stable", "--templates", "--templates-dir",
                             str(tmp_path / "data"), "--cache", str(cache)]) == 1
    assert "SHA-512 mismatch" in capsys.readouterr().err
    assert list(cache.iterdir()) == []
