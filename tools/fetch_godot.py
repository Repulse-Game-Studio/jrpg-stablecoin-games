#!/usr/bin/env python3
"""Download a Godot Linux x86_64 release, verify its SHA-512 and print the binary's path.

    python tools/fetch_godot.py --version 4.3-stable --dest /tmp/godot

The zip comes from the godotengine/godot GitHub release of that tag and is checked against
the release's SHA512-SUMS.txt before the binary is extracted. If the binary is already in
--dest, nothing is downloaded.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

RELEASES = "https://github.com/godotengine/godot/releases/download"
SUMS_FILE = "SHA512-SUMS.txt"
_VERSION = re.compile(r"^\d+\.\d+(\.\d+)*-[a-z]+\d*$")
_SUM_LINE = re.compile(r"^([0-9a-fA-F]{128}) [ *](\S+)$")


def check_version(version: str) -> str:
    if not _VERSION.match(version):
        raise ValueError(f"not a Godot release tag (like 4.3-stable): {version!r}")
    return version


def binary_name(version: str) -> str:
    return f"Godot_v{version}_linux.x86_64"


def zip_name(version: str) -> str:
    return binary_name(version) + ".zip"


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


def download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=300) as resp:  # always an https github.com URL
        return resp.read()


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


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True, help="release tag, for example 4.3-stable")
    parser.add_argument("--dest", required=True, help="directory to put the binary in")
    args = parser.parse_args(argv)
    try:
        print(fetch(args.version, Path(args.dest)))
    except (ValueError, OSError, zipfile.BadZipFile) as e:  # URLError is an OSError
        print(f"fetch_godot: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
