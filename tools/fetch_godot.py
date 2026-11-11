#!/usr/bin/env python3
"""Download a Godot Linux x86_64 release, verify its SHA-512 and print the binary's path.

    python tools/fetch_godot.py --version 4.3-stable --dest /tmp/godot

The zip comes from the godotengine/godot GitHub release of that tag and is checked before the
binary is extracted: for the versions the repository supports (PINNED: 4.3-stable and
4.7.2-stable) against the SHA-512 recorded here, else against the release's SHA512-SUMS.txt. If
the binary is already in --dest, nothing is downloaded.

With --templates it installs that release's web export templates instead and prints their
directory:

    python tools/fetch_godot.py --version 4.3-stable --templates [--templates-dir DIR]

The export templates archive (about 1 GB, Godot_v<version>_export_templates.tpz) is streamed
to --cache (default /tmp/godot-templates) and checked while it downloads, against the pinned
SHA-512 or else SHA512-SUMS.txt; a copy there that passes the same check is reused. A pinned
version needs no SHA512-SUMS.txt, so it also works offline. For an unpinned version, when
SHA512-SUMS.txt cannot be downloaded (offline), the cached copy is checked against the SHA-512
recorded beside it (`<archive>.sha512`) when it was verified: that only guards against a
corrupted copy, not against one tampered with together with its record. Only the web templates
(web_*) and version.txt are unpacked, into <data>/godot/export_templates/<version.txt>/, where
<data> is --templates-dir, else $XDG_DATA_HOME, else ~/.local/share. Godot finds them there when
it runs with XDG_DATA_HOME=<data>.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import os
import re
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

RELEASES = "https://github.com/godotengine/godot/releases/download"
SUMS_FILE = "SHA512-SUMS.txt"
_VERSION = re.compile(r"^\d+\.\d+(\.\d+)*-[a-z]+\d*$")
_SUM_LINE = re.compile(r"^([0-9a-fA-F]{128}) [ *](\S+)$")
# version.txt in the templates archive, such as "4.3.stable": the templates directory's name.
_TEMPLATES_VERSION = re.compile(r"^\d+\.\d+(\.\d+)*\.[a-z]+\d*$")
TEMPLATES_CACHE = Path("/tmp/godot-templates")
# The SHA-512 of the downloads of the Godot versions the repository supports (CI and the e2e
# runs), copied from each release's SHA512-SUMS.txt on 2026-11-11. A pinned file is checked
# against this, never against a SHA512-SUMS.txt fetched at run time.
PINNED = {
    "4.3-stable": {
        "Godot_v4.3-stable_linux.x86_64.zip":
            "fd52bb4ba8acc30ca5accd1c566d470ad7282f891ccc0995dfafabcf92bcf762"
            "80ce182bf9d80ebd885f3ed2165d01e1fc3f2928436b15498dfbd98656c2a45a",
        "Godot_v4.3-stable_export_templates.tpz":
            "476366caf0fd45a8f24136cf9cf1dc0bc2b96f7c82d53e5f82200b55aefd07b2"
            "86d283fd6f1ce29e0de70648c5a51d3b12f96c6d4fafd4e8c4878ecda6406d6a",
    },
    "4.7.2-stable": {
        "Godot_v4.7.2-stable_linux.x86_64.zip":
            "9aa00f7a605200940bce3027a567b782f49bd8e940dd06ae9e987bd65aee1b14"
            "67edd56ed84fcdcbdd44354bf613bdbb4e5d2913e925850368e150c59ed54c65",
        "Godot_v4.7.2-stable_export_templates.tpz":
            "ca4d71c4d7b81dfc15d1a98baa07534aa95b03fdda78a0075b06672e1648d2e5"
            "f40980c9adc28d23e1b92e732ee7bf3461997aa804af74ec2fcd7a93ccb84079",
    },
}
CHUNK = 1 << 20


def check_version(version: str) -> str:
    if not _VERSION.match(version):
        raise ValueError(f"not a Godot release tag (like 4.3-stable): {version!r}")
    return version


def binary_name(version: str) -> str:
    return f"Godot_v{version}_linux.x86_64"


def zip_name(version: str) -> str:
    return binary_name(version) + ".zip"


def templates_name(version: str) -> str:
    return f"Godot_v{version}_export_templates.tpz"


def release_url(version: str, filename: str) -> str:
    return f"{RELEASES}/{version}/{filename}"


def parse_sums(text: str) -> dict:
    """`sha512sum` output (text or binary mode lines) as {filename: lowercase hex digest}."""
    sums = {}
    for line in text.splitlines():
        m = _SUM_LINE.match(line.strip())
        if m:
            sums[m.group(2)] = m.group(1).lower()
    return sums


def expected_sha512(sums: dict, filename: str) -> str:
    if filename not in sums:
        raise ValueError(f"{filename} is not listed in {SUMS_FILE}")
    return sums[filename]


def _check_sha512(actual: str, expected: str) -> None:
    if actual != expected.lower():
        raise ValueError(f"SHA-512 mismatch: expected {expected}, got {actual}")


def verify_sha512(data: bytes, expected: str) -> None:
    _check_sha512(hashlib.sha512(data).hexdigest(), expected)


@contextlib.contextmanager
def _staged(out: Path):
    """Yield a `.part` path beside `out` to write; when the block succeeds it replaces `out`,
    when anything fails it is removed (so `out` is never left half-written)."""
    tmp = out.with_name(out.name + ".part")
    try:
        yield tmp
        os.replace(tmp, out)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise


def extract_binary(zip_bytes: bytes, member: str, dest: Path) -> Path:
    """Write only `member` from the zip to dest/member (never a path from the archive)."""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
        try:
            data = zf.read(member)
        except KeyError:
            raise ValueError(f"{member} is not in the archive") from None
    out = Path(dest) / member
    with _staged(out) as tmp:
        tmp.write_bytes(data)
        tmp.chmod(0o755)
    return out


def open_url(url: str):
    return urllib.request.urlopen(url, timeout=300)  # always an https github.com URL


def download(url: str) -> bytes:
    with open_url(url) as resp:
        return resp.read()


def download_verified(url: str, out: Path, expected: str) -> Path:
    """Stream `url` to `out`, hashing it on the way; `out` only appears if its SHA-512 is
    `expected` (a `.part` file holds it until then and is removed on any failure)."""
    out = Path(out)
    digest = hashlib.sha512()
    with _staged(out) as tmp:
        with open_url(url) as resp, tmp.open("wb") as f:
            while chunk := resp.read(CHUNK):
                digest.update(chunk)
                f.write(chunk)
        _check_sha512(digest.hexdigest(), expected)
    return out


def file_sha512(path: Path) -> str:
    digest = hashlib.sha512()
    with Path(path).open("rb") as f:
        while chunk := f.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def pinned_sha512(version: str, filename: str) -> str | None:
    """The SHA-512 PINNED for this download, or None (an unpinned version)."""
    return PINNED.get(version, {}).get(filename)


def release_sums(version: str) -> dict:
    """The release's SHA512-SUMS.txt (downloaded), as parse_sums gives it."""
    return parse_sums(download(release_url(version, SUMS_FILE)).decode("utf-8"))


