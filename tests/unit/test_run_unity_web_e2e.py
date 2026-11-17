"""tools/run_unity_web_e2e.py's pure parts (the run itself needs a Unity editor, a browser and a
local chain)."""

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_unity_web_e2e as web  # noqa: E402

BUILD_SCRIPT = ROOT / "unity" / "Project" / "Assets" / "E2E" / "Editor" / "ScfgE2EBuild.cs"
PACKAGE = ROOT / "unity" / "Packages" / "com.repulse.scfg"
MIRROR = PACKAGE / "Runtime" / "Unity" / "Web" / "ScfgWebMirror.cs"


def test_the_mirror_key_is_the_packages():
    product = "SCFG E2E"
    assert web.mirror_key() == f"scfg:{product}:{hashlib.sha256(product.encode()).hexdigest()}"
    assert web.mirror_key("Ruby Dreams", "rd-2") == (
        "scfg:Ruby Dreams:" + hashlib.sha256(b"rd-2").hexdigest()
    )
    assert 'Prefix = "scfg:"' in MIRROR.read_text()


def test_the_product_name_is_the_builds():
    assert f'Product = "{web.PRODUCT}"' in BUILD_SCRIPT.read_text()


def test_the_indexeddb_helpers_read_unitys_database():
    for code in (web.IDB_READ, web.IDB_DELETE, web.IDB_LIST):
        assert '"/idbfs"' in code
        assert "/userfs" not in code


def test_the_families():
    assert set(web.FAMILIES) == {"evm", "arc", "solana"}
    config = web.Solana.config.__qualname__
    assert config == "Solana.config"


def test_console_problems_are_errors_warnings_and_page_errors():
    console = [
        ("log", "Loading player"),
        ("warning", "[UnityCache] Response is served without Content-Length header."),
        ("error", "scfg: something"),
        ("pageerror", "TypeError: x"),
        ("info", "fine"),
    ]
    assert web.console_problems(console) == [
        "[UnityCache] Response is served without Content-Length header.",
        "scfg: something",
        "TypeError: x",
    ]
    assert web.console_problems(console, (re.escape("[UnityCache]"),)) == [
        "scfg: something",
        "TypeError: x",
    ]


def test_the_flood_offers_more_than_the_cap():
    assert web.CAP_OFFER > web.CAP_BYTES
    coio = ROOT / "unity" / "Packages" / "com.repulse.scfg" / "Runtime" / "Core" / "Coio.cs"
    assert "MaxResponseBytes = 16 * 1024 * 1024" in coio.read_text()
