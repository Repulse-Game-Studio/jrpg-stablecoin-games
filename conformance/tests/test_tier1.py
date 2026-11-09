"""Tier 1 vectors: registry, replay of every committed case, and independent cross-checks."""

from __future__ import annotations

import ast
import json
from pathlib import Path

import pytest

from scfg_conformance import tier1, tier1_config, vectors

ROOT = Path(__file__).resolve().parents[2]
TIER1_DIR = ROOT / "conformance" / "vectors" / "tier1"
TOPICS = ["keccak", "evm_address", "base58", "ed25519", "pda", "solana_tx", "amounts",
          "paylinks", "qr", "page_messages", "receipts", "tx_ids",
          "chains", "config", "custom_chains", "items", "payments"]
USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"


def _all_cases():
    out = []
    for path in sorted(TIER1_DIR.glob("*.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        for i, case in enumerate(doc["cases"]):
            out.append(pytest.param(case, id=f"{path.stem}-{i}-{case['fn']}"))
    return out


def _cases(topic):
    doc = json.loads((TIER1_DIR / f"{topic}.json").read_text(encoding="utf-8"))
    assert doc["tier"] == 1 and doc["topic"] == topic
    return doc["cases"]


def test_tier1_topics_registered():
    vectors.load_all()
    for topic in TOPICS:
        assert f"tier1/{topic}.json" in vectors.SOURCES


def test_tier1_files_exist_with_shape():
    for topic in TOPICS:
        cases = _cases(topic)
        assert cases
        for case in cases:
            assert set(case) in ({"fn", "in", "out"}, {"fn", "in", "error"})
            assert case["fn"] in tier1.FUNCS


@pytest.mark.parametrize("case", _all_cases())
def test_tier1_vectors_replay(case):
    tier1.run_case(case)


def test_run_case_detects_mismatch():
    with pytest.raises(AssertionError):
        tier1.run_case({"fn": "keccak256", "in": {"data_hex": ""}, "out": "00"})
    with pytest.raises(AssertionError):
        tier1.run_case({"fn": "b58decode", "in": {"text": "0"}, "out": ""})


def test_required_case_counts():
    keccak_lengths = {len(c["in"]["data_hex"]) // 2 for c in _cases("keccak")}
    assert {0, 1, 31, 32, 135, 136, 137, 200, 1000} <= keccak_lengths
    assert len([c for c in _cases("ed25519") if c["fn"] == "is_on_curve"]) >= 40 + 10
    pda = _cases("pda")
    assert len([c for c in pda if c["fn"] == "associated_token_address" and "out" in c]) >= 6
    assert any(c["fn"] == "create_program_address" and c.get("out") is None and "out" in c
               for c in pda)
    assert any(c.get("error") == "invalid" for c in pda)


# --- independent cross-checks (not through the code under test) -------------------------


def test_keccak_empty_matches_unit_test_literal():
    literal = "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"
    assert any(c["fn"] == "keccak256" and c["in"]["data_hex"] == "" and c["out"] == literal
               for c in _cases("keccak"))
    unit = (ROOT / "tests" / "unit" / "test_keccak.py").read_text(encoding="utf-8")
    assert literal in unit


def test_eip55_reference_addresses():
    # the EIP-55 specification's own examples
    reference = [
        "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
        "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
        "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
        "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb",
    ]
    cases = _cases("evm_address")
    for addr in reference:
        assert any(c["fn"] == "to_checksum_address" and c["in"]["address"] == addr.lower()
                   and c["out"] == addr for c in cases)


def test_pda_and_curve_match_solders():
    pytest.importorskip("solders.pubkey")
    from solders.pubkey import Pubkey

    from scfg_core.solana.base58 import b58decode

    for case in _cases("pda"):
        i = case["in"]
        if case["fn"] == "associated_token_address" and "out" in case:
            expected, _ = Pubkey.find_program_address(
                [bytes(Pubkey.from_string(i["owner"])),
                 bytes(Pubkey.from_string("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA")),
                 bytes(Pubkey.from_string(i["mint"]))],
                Pubkey.from_string("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"))
            assert case["out"] == str(expected)
        elif case["fn"] == "find_program_address" and "out" in case:
            seeds = [bytes.fromhex(s) for s in i["seeds_hex"]]
            expected, bump = Pubkey.find_program_address(seeds, Pubkey.from_string(i["program_id"]))
            assert case["out"] == {"address": str(expected), "bump": bump}
        elif case["fn"] == "create_program_address" and "out" in case:
            seeds = [bytes.fromhex(s) for s in i["seeds_hex"]]
            try:
                expected = str(Pubkey.create_program_address(
                    seeds, Pubkey.from_string(i["program_id"])))
            except Exception:
                expected = None
            assert case["out"] == expected
    assert any(c["fn"] == "associated_token_address" and c["in"]["mint"] == USDC_MINT
               for c in _cases("pda"))
    for case in _cases("ed25519"):
        raw = bytes.fromhex(case["in"]["point_hex"])
        if len(raw) == 32 and "out" in case:
            assert case["out"] == Pubkey.from_bytes(raw).is_on_curve()
    assert b58decode(USDC_MINT) == bytes(Pubkey.from_string(USDC_MINT))


def test_usdc_mint_is_on_curve_case_present_and_atas_off_curve():
    cases = _cases("ed25519")
    from scfg_core.solana.base58 import b58decode

    usdc = b58decode(USDC_MINT).hex()
    assert any(c["in"]["point_hex"] == usdc for c in cases)
    atas = [c["out"] for c in _cases("pda") if c["fn"] == "associated_token_address"
            and "out" in c]
    off = {b58decode(a).hex() for a in atas}
    assert sum(1 for c in cases if c["in"]["point_hex"] in off and c["out"] is False) >= 2


def test_solana_tx_matches_solders_message():
    pytest.importorskip("solders")
    from solders.hash import Hash
    from solders.instruction import AccountMeta, Instruction
    from solders.message import Message
    from solders.pubkey import Pubkey

    n = 0
    for case in _cases("solana_tx"):
        if case["fn"] != "compile_message" or "out" not in case:
            continue
        i = case["in"]
        ixs = [Instruction(Pubkey.from_string(x["program_id"]),
                           bytes.fromhex(x["data_hex"]),
                           [AccountMeta(Pubkey.from_string(m["pubkey"]), m["is_signer"],
                                        m["is_writable"]) for m in x["accounts"]])
               for x in i["instructions"]]
        msg = Message.new_with_blockhash(ixs, Pubkey.from_string(i["payer"]),
                                         Hash.from_string(i["recent_blockhash"]))
        assert case["out"] == bytes(msg).hex()
        n += 1
    assert n >= 2


def test_compact_u16_known_encodings():
    known = {0: "00", 127: "7f", 128: "8001", 16383: "ff7f", 16384: "808001", 65535: "ffff03"}
    got = {c["in"]["n"]: c["out"] for c in _cases("solana_tx") if c["fn"] == "compact_u16"
           and "out" in c}
    for n, hexed in known.items():
        assert got[n] == hexed



# --- Task 4: amounts, links, QR, page messages, receipts, tx ids, message catalogue -----------


def test_messages_catalogue_matches_checkout():
    from scfg_core.checkout import MESSAGES

    path = ROOT / "conformance" / "vectors" / "messages.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    assert set(doc) == {"messages"}
    assert doc["messages"] == MESSAGES
    assert list(doc["messages"]) == sorted(doc["messages"])
    assert all(isinstance(v, str) and v for v in doc["messages"].values())
    vectors.load_all()
    assert "messages.json" in vectors.SOURCES


def _fn_cases(topic, fn):
    return [c for c in _cases(topic) if c["fn"] == fn]


def test_amounts_required_inputs():
    parse = {json.dumps(c["in"], sort_keys=True): c for c in _fn_cases("amounts", "parse_amount")}
    for text in ["4.99", " 4.99 ", "5", "0.001", "0", "-1", "1000000", "1000000.01", "NaN",
                 "Infinity", "abc", "1e2"]:
        assert json.dumps({"type": "str", "value": text}, sort_keys=True) in parse, text
    assert parse[json.dumps({"type": "int", "value": 5}, sort_keys=True)]["out"] == "5.00"
    assert parse[json.dumps({"type": "float"}, sort_keys=True)]["error"] == "invalid"
    for code_nonce in (0, 99):
        assert any(c["in"]["code"] == code_nonce and "out" in c
                   for c in _fn_cases("amounts", "encode"))
    assert any(c["in"]["code"] == 100 and c.get("error") == "invalid"
               for c in _fn_cases("amounts", "encode"))
    assert any(c["in"]["nonce"] == 100 and c.get("error") == "invalid"
               for c in _fn_cases("amounts", "encode"))
    outs = {c["in"]["units"]: c["out"] for c in _fn_cases("amounts", "units_to_decimal")}
    assert outs["0"] == "0.000000"
    assert outs["4990000"] == "4.990000"
    assert outs[str(10**12)] == "1000000.000000"
    assert outs[str(10**18 // 10**12)] == "1.000000"
    assert any(c.get("error") == "invalid" for c in _fn_cases("amounts", "decimal_to_units"))
    assert _fn_cases("amounts", "format_units")


def test_paylinks_uri_encoding():
    cases = _fn_cases("paylinks", "solana_pay_uri")
    joined = " ".join(c["out"] for c in cases)
    for fragment in ("%20", "%26", "%3F", "%C3%A9", "%E6%97%A5%E6%9C%AC", "%F0%9F"):
        assert fragment in joined, fragment
    assert _fn_cases("paylinks", "eip681_transfer_uri")


def test_qr_inputs_and_shapes():
    cases = _fn_cases("qr", "qr_matrix")
    assert len(cases) == 6
    sizes = {len(c["in"]["text"]): len(c["out"]) for c in cases}
    for c in cases:
        n = len(c["out"])
        assert n >= 21 and (n - 17) % 4 == 0
        assert all(len(row) == n and set(row) <= {"0", "1"} for row in c["out"])
    assert 1 in sizes and sizes[1] == 21
    # byte mode at ECC MEDIUM holds 14 bytes in version 1 (21 modules) and 15 in version 2 (25)
    texts = {c["in"]["text"]: len(c["out"]) for c in cases}
    boundary = [t for t in texts if t.isascii() and len(t) == 15]
    assert boundary and texts[boundary[0]] == 25
    assert len(_fn_cases("qr", "qr_rects")) == 2
    for c in _fn_cases("qr", "qr_rects"):
        assert c["in"]["border"] == 2
        assert c["out"]["size"] == len(
            next(m["out"] for m in cases if m["in"]["text"] == c["in"]["text"])) + 4


def test_page_messages_cases():
    tx = _fn_cases("page_messages", "parse_tx_message")
    build = _fn_cases("page_messages", "parse_build_message")
    assert any("out" in c for c in tx) and any(c.get("error") == "invalid" for c in tx)
    assert any("out" in c for c in build) and any(c.get("error") == "invalid" for c in build)


def test_receipts_cases():
    own = _fn_cases("receipts", "compute_ownership")
    assert any(c["out"]["tip_total"] != "0" and c["out"]["owned"] for c in own)
    assert _fn_cases("receipts", "receipt_key") and _fn_cases("receipts", "make_receipt")


def test_tx_ids_cases():
    for fn in ("normalize_tx_hash", "normalize_signature", "normalize_any_tx"):
        cs = _fn_cases("tx_ids", fn)
        assert any("out" in c for c in cs) and any("error" in c for c in cs), fn


def test_amount_codec_matches_core_directly():
    from scfg_core.amounts import encode

    for c in _fn_cases("amounts", "encode"):
        if "out" in c:
            i = c["in"]
            assert encode(i["price"], i["code"], i["nonce"]) == int(c["out"])


# --- configuration vectors -------------------------------------------------------------------

CORE = ROOT / "plugin" / "game" / "python-packages" / "scfg_core"


def _source_codes():
    codes = set()
    for name in ("config.py", "payments.py", "engine.py"):
        for node in ast.walk(ast.parse((CORE / name).read_text(encoding="utf-8"))):
            if isinstance(node, ast.Call) and getattr(node.func, "id", "") == "ConfigError":
                for kw in node.keywords:
                    if kw.arg == "code":
                        codes.add(kw.value.value)
    return codes


def test_config_vectors_cover_every_code():
    covered = set()
    for topic in ("config", "custom_chains", "items", "payments"):
        covered |= {c["error"] for c in _cases(topic) if "error" in c}
    source = _source_codes()
    unreachable = set(tier1_config.UNREACHABLE_CODES)
    assert unreachable <= source, "stale UNREACHABLE_CODES entries"
    assert all(tier1_config.UNREACHABLE_CODES.values())
    assert not unreachable & covered
    assert source - unreachable - covered == set()
    assert covered - {"invalid"} <= source


def test_chains_vectors_are_the_registry():
    from scfg_conformance.canonical import to_jsonable
    from scfg_core.chains import MAINNET, TESTNET
    cases = _cases("chains")
    assert [c["in"] for c in cases] == [{"testnet": False}, {"testnet": True}]
    assert cases[0]["out"] == to_jsonable(MAINNET)
    assert cases[1]["out"] == to_jsonable(TESTNET)
    assert cases[0]["out"]["arc"]["tokens"]["USDC"]["log_decimals"] == 18
    assert cases[0]["out"]["arc"]["min_fee_wei"] == 20_000_000_000


def test_config_valid_cases_present():
    outs = [c["out"] for c in _cases("config") if "out" in c]
    assert any(o["wallet"] and not o["solana_wallet"] for o in outs)
    assert any(o["solana_wallet"] and not o["wallet"] for o in outs)
    assert any(o["wallet"] and o["solana_wallet"] for o in outs)
    assert {len(o["accent"]) for o in outs} == {7}
    assert any(o["testnet"] for o in outs)
    assert any(o["logo"] == "images/logo.png" for o in outs)
    assert any(o["previous_wallets"] and o["previous_solana_wallets"] for o in outs)


def test_payment_cases_include_float_and_unknown_item():
    errs = {c.get("error") for c in _cases("payments")}
    assert {"tip.below_minimum", "item.unknown", "token.not_enabled"} <= errs
    assert any(isinstance(c["in"].get("amount"), dict) for c in _cases("payments"))