def release_sha512(version: str, filename: str) -> str:
    """The SHA-512 a download must have: the pinned one, else the one the release's
    SHA512-SUMS.txt lists (downloaded)."""
    pinned = pinned_sha512(version, filename)
    if pinned:
        return pinned
    return expected_sha512(release_sums(version), filename)


def fetch(version: str, dest: Path) -> Path:
    check_version(version)
    dest = Path(dest)
    binary = dest / binary_name(version)
    if binary.is_file():
        return binary
    dest.mkdir(parents=True, exist_ok=True)
    expected = release_sha512(version, zip_name(version))
    data = download(release_url(version, zip_name(version)))
    verify_sha512(data, expected)
    return extract_binary(data, binary_name(version), dest)


def data_dir(templates_dir=None) -> Path:
    """Where Godot looks for user data on Linux: --templates-dir, $XDG_DATA_HOME or
    ~/.local/share."""
    if templates_dir:
        return Path(templates_dir)
    xdg = os.environ.get("XDG_DATA_HOME", "")
    return Path(xdg) if xdg else Path.home() / ".local" / "share"


def is_web_template(member: str) -> bool:
    name = member.rsplit("/", 1)[-1]
    return name.startswith("web_") and name.endswith(".zip")


def unpack_web_templates(tpz: Path, root: Path) -> Path:
    """Unpack version.txt and the web_*.zip templates of the archive `tpz` (a zip of
    templates/<file>) into root/godot/export_templates/<version>/; returns that directory.
    Members are written under their base name only, never a path from the archive."""
    with zipfile.ZipFile(tpz) as zf:
        try:
            version = zf.read("templates/version.txt").decode("utf-8").strip()
        except KeyError:
            raise ValueError(f"{tpz.name} has no templates/version.txt") from None
        if not _TEMPLATES_VERSION.match(version):
            raise ValueError(f"{tpz.name}: unexpected version.txt {version!r}")
        members = [m for m in zf.namelist()
                   if m.startswith("templates/") and m.count("/") == 1 and is_web_template(m)]
        if not members:
            raise ValueError(f"{tpz.name} has no web templates")
        out = Path(root) / "godot" / "export_templates" / version
        out.mkdir(parents=True, exist_ok=True)
        for member in members + ["templates/version.txt"]:
            with _staged(out / member.rsplit("/", 1)[-1]) as tmp:
                with zf.open(member) as src, tmp.open("wb") as dst:
                    shutil.copyfileobj(src, dst, CHUNK)
    return out


