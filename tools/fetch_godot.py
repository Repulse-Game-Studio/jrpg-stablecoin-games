#!/usr/bin/env python3
"""Download a Godot Linux x86_64 release, verify its SHA-512 and print the binary's path.

    python tools/fetch_godot.py --version 4.3-stable --dest /tmp/godot

The zip comes from the godotengine/godot GitHub release of that tag and is checked against
the release's SHA512-SUMS.txt before the binary is extracted. If the binary is already in
--dest, nothing is downloaded.

With --templates it installs that release's web export templates instead and prints their
directory:

    python tools/fetch_godot.py --version 4.3-stable --templates [--templates-dir DIR]

The export templates archive (about 1 GB, Godot_v<version>_export_templates.tpz) is streamed
to --cache (default /tmp/godot-templates) and checked against SHA512-SUMS.txt while it
downloads; a verified copy there is reused. Only the web templates (web_*) and version.txt are
unpacked, into <data>/godot/export_templates/<version.txt>/, where <data> is --templates-dir,
else $XDG_DATA_HOME, else ~/.local/share. Godot finds them there when it runs with
XDG_DATA_HOME=<data>.
"""

from __future__ import annotations

import argparse
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


def verify_sha512(data: bytes, expected: str) -> None:
    actual = hashlib.sha512(data).hexdigest()
    if actual != expected.lower():
        raise ValueError(f"SHA-512 mismatch: expected {expected}, got {actual}")


def extract_binary(zip_bytes: bytes, member: str, dest: Path) -> Path:
    """Write only `member` from the zip to dest/member (never a path from the archive)."""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
        try:
            data = zf.read(member)
        except KeyError:
            raise ValueError(f"{member} is not in the archive") from None
    out = Path(dest) / member
    tmp = out.with_name(out.name + ".part")
    try:
        tmp.write_bytes(data)
        tmp.chmod(0o755)
        os.replace(tmp, out)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
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
    tmp = out.with_name(out.name + ".part")
    digest = hashlib.sha512()
    try:
        with open_url(url) as resp, tmp.open("wb") as f:
            while chunk := resp.read(CHUNK):
                digest.update(chunk)
                f.write(chunk)
        if digest.hexdigest() != expected.lower():
            raise ValueError(f"SHA-512 mismatch: expected {expected}, got {digest.hexdigest()}")
        os.replace(tmp, out)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    return out


def file_sha512(path: Path) -> str:
    digest = hashlib.sha512()
    with Path(path).open("rb") as f:
        while chunk := f.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(version: str, dest: Path) -> Path:
    check_version(version)
    dest = Path(dest)
    binary = dest / binary_name(version)
    if binary.is_file():
        return binary
    dest.mkdir(parents=True, exist_ok=True)
    sums = parse_sums(download(release_url(version, SUMS_FILE)).decode("utf-8"))
    expected = expected_sha512(sums, zip_name(version))
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
            target = out / member.rsplit("/", 1)[-1]
            tmp = target.with_name(target.name + ".part")
            try:
                with zf.open(member) as src, tmp.open("wb") as dst:
                    shutil.copyfileobj(src, dst, CHUNK)
                os.replace(tmp, target)
            except BaseException:
                tmp.unlink(missing_ok=True)
                raise
    return out


def fetch_templates(version: str, templates_dir=None, cache: Path = TEMPLATES_CACHE) -> Path:
    """Install the web export templates of `version` (see the module doc); returns their
    directory. A verified archive in `cache` is reused; one that fails the check is fetched
    again."""
    check_version(version)
    name = templates_name(version)
    sums = parse_sums(download(release_url(version, SUMS_FILE)).decode("utf-8"))
    expected = expected_sha512(sums, name)
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    tpz = cache / name
    if not (tpz.is_file() and file_sha512(tpz) == expected):
        tpz.unlink(missing_ok=True)
        download_verified(release_url(version, name), tpz, expected)
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
