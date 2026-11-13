"""The TypeScript and GDScript cores read exactly the decimal digits CPython 3.9 reads (int(), re's
\\d): their tables of digit-run zeros equal this interpreter's unicodedata (Unicode 13.0.0)."""
import re
import sys
import unicodedata
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TABLES = {
    "rpgmaker/src/core/pystr.ts": r"const ND_ZEROS: readonly number\[\] = \[(.*?)\];",
    "godot/addons/scfg/core/pystr.gd": r"const _ND_ZEROS: Array\[int\] = \[(.*?)\]",
}


@pytest.mark.skipif(unicodedata.unidata_version != "13.0.0",
                    reason="the tables are CPython 3.9's (Unicode 13.0.0), the project's interpreter")
@pytest.mark.parametrize("path", sorted(TABLES))
def test_digit_table_is_cpython_39s(path):
    zeros = [c for c in range(sys.maxunicode + 1) if unicodedata.decimal(chr(c), -1) == 0]
    text = (ROOT / path).read_text(encoding="utf-8")
    table = re.search(TABLES[path], text, re.S).group(1)
    assert [int(h, 16) for h in re.findall(r"0x[0-9a-f]+", table)] == zeros
    # every decimal digit is its run's zero plus its value
    assert all(c - unicodedata.decimal(chr(c)) in zeros for c in range(sys.maxunicode + 1)
               if unicodedata.decimal(chr(c), -1) >= 0)