def _recorded_sha512(path: Path) -> str | None:
    """The SHA-512 written beside a cached archive when it was verified, or None."""
    try:
        text = Path(path).read_text(encoding="ascii").strip().lower()
    except (OSError, UnicodeDecodeError):
        return None
    return text if re.fullmatch(r"[0-9a-f]{128}", text) else None


def _record_sha512(path: Path, digest: str) -> None:
    with _staged(Path(path)) as tmp:
        tmp.write_text(digest.lower() + "\n", encoding="ascii")


def fetch_templates(version: str, templates_dir=None, cache: Path = TEMPLATES_CACHE) -> Path:
    """Install the web export templates of `version` (see the module doc); returns their
    directory. An archive in `cache` that passes the check (the pinned SHA-512, else
    SHA512-SUMS.txt) is reused; one that fails it is fetched again. For an unpinned version the
    SHA-512 an archive was verified against is kept beside it (`<archive>.sha512`): when
    SHA512-SUMS.txt cannot be downloaded (offline), a cached archive that still has that SHA-512
    is reused, with a note on stderr (a guard against corruption only, not tampering)."""
    check_version(version)
    name = templates_name(version)
    cache = Path(cache)
    tpz = cache / name
    recorded = cache / (name + ".sha512")
    expected = pinned_sha512(version, name)
    if expected is None:
        try:
            sums = release_sums(version)
        except OSError as e:  # URLError is an OSError
            known = _recorded_sha512(recorded)
            if not (known and tpz.is_file() and file_sha512(tpz) == known):
                raise
            print(f"fetch_godot: {SUMS_FILE} could not be downloaded ({e}); reusing {tpz}, which "
                  "still has the SHA-512 it was verified against (an unpinned version: this guards "
                  "against a corrupted copy, not a tampered one)", file=sys.stderr)
            return unpack_web_templates(tpz, data_dir(templates_dir))
        expected = expected_sha512(sums, name)
    cache.mkdir(parents=True, exist_ok=True)
    if not (tpz.is_file() and file_sha512(tpz) == expected):
        tpz.unlink(missing_ok=True)
        recorded.unlink(missing_ok=True)
        download_verified(release_url(version, name), tpz, expected)
    if pinned_sha512(version, name) is None:
        _record_sha512(recorded, expected)
    return unpack_web_templates(tpz, data_dir(templates_dir))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True, help="release tag, for example 4.3-stable")
    parser.add_argument("--dest", help="directory to put the binary in")
    parser.add_argument("--templates", action="store_true",
                        help="install the web export templates instead of the binary")
    parser.add_argument("--templates-dir",
                        help="data directory to install them under (Godot's XDG_DATA_HOME)")
    parser.add_argument("--cache", default=str(TEMPLATES_CACHE),
                        help="where the templates archive is kept (default %(default)s)")
    args = parser.parse_args(argv)
    if not args.templates and not args.dest:
        parser.error("--dest is required (or pass --templates)")
    try:
        if args.templates:
            print(fetch_templates(args.version, args.templates_dir, Path(args.cache)))
        else:
            print(fetch(args.version, Path(args.dest)))
    except (ValueError, OSError, zipfile.BadZipFile) as e:  # URLError is an OSError
        print(f"fetch_godot: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
