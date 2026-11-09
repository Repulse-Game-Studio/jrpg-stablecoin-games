"""Tier 1 vectors: pure function tables replayed by every core.

File shape: {"tier": 1, "topic": str, "cases": [{"fn", "in", "out"} | {"fn", "in", "error"}]}.
`fn` is the Python function name; JS and GDScript map it to their own case style. `error` is
"invalid" for ValueError/TypeError and the ConfigError code otherwise. Bytes travel as
lowercase hex in `*_hex` fields, base58 strings stay strings, and any int that could reach
2**53 is a decimal string.
"""

from __future__ import annotations

import hashlib
import random
from decimal import Decimal
from typing import Any, Callable

from scfg_core import amounts, evm, page_messages, paylinks, receipts, sessions
from scfg_core.backends import normalize_any_tx
from scfg_core.chains import ARC_SYSTEM_EMITTER, ARC_USDC
from scfg_core.checkout import MESSAGES
from scfg_core.errors import ConfigError
from scfg_core.keccak import keccak256
from scfg_core.models import Item
from scfg_core.solana import base58, pda, tx
from scfg_core.solana.ed25519 import D, P, is_on_curve
from scfg_core.transfers import Transfer

SOURCES: dict[str, Callable[[], dict]] = {}
FUNCS: dict[str, Callable[[dict], Any]] = {}


def _fn(name):
    def deco(f):
        FUNCS[name] = f
        return f
    return deco


# --- adapters: JSON-friendly `in` -> Python args -> JSON-friendly `out` ---------------------


@_fn("keccak256")
def _keccak256(i):
    return keccak256(bytes.fromhex(i["data_hex"])).hex()


@_fn("to_checksum_address")
def _to_checksum_address(i):
    return evm.to_checksum_address(i["address"])


@_fn("is_checksum_address")
def _is_checksum_address(i):
    return evm.is_checksum_address(i["address"])


@_fn("normalize_address")
def _normalize_address(i):
    return evm.normalize_address(i["address"])


@_fn("address_topic")
def _address_topic(i):
    return evm.address_topic(i["address"])


@_fn("parse_transfer_log")
def _parse_transfer_log(i):
    t = evm.parse_transfer_log(i["log"])
    if t is None:
        return None
    return {"token": t.token, "sender": t.sender, "recipient": t.recipient,
            "value": str(t.value), "tx_hash": t.tx_hash, "log_index": t.log_index,
            "block_number": t.block_number, "block_hash": t.block_hash}


@_fn("b58encode")
def _b58encode(i):
    return base58.b58encode(bytes.fromhex(i["data_hex"]))


@_fn("b58decode")
def _b58decode(i):
    return base58.b58decode(i["text"]).hex()


@_fn("decode_fixed")
def _decode_fixed(i):
    return base58.decode_fixed(i["text"], i["length"]).hex()


@_fn("normalize_pubkey")
def _normalize_pubkey(i):
    return base58.normalize_pubkey(i["text"])


@_fn("normalize_signature")
def _normalize_signature(i):
    return base58.normalize_signature(i["text"])


@_fn("is_on_curve")
def _is_on_curve(i):
    return is_on_curve(bytes.fromhex(i["point_hex"]))


def _seeds(i):
    return [bytes.fromhex(s) for s in i["seeds_hex"]]


@_fn("create_program_address")
def _create_program_address(i):
    out = pda.create_program_address(_seeds(i), base58.decode_fixed(i["program_id"], 32))
    return None if out is None else base58.b58encode(out)


@_fn("find_program_address")
def _find_program_address(i):
    address, bump = pda.find_program_address(_seeds(i), base58.decode_fixed(i["program_id"], 32))
    return {"address": base58.b58encode(address), "bump": bump}


@_fn("associated_token_address")
def _associated_token_address(i):
    return pda.associated_token_address(i["owner"], i["mint"])


@_fn("compact_u16")
def _compact_u16(i):
    return tx.compact_u16(i["n"]).hex()


@_fn("compile_message")
def _compile_message(i):
    ixs = [tx.Instruction(x["program_id"],
                          tuple(tx.AccountMeta(m["pubkey"], m["is_signer"], m["is_writable"])
                                for m in x["accounts"]),
                          bytes.fromhex(x["data_hex"])) for x in i["instructions"]]
    return tx.compile_message(i["payer"], ixs, i["recent_blockhash"]).hex()


@_fn("unsigned_transaction")
def _unsigned_transaction(i):
    return tx.unsigned_transaction(bytes.fromhex(i["message_hex"])).hex()


# amounts


@_fn("parse_amount")
def _parse_amount(i):
    kind = i["type"]
    if kind == "float":
        value = 4.99
    elif kind == "bool":
        value = bool(i["value"])
    else:  # "str" or "int": the JSON value itself
        value = i["value"]
    return str(amounts.parse_amount(value))


@_fn("encode")
def _encode(i):
    return str(amounts.encode(i["price"], i["code"], i["nonce"]))


@_fn("decode")
def _decode(i):
    price, code, nonce = amounts.decode(int(i["units"]))
    return {"price": str(price), "code": code, "nonce": nonce}


@_fn("units_to_decimal")
def _units_to_decimal(i):
    return str(amounts.units_to_decimal(int(i["units"])))


@_fn("decimal_to_units")
def _decimal_to_units(i):
    return str(amounts.decimal_to_units(Decimal(i["amount"])))


@_fn("format_units")
def _format_units(i):
    return amounts.format_units(int(i["units"]))


# payment links and QR


@_fn("eip681_transfer_uri")
def _eip681_transfer_uri(i):
    return paylinks.eip681_transfer_uri(token_address=i["token_address"],
                                        chain_id=i["chain_id"], recipient=i["recipient"],
                                        units=int(i["units"]))


@_fn("solana_pay_uri")
def _solana_pay_uri(i):
    from scfg_core.solana.paylinks import solana_pay_uri

    return solana_pay_uri(recipient=i["recipient"], units=int(i["units"]), mint=i["mint"],
                          reference=i["reference"], label=i["label"], message=i["message"])


@_fn("qr_matrix")
def _qr_matrix(i):
    return ["".join("1" if m else "0" for m in row) for row in paylinks.qr_matrix(i["text"])]


@_fn("qr_rects")
def _qr_rects(i):
    size, runs = paylinks.qr_rects(i["text"], i["border"])
    return {"size": size, "rects": [list(r) for r in runs]}


# checkout page messages


@_fn("parse_tx_message")
def _parse_tx_message(i):
    tx_hash, sender = page_messages.parse_tx_message(i["data"])
    return [tx_hash, sender]


@_fn("parse_build_message")
def _parse_build_message(i):
    return page_messages.parse_build_message(i["data"])


# receipts


@_fn("receipt_key")
def _receipt_key(i):
    return receipts.receipt_key(i["chain"], i["tx_hash"], i["log_index"])


def _transfer(t):
    fields = {k: v for k, v in t.items() if k != "class"}
    fields["value"] = int(fields["value"])
    if t["class"] == "TransferLog":
        return evm.TransferLog(**fields)
    if t["class"] == "Transfer":
        return Transfer(**fields)
    raise ValueError(f"unknown transfer class {t['class']!r}")


@_fn("make_receipt")
def _make_receipt(i):
    return receipts.make_receipt(chain=i["chain"], transfer=_transfer(i["transfer"]),
                                 kind=i["kind"], item=i["item"], token=i["token"],
                                 source=i["source"])


@_fn("add_receipt")
def _add_receipt(i):
    store = dict(i["store"])
    added = receipts.add_receipt(store, i["receipt"])
    return {"added": added, "store": store}


@_fn("remove_receipt")
def _remove_receipt(i):
    store = dict(i["store"])
    receipts.remove_receipt(store, i["key"])
    return store


@_fn("compute_ownership")
def _compute_ownership(i):
    items = {x["item_id"]: Item(item_id=x["item_id"], code=x["code"],
                                price=Decimal(x["price"]), token=x["token"], title=x["title"],
                                min_price=Decimal(x["min_price"])) for x in i["items"]}
    owned, tips = receipts.compute_ownership(i["store"], items)
    return {"owned": sorted(owned), "tip_total": str(tips)}


# transaction ids (normalize_signature is registered with base58)


@_fn("normalize_tx_hash")
def _normalize_tx_hash(i):
    return sessions.normalize_tx_hash(i["text"])


@_fn("normalize_any_tx")
def _normalize_any_tx(i):
    return normalize_any_tx(i["text"])


# --- runner ---------------------------------------------------------------------------------


def _compute(fn: str, inputs: dict) -> dict:
    try:
        return {"out": FUNCS[fn](inputs)}
    except ConfigError as e:
        return {"error": e.code}
    except (ValueError, TypeError):
        return {"error": "invalid"}


def run_case(case: dict) -> None:
    """Recompute a case through FUNCS and assert it equals the vector."""
    got = _compute(case["fn"], case["in"])
    want = {k: case[k] for k in ("out", "error") if k in case}
    assert got == want, f"{case['fn']}({case['in']!r}): got {got!r}, vector says {want!r}"


def topic(name: str, cases: list[tuple[str, dict]]) -> None:
    """Register tier1/<name>.json; outputs are computed through FUNCS at generation time."""
    def build() -> dict:
        return {"tier": 1, "topic": name,
                "cases": [{"fn": fn, "in": inputs, **_compute(fn, inputs)}
                          for fn, inputs in cases]}
    SOURCES[f"tier1/{name}.json"] = build


# --- shared fixtures ------------------------------------------------------------------------

USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
DEVNET_USDC_MINT = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU"
SYSTEM_PROGRAM = "11111111111111111111111111111111"
TOKEN_PROGRAM = pda.TOKEN_PROGRAM_ID
ATA_PROGRAM = pda.ASSOCIATED_TOKEN_PROGRAM_ID
COMPUTE_BUDGET = tx.COMPUTE_BUDGET_PROGRAM_ID


def _key(label: str) -> str:
    """A deterministic, well-formed 32-byte base58 address for a label."""
    return base58.b58encode(hashlib.sha256(label.encode("utf-8")).digest())


def _hex(n: int, fill: int = 0xAB) -> str:
    return bytes([(fill + k) % 256 for k in range(n)]).hex()


# --- keccak ---------------------------------------------------------------------------------


def _keccak_cases():
    cases = [("keccak256", {"data_hex": _hex(n, 7)}) for n in (0, 1, 31, 32, 135, 136, 137,
                                                                 200, 1000)]
    cases.append(("keccak256", {"data_hex": b"Transfer(address,address,uint256)".hex()}))
    cases.append(("keccak256", {"data_hex": b"abc".hex()}))
    cases.append(("keccak256", {"data_hex": ("ff" * 136)}))
    cases.append(("keccak256", {"data_hex": ("00" * 272)}))
    return cases


topic("keccak", _keccak_cases())

# --- evm_address ----------------------------------------------------------------------------

_EIP55 = [
    "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
    "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
    "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
    "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb",
]
_USDC_ETH = "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"
_ZERO_ADDR = "0x" + "00" * 20


def _flip_case(addr: str) -> str:
    """Valid mixed-case address with the checksum broken (first letter's case flipped)."""
    body = list(addr[2:])
    for k, c in enumerate(body):
        if c.isalpha():
            body[k] = c.swapcase()
            break
    return "0x" + "".join(body)


def _tlog(**kw):
    log = {
        "address": _USDC_ETH.lower(),
        "topics": [evm.TRANSFER_TOPIC,
                   evm.address_topic(_EIP55[0]), evm.address_topic(_EIP55[1])],
        "data": "0x" + f"{4_990_000:064x}",
        "transactionHash": "0x" + "ab" * 32,
        "logIndex": "0x7",
        "blockNumber": "0x12d687",
        "blockHash": "0x" + "CD" * 32,
        "removed": False,
    }
    log.update(kw)
    return log


def _evm_cases():
    addrs = _EIP55 + [_USDC_ETH, ARC_USDC, ARC_SYSTEM_EMITTER, _ZERO_ADDR]
    cases = []
    for a in addrs:
        for form in (a.lower(), "0x" + a[2:].upper()):
            cases.append(("to_checksum_address", {"address": form}))
    for a in _EIP55[:2]:
        cases.append(("to_checksum_address", {"address": a}))
        cases.append(("to_checksum_address", {"address": "0X" + a[2:]}))
    bad = ["", "0x", _EIP55[0][2:], _EIP55[0][:-1], _EIP55[0] + "0",
           "0x" + "zz" * 20, "0x" + "g" + "0" * 39, " " + _EIP55[0], _EIP55[0] + "\n"]
    for b in bad:
        cases.append(("to_checksum_address", {"address": b}))
    cases.append(("to_checksum_address", {"address": 5}))
    cases.append(("to_checksum_address", {"address": None}))

    check_inputs = ([a for a in _EIP55] + [a.lower() for a in _EIP55]
                    + ["0x" + a[2:].upper() for a in _EIP55] + [_flip_case(a) for a in _EIP55]
                    + [_EIP55[0][2:], _EIP55[0][:-1], _EIP55[0] + "0", "0x" + "zz" * 20, "", 5])
    for a in check_inputs:
        cases.append(("is_checksum_address", {"address": a}))

    norm_inputs = ([_EIP55[0].lower(), "0x" + _EIP55[1][2:].upper(), _EIP55[2], _EIP55[3],
                    _ZERO_ADDR, ARC_SYSTEM_EMITTER, ARC_SYSTEM_EMITTER.lower()]
                   + [_flip_case(a) for a in _EIP55[:3]]
                   + [_EIP55[0][2:], _EIP55[0][:-1], _EIP55[0] + "0", "0x" + "zz" * 20,
                      "0x" + "12345678901234567890123456789012345678gh", "", 7])
    for a in norm_inputs:
        cases.append(("normalize_address", {"address": a}))

    for a in [_EIP55[0], _EIP55[0].lower(), _USDC_ETH, ARC_SYSTEM_EMITTER, _ZERO_ADDR,
              _EIP55[1][2:], "0x1234"]:
        cases.append(("address_topic", {"address": a}))

    # parse_transfer_log: valid, topic0 case, wrong topic0, topic counts, data edge cases
    cases.append(("parse_transfer_log", {"log": _tlog()}))
    cases.append(("parse_transfer_log", {"log": _tlog(
        topics=[evm.TRANSFER_TOPIC.upper().replace("0X", "0x")] + _tlog()["topics"][1:])}))
    cases.append(("parse_transfer_log", {"log": _tlog(topics=[
        "0x" + "11" * 32] + _tlog()["topics"][1:])}))
    cases.append(("parse_transfer_log", {"log": _tlog(topics=_tlog()["topics"][:2])}))
    cases.append(("parse_transfer_log", {"log": _tlog(topics=_tlog()["topics"][:1])}))
    cases.append(("parse_transfer_log", {"log": _tlog(topics=[])}))
    cases.append(("parse_transfer_log", {"log": _tlog(
        topics=_tlog()["topics"] + ["0x" + "00" * 31 + "05"])}))  # ERC-721 shape
    cases.append(("parse_transfer_log", {"log": _tlog(removed=True)}))
    cases.append(("parse_transfer_log", {"log": _tlog(data="0x")}))
    cases.append(("parse_transfer_log", {"log": _tlog(data="")}))
    cases.append(("parse_transfer_log", {"log": _tlog(data="0x01")}))  # short data
    cases.append(("parse_transfer_log", {"log": _tlog(data="0x" + "00" * 12 + "ff" * 20)}))
    cases.append(("parse_transfer_log", {"log": _tlog(data="0xzz")}))
    cases.append(("parse_transfer_log", {"log": _tlog(address="0x1234")}))
    cases.append(("parse_transfer_log", {"log": _tlog(
        address=_USDC_ETH.upper().replace("0X", "0x"))}))
    cases.append(("parse_transfer_log", {"log": _tlog(logIndex="0x0", blockNumber="0x0")}))
    # Arc system emitter: native USDC transfer, 18-decimal data (1.2300179 USDC)
    cases.append(("parse_transfer_log", {"log": _tlog(
        address=ARC_SYSTEM_EMITTER, data="0x" + f"{1_230_017_900_000_000_000:064x}",
        logIndex="0x4", blockNumber="0x3e6")}))
    cases.append(("parse_transfer_log", {"log": _tlog(
        address=ARC_SYSTEM_EMITTER, data="0x" + f"{10**18:064x}")}))
    cases.append(("parse_transfer_log", {"log": _tlog(
        address=ARC_SYSTEM_EMITTER, data="0x" + f"{2**256 - 1:064x}")}))
    # the ERC-20 interface log Arc emits alongside it (6 decimals)
    cases.append(("parse_transfer_log", {"log": _tlog(address=ARC_USDC)}))
    return cases


topic("evm_address", _evm_cases())

# --- base58 ---------------------------------------------------------------------------------


def _b58_cases():
    cases = []
    datas = ["", "00", "0000", "01", "00" + "01", "ff", "0000000000" + "01", "00" * 5,
             "00" * 32, _hex(32, 1), "ff" * 32, "00" + _hex(31, 9), _hex(64, 3), "00" * 64,
             "00" + "ff" * 63, "0001", "61", "626262", "516b6fcd0f"]
    for d in datas:
        cases.append(("b58encode", {"data_hex": d}))
    encoded = [SYSTEM_PROGRAM, USDC_MINT, TOKEN_PROGRAM, "1", "11", "111112", "2", "z",
               "zzzzzzzz", "Cn8eVZg", "11111", "1" * 5 + "2", _key("sig-seed")]
    for t in encoded:
        cases.append(("b58decode", {"text": t}))
    for ch in "0OIl":
        cases.append(("b58decode", {"text": "abc" + ch + "def"}))
        cases.append(("b58decode", {"text": ch}))
    cases += [("b58decode", {"text": ""}), ("b58decode", {"text": " "}),
              ("b58decode", {"text": "abc def"}), ("b58decode", {"text": "é"}),
              ("b58decode", {"text": "+/="}), ("b58decode", {"text": 5}),
              ("b58decode", {"text": None})]
    # a 64-byte signature-shaped value round trips
    sig = base58.b58encode(bytes(range(64)))
    cases.append(("b58decode", {"text": sig}))
    cases.append(("b58decode", {"text": base58.b58encode(b"\0" * 64)}))
    for text, length in [(USDC_MINT, 32), (SYSTEM_PROGRAM, 32), (sig, 64), (USDC_MINT, 31),
                         (USDC_MINT, 33), (USDC_MINT, 64), (sig, 32), (SYSTEM_PROGRAM, 31),
                         ("1", 1), ("1", 0), ("", 32), ("0" + USDC_MINT[1:], 32)]:
        cases.append(("decode_fixed", {"text": text, "length": length}))
    pk_inputs = [USDC_MINT, "  " + USDC_MINT + "\n", SYSTEM_PROGRAM, TOKEN_PROGRAM,
                 USDC_MINT[:-1], USDC_MINT + "1", sig, "", "0", 12, None,
                 "\t" + _key("owner") + " "]
    for t in pk_inputs:
        cases.append(("normalize_pubkey", {"text": t}))
    sig_inputs = [sig, " " + sig + " ", base58.b58encode(b"\0" * 64),
                  base58.b58encode(b"\xff" * 64), USDC_MINT, sig[:-1], sig + "1", "", "O", 3, None]
    for t in sig_inputs:
        cases.append(("normalize_signature", {"text": t}))
    return cases


topic("base58", _b58_cases())

# --- ed25519 --------------------------------------------------------------------------------

_MASK255 = (1 << 255) - 1


def _le32(n: int) -> str:
    return n.to_bytes(32, "little").hex()


def _find_on_curve(label: str) -> bytes:
    n = 0
    while True:
        cand = hashlib.sha256(f"{label}/{n}".encode()).digest()
        if is_on_curve(cand):
            return cand
        n += 1


def _ed_cases():
    pts = []
    rng = random.Random("scfg-conformance/ed25519/random/v1")
    for _ in range(40):
        pts.append(rng.getrandbits(256).to_bytes(32, "big").hex())
    pts.append(base58.b58decode(USDC_MINT).hex())
    pts.append(base58.b58decode(DEVNET_USDC_MINT).hex())
    pts.append(base58.b58decode(TOKEN_PROGRAM).hex())
    pts.append(base58.b58decode(SYSTEM_PROGRAM).hex())
    # two known off-curve points: associated token accounts are PDAs, so off-curve
    for owner in (_key("ed25519/owner-a"), _key("ed25519/owner-b")):
        pts.append(base58.b58decode(pda.associated_token_address(owner, USDC_MINT)).hex())
    pts += ["00" * 32, "ff" * 32, _le32(1), _le32(0), _le32(2), _le32(P - 1), _le32(P),
            _le32(P + 1), _le32(P + 2), _le32(P + 3), _le32(P + 18), _le32(_MASK255),
            _le32(1 | (1 << 255)),
            _le32(P | (1 << 255)), _le32((P + 1) | (1 << 255)), _le32(P - 1 | (1 << 255)),
            _le32(D % P), _le32(pow(2, 254, P))]
    # sign bit on an on-curve point (accepted: the bit only picks the x sign)
    on = _find_on_curve("ed25519/on-curve")
    pts.append(on.hex())
    flipped = bytearray(on)
    flipped[31] ^= 0x80
    pts.append(bytes(flipped).hex())
    on2 = _find_on_curve("ed25519/on-curve-2")
    pts.append(on2.hex())
    f2 = bytearray(on2)
    f2[31] |= 0x80
    pts.append(bytes(f2).hex())
    # lengths other than 32
    pts += ["", "00", "00" * 31, "01" + "00" * 30, on.hex()[:-2], on.hex() + "00",
            "00" * 33, on.hex() + on.hex()]
    return [("is_on_curve", {"point_hex": p}) for p in pts]


topic("ed25519", _ed_cases())

# --- pda ------------------------------------------------------------------------------------


def _pda_cases():
    cases = []
    prog = ATA_PROGRAM
    cases_seeds = [[], [""], ["00"], [b"vault".hex()], [b"vault".hex(), "ff"],
                   [_hex(32, 1)], [_hex(32, 1), _hex(32, 2), _hex(32, 3)],
                   ["", "", ""], [b"a".hex()] * 16]
    # search for deterministic seeds giving on-curve (None) and off-curve results
    nulls, offs = [], []
    n = 0
    while len(nulls) < 3 or len(offs) < 3:
        seeds = [b"probe".hex(), n.to_bytes(2, "big").hex()]
        r = pda.create_program_address([bytes.fromhex(s) for s in seeds],
                                       base58.decode_fixed(prog, 32))
        (nulls if r is None else offs).append(seeds)
        n += 1
    cases_seeds += nulls[:3] + offs[:3]
    for program in (prog, TOKEN_PROGRAM, SYSTEM_PROGRAM, _key("pda/program")):
        for seeds in cases_seeds:
            cases.append(("create_program_address", {"seeds_hex": seeds, "program_id": program}))
    # invalid: too many seeds, long seed, bad program id
    cases += [
        ("create_program_address", {"seeds_hex": [b"a".hex()] * 17, "program_id": prog}),
        ("create_program_address", {"seeds_hex": ["00" * 33], "program_id": prog}),
        ("create_program_address", {"seeds_hex": ["00" * 32, "00" * 33], "program_id": prog}),
        ("create_program_address", {"seeds_hex": ["00" * 32], "program_id": prog}),
        ("create_program_address", {"seeds_hex": [], "program_id": "abc"}),
        ("create_program_address", {"seeds_hex": [], "program_id": ""}),
    ]
    find_seeds = [[], [""], [b"vault".hex()], [b"config".hex(), _hex(32, 5)],
                  [_hex(32, 1), _hex(32, 2), _hex(32, 3)], [b"a".hex()] * 15,
                  [b"seed".hex(), "00"], ["00" * 32]]
    for program in (prog, TOKEN_PROGRAM, SYSTEM_PROGRAM, _key("pda/program")):
        for seeds in find_seeds:
            cases.append(("find_program_address", {"seeds_hex": seeds, "program_id": program}))
    # a spread of seeds so several different bumps show up
    for k in range(12):
        cases.append(("find_program_address", {"seeds_hex": [b"bump".hex(), bytes([k]).hex()],
                                               "program_id": prog}))
    cases += [
        ("find_program_address", {"seeds_hex": [b"a".hex()] * 16, "program_id": prog}),
        ("find_program_address", {"seeds_hex": [b"a".hex()] * 17, "program_id": prog}),
        ("find_program_address", {"seeds_hex": ["00" * 33], "program_id": prog}),
        ("find_program_address", {"seeds_hex": [], "program_id": "0"}),
    ]
    pairs = [
        (_key("ata/owner-1"), USDC_MINT),
        (_key("ata/owner-2"), USDC_MINT),
        (SYSTEM_PROGRAM, USDC_MINT),
        (_key("ata/owner-1"), DEVNET_USDC_MINT),
        (TOKEN_PROGRAM, USDC_MINT),
        (_key("ata/owner-3"), _key("ata/mint-3")),
        (USDC_MINT, USDC_MINT),
        # the two off-curve points the ed25519 topic checks
        (_key("ed25519/owner-a"), USDC_MINT),
        (_key("ed25519/owner-b"), USDC_MINT),
    ]
    for owner, mint in pairs:
        cases.append(("associated_token_address", {"owner": owner, "mint": mint}))
    cases += [
        ("associated_token_address", {"owner": "0" + USDC_MINT[1:], "mint": USDC_MINT}),
        ("associated_token_address", {"owner": USDC_MINT[:-1], "mint": USDC_MINT}),
        ("associated_token_address", {"owner": _key("ata/owner-1"), "mint": ""}),
    ]
    return cases


topic("pda", _pda_cases())

# --- solana_tx ------------------------------------------------------------------------------


def _meta(pubkey, signer, writable):
    return {"pubkey": pubkey, "is_signer": signer, "is_writable": writable}


def _ix(program_id, accounts, data_hex):
    return {"program_id": program_id, "accounts": accounts, "data_hex": data_hex}


def _tx_cases():
    cases = [("compact_u16", {"n": n}) for n in (0, 1, 127, 128, 255, 256, 16383, 16384,
                                                  65534, 65535)]
    cases += [("compact_u16", {"n": n}) for n in (-1, 65536, 100000)]
    cases.append(("compact_u16", {"n": "1"}))

    payer = _key("tx/payer")
    dest_owner = _key("tx/dev-wallet")
    reference = _key("tx/reference")
    source = pda.associated_token_address(payer, USDC_MINT)
    dest = pda.associated_token_address(dest_owner, USDC_MINT)
    blockhash = _key("tx/blockhash")
    units = 4_990_000

    def limit(u):
        return _ix(COMPUTE_BUDGET, [], tx.set_compute_unit_limit(u).data.hex())

    def price(p):
        return _ix(COMPUTE_BUDGET, [], tx.set_compute_unit_price(p).data.hex())

    def transfer(amount, decimals=6, extra=()):
        ix = tx.transfer_checked(source=source, mint=USDC_MINT, destination=dest, owner=payer,
                                 amount=amount, decimals=decimals, extra=extra)
        return _ix(ix.program_id,
                   [_meta(m.pubkey, m.is_signer, m.is_writable) for m in ix.accounts],
                   ix.data.hex())

    payments = [
        # the shape build_payment makes
        (payer, [limit(30_000), price(1_000), transfer(units, 6, (reference,))], blockhash),
        (payer, [limit(30_000), price(50_000), transfer(1, 6, (reference,))], blockhash),
        (payer, [limit(30_000), price(1_000), transfer(units * 1000, 6, (reference,))],
         _key("tx/blockhash-2")),
        # without the reference key
        (payer, [limit(30_000), price(1_000), transfer(units)], blockhash),
        # transfer_checked alone
        (payer, [transfer(units)], blockhash),
        (payer, [transfer(units, 6, (reference,))], blockhash),
        (payer, [transfer(2**64 - 1, 9, (reference, _key("tx/ref2")))], blockhash),
        # a payer that also appears in the account list as a writable signer, the other
        # sort groups, and no instructions
        (dest, [transfer(units)], blockhash),
        (payer, [], blockhash),
    ]
    for p, ixs, bh in payments:
        cases.append(("compile_message", {"payer": p, "instructions": ixs,
                                          "recent_blockhash": bh}))
    cases += [
        ("compile_message", {"payer": "abc", "instructions": [], "recent_blockhash": blockhash}),
        ("compile_message", {"payer": payer, "instructions": [], "recent_blockhash": "abc"}),
        ("compile_message", {"payer": payer, "instructions": [transfer(units)],
                             "recent_blockhash": ""}),
    ]
    messages = [FUNCS["compile_message"]({"payer": p, "instructions": ixs,
                                          "recent_blockhash": bh}) for p, ixs, bh in payments]
    for m in messages:
        cases.append(("unsigned_transaction", {"message_hex": m}))
    cases += [("unsigned_transaction", {"message_hex": "02" + "00" * 5}),
              ("unsigned_transaction", {"message_hex": "00"}),
              ("unsigned_transaction", {"message_hex": "zz"})]
    return cases


topic("solana_tx", _tx_cases())


# --- amounts --------------------------------------------------------------------------------


def _amount_cases():
    cases = []
    for text in ["4.99", " 4.99 ", "5", "0.001", "0", "-1", "1000000", "1000000.01", "NaN",
                 "Infinity", "abc", "1e2", "", "   ", "0.01", "0.005", "0.10", "1.5", "+5",
                 "5.", ".5", "1_000", "1,000", "-0", "0.00", "999999.99", "-Infinity", "sNaN",
                 "4.990", "4.999", "007", "1E+1", "5\n", "\u00a05"]:
        cases.append(("parse_amount", {"type": "str", "value": text}))
    for n in (5, 0, -1, 1, 1_000_000, 1_000_001, 12):
        cases.append(("parse_amount", {"type": "int", "value": n}))
    cases.append(("parse_amount", {"type": "float"}))
    cases.append(("parse_amount", {"type": "bool", "value": True}))

    for price, code, nonce in [("4.99", 0, 0), ("4.99", 99, 99), ("4.99", 37, 42),
                               ("0.01", 1, 0), ("1000000", 99, 99), ("1000000", 0, 0),
                               ("5", 5, 5), ("0.50", 0, 7), ("999999.99", 99, 99)]:
        cases.append(("encode", {"price": price, "code": code, "nonce": nonce}))
    for price, code, nonce in [("4.99", 100, 0), ("4.99", 0, 100), ("4.99", -1, 0),
                               ("4.99", 0, -1), ("4.99", True, 0), ("4.99", 0, True),
                               ("0", 1, 1), ("-1", 1, 1), ("1000000.01", 1, 1), ("abc", 1, 1),
                               ("4.999", 1, 1), ("4.99", "1", 0), ("4.99", 0, None)]:
        cases.append(("encode", {"price": price, "code": code, "nonce": nonce}))
    for units in ("0", "99", "100", "9999", "10000", "4993742", "4990000", "1000000009999",
                  str(10**12), "999999999999", "1", "123456789"):
        cases.append(("decode", {"units": units}))
    for units in ("-1", "abc", "", "1.5"):
        cases.append(("decode", {"units": units}))

    for units in ("0", "1", "4990000", str(10**12), str(10**18 // 10**12), "999999",
                  "1000000", "4993742", "10", "123", str(10**18), str(2**64), "-1", "-5000000"):
        cases.append(("units_to_decimal", {"units": units}))
    for amount in ("0", "1", "4.99", "4.990000", "0.000001", "0.0000001", "1000000", "5",
                   "0.1234567", "1.2300179", "-1", "0.00000100", "123456.654321", "000.5",
                   "0.000000000000000001"):
        cases.append(("decimal_to_units", {"amount": amount}))
    for units in ("0", "1", "4990000", str(10**12), str(10**18 // 10**12), "4993742", "999999",
                  "100", "-5", "123456789012", str(2**53)):
        cases.append(("format_units", {"units": units}))
    return cases


topic("amounts", _amount_cases())

# --- paylinks -------------------------------------------------------------------------------

_WALLET = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
_REF = "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny"
_MAX_UNITS = 10**12 + 9999  # 1,000,000.00 USD (MAX_AMOUNT) with item code 99 and nonce 99
_MAX_EVM_CHAIN_ID = 11155111  # Sepolia: the longest chain id among the built-in chains
_REFERENCE_EVM = (_USDC_ETH, 1, _EIP55[0], 4_993_742)


def _link_cases():
    cases = []
    for token, chain_id, recipient, units in [
            _REFERENCE_EVM,
            (ARC_USDC, 5042, _EIP55[1], 1_230_017),
            (_USDC_ETH.lower(), 137, _EIP55[2].lower(), 0),
            (_EIP55[3], 8453, _EIP55[0], 5_000_000),
            ("0x036CbD53842c5426634e7929541eC2318f3dCF7e", _MAX_EVM_CHAIN_ID, _EIP55[1],
             _MAX_UNITS),
            (_USDC_ETH, 0, _EIP55[0], 1),
            (_USDC_ETH, 42161, _EIP55[0], 10**12)]:
        cases.append(("eip681_transfer_uri", {"token_address": token, "chain_id": chain_id,
                                              "recipient": recipient, "units": str(units)}))
    labels = [
        ("Ruby Dreams", "Chapter 2: The Fall & more"),
        ("Ruby Dreams", "Tip"),
        ("", "Tip"),
        ("Ruby Dreams", ""),
        ("", ""),
        ("Q&A? yes", "a b  c"),
        ("What? Really & truly", "x=1&y=2?z"),
        ("Caf\u00e9 au lait", "\u65e5\u672c\u8a9e"),
        ("\u65e5\u672c", "Ch\u00e2teau"),
        ("Game \U0001f3ae", "Unlock \U0001f513 now"),
        ("\U0001f468\u200d\U0001f469\u200d\U0001f467", "\U0001d11e clef"),
        ("safe-chars_.~", "a/b+c=d%e#f"),
        ("unreserved !*'()", "100% [done]"),
        ("tab\there", "line\nbreak"),
    ]
    for label, message in labels:
        cases.append(("solana_pay_uri", {"recipient": _WALLET, "units": "4993742",
                                         "mint": USDC_MINT, "reference": _REF,
                                         "label": label, "message": message}))
    for units in (0, 1, 5_000_000, 1_000_000, _MAX_UNITS, 999_999):
        cases.append(("solana_pay_uri", {"recipient": _WALLET, "units": str(units),
                                         "mint": DEVNET_USDC_MINT, "reference": _key("ref"),
                                         "label": "Ruby Dreams", "message": "Tip"}))
    return cases


topic("paylinks", _link_cases())

# --- qr -------------------------------------------------------------------------------------

# The longest URI the payment sheet can produce. The Solana Pay link is longer than any EVM
# link (EIP-681 maxes out at about 150 chars: 42-char token + 8-digit chain id + 42-char
# recipient + 13-digit units). Solana Pay has a 44-char recipient, mint and reference, the
# amount 1000000.009999 (MAX_AMOUNT with code 99 / nonce 99, from `encode`), a game name label
# capped here at 64 characters and an item title (config caps it at 80 characters) as the
# message. Both are written as ASCII with spaces, which percent-encode to three characters
# each, the worst case for a realistic title.
_LONG_LABEL = ("The Chronicles of Ruby Dreams and the Endless Winter Night " * 2)[:64]
_LONG_TITLE = ("Chapter Two: The Fall of the House of Ruby & the Rise of Dawn " * 2)[:80]
_LONGEST_URI = FUNCS["solana_pay_uri"]({
    "recipient": _WALLET, "units": str(_MAX_UNITS), "mint": USDC_MINT, "reference": _REF,
    "label": _LONG_LABEL, "message": _LONG_TITLE})

# Byte mode at ECC MEDIUM holds 14 bytes in version 1 (21x21 modules) and 15 in version 2
# (25x25); this 15-byte lowercase string is the smallest that forces version 2.
_QR_BOUNDARY_TEXT = "abcdefghijklmno"

_QR_TEXTS = {
    "eip681": FUNCS["eip681_transfer_uri"]({"token_address": _USDC_ETH, "chain_id": 1,
                                            "recipient": _EIP55[0], "units": "4993742"}),
    "solana": FUNCS["solana_pay_uri"]({"recipient": _WALLET, "units": "4993742",
                                       "mint": USDC_MINT, "reference": _REF,
                                       "label": "Ruby Dreams", "message": "Unlock Chapter 2"}),
    "one_char": "a",
    "boundary": _QR_BOUNDARY_TEXT,
    "non_ascii": "Caf\u00e9 \u65e5\u672c\u8a9e \U0001f3ae",
    "longest": _LONGEST_URI,
}


def _qr_cases():
    cases = [("qr_matrix", {"text": t}) for t in _QR_TEXTS.values()]
    cases.append(("qr_rects", {"text": _QR_TEXTS["one_char"], "border": 2}))
    cases.append(("qr_rects", {"text": _QR_TEXTS["eip681"], "border": 2}))
    return cases


topic("qr", _qr_cases())

# --- page_messages --------------------------------------------------------------------------

_EVM_TX = "0x" + "ab" * 32
_SOL_SIG = base58.b58encode(bytes(range(64)))
_SOL_ACCOUNT = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"
_SENDER_LOWER = "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"


def _page_cases():
    cases = []
    bodies = [
        {"tx_hash": _EVM_TX.upper().replace("0X", "0x"), "from": _SENDER_LOWER},
        {"tx_hash": _EVM_TX, "from": _EIP55[0]},
        {"tx_hash": "  " + _EVM_TX + "\n", "from": "  " + _SENDER_LOWER + " "},
        {"tx_hash": _EVM_TX},
        {"tx_hash": _EVM_TX, "from": None},
        {"tx_hash": _EVM_TX, "from": ""},
        {"tx_hash": _EVM_TX, "from": "nonsense"},
        {"tx_hash": _EVM_TX, "from": 5},
        {"tx_hash": _EVM_TX, "from": []},
        {"tx_hash": _EVM_TX, "from": True},
        {"tx_hash": _EVM_TX, "from": "0x1234"},
        {"tx_hash": _EVM_TX, "from": "0x" + "zz" * 20},
        {"tx_hash": _EVM_TX, "from": _SOL_ACCOUNT},
        {"tx_hash": _EVM_TX, "from": "x" * 10_000},
        {"tx_hash": _EVM_TX, "from": "0x" + "a" * 10_000},
        {"tx_hash": _SOL_SIG, "from": _SOL_ACCOUNT},
        {"tx_hash": " " + _SOL_SIG + " ", "from": " " + _SOL_ACCOUNT + " "},
        {"tx_hash": _SOL_SIG},
        {"tx_hash": _SOL_SIG, "from": _SENDER_LOWER},
        {"tx_hash": _SOL_SIG, "from": _SOL_SIG},
        {"tx_hash": _EVM_TX, "from": _SENDER_LOWER, "extra": "ignored", "nested": {"a": [1]}},
        {"tx_hash": _SOL_SIG, "account": _SOL_ACCOUNT, "x": 1},
        # missing, wrong type, malformed, oversized
        {}, {"from": _SENDER_LOWER}, {"hash": _EVM_TX},
        {"tx_hash": None}, {"tx_hash": 5}, {"tx_hash": True}, {"tx_hash": []},
        {"tx_hash": {}}, {"tx_hash": ["0x" + "ab" * 32]},
        {"tx_hash": ""}, {"tx_hash": "0x12"}, {"tx_hash": "0x" + "ab" * 31},
        {"tx_hash": "0x" + "ab" * 33}, {"tx_hash": "0x" + "zz" * 32},
        {"tx_hash": "0x" + "ab" * 5000}, {"tx_hash": "1" * 10_000},
        {"tx_hash": _EVM_TX[2:]}, {"tx_hash": _SOL_SIG[:-1]}, {"tx_hash": _SOL_SIG + "1"},
        None, [], "x", "", 5, True, [{"tx_hash": _EVM_TX}],
    ]
    for body in bodies:
        cases.append(("parse_tx_message", {"data": body}))
    for body in [
        {"account": _SOL_ACCOUNT}, {"account": " " + _SOL_ACCOUNT + "\n"},
        {"account": _SOL_ACCOUNT, "tx_hash": _EVM_TX, "more": [1, 2]},
        {"account": USDC_MINT}, {"account": SYSTEM_PROGRAM},
        {}, {"tx_hash": _EVM_TX}, {"account": None}, {"account": 5}, {"account": True},
        {"account": []}, {"account": ""}, {"account": "0xabc"}, {"account": _EIP55[0]},
        {"account": _SOL_ACCOUNT[:-1]}, {"account": _SOL_SIG}, {"account": "1" * 10_000},
        {"account": "x" * 10_000},
        None, [], "x", "", 5, [{"account": _SOL_ACCOUNT}],
    ]:
        cases.append(("parse_build_message", {"data": body}))
    return cases


topic("page_messages", _page_cases())

# --- receipts -------------------------------------------------------------------------------

_SOL_SIG_B = base58.b58encode(bytes(range(1, 65)))


def _evm_transfer(**kw):
    t = {"class": "Transfer", "token": _USDC_ETH, "sender": _EIP55[0], "recipient": _EIP55[1],
         "value": "4990000", "tx_hash": _EVM_TX, "log_index": 7, "block_number": 1234567}
    t.update(kw)
    return t


def _evm_log_transfer(**kw):
    t = _evm_transfer(**{"class": "TransferLog", "block_hash": "0x" + "cd" * 32})
    t.update(kw)
    return t


def _sol_transfer(**kw):
    t = {"class": "Transfer", "token": USDC_MINT, "sender": _SOL_ACCOUNT, "recipient": _WALLET,
         "value": "4993742", "tx_hash": _SOL_SIG, "log_index": 3, "block_number": 287_654_321}
    t.update(kw)
    return t


def _receipt_cases():
    cases = []
    keys = [("ethereum", _EVM_TX, 7), ("ethereum", _EVM_TX.upper().replace("0X", "0x"), 7),
            ("ethereum", "0X" + "AB" * 32, 0), ("arc", "0x" + "Ab" * 32, 12),
            ("polygon", "0xABCDEF", 1), ("solana", _SOL_SIG, 3),
            ("solana", _SOL_SIG.lower(), 3), ("solana", _SOL_SIG.upper(), 3),
            ("solana-devnet", _SOL_SIG_B, 0), ("base", "", 0)]
    for chain, tx_hash, idx in keys:
        cases.append(("receipt_key", {"chain": chain, "tx_hash": tx_hash, "log_index": idx}))

    def mk(chain, transfer, kind="unlock", item="chapter2", token="USDC", source="payment"):
        return {"chain": chain, "transfer": transfer, "kind": kind, "item": item,
                "token": token, "source": source}

    cases += [
        ("make_receipt", mk("ethereum", _evm_transfer())),
        ("make_receipt", mk("ethereum", _evm_log_transfer(), source="restore")),
        ("make_receipt", mk("solana", _sol_transfer())),
        ("make_receipt", mk("solana", _sol_transfer(value="5000000"), kind="tip", item=None)),
        ("make_receipt", mk("ethereum", _evm_transfer(value="0"))),
        ("make_receipt", mk("ethereum", _evm_transfer(value="1"))),
        ("make_receipt", mk("ethereum", _evm_transfer(value=str(10**12)), kind="tip", item=None)),
        ("make_receipt", mk("arc", _evm_transfer(value=str(10**18 // 10**12)))),
        ("make_receipt", mk("ethereum", _evm_transfer(value="1230017"), kind="tip", item=None,
                            source="restore")),
        ("make_receipt", mk("ethereum", _evm_log_transfer(log_index=0, block_number=0))),
        ("make_receipt", mk("ethereum", _evm_transfer(), kind="refund")),
        ("make_receipt", mk("ethereum", _evm_transfer(), kind="")),
        ("make_receipt", mk("ethereum", _evm_transfer(), kind="Unlock")),
        ("make_receipt", mk("ethereum", _evm_transfer(), source="manual")),
        ("make_receipt", mk("ethereum", _evm_transfer(), source=None)),
    ]

    def rec(chain="ethereum", tx_hash=_EVM_TX, idx=7, kind="unlock", item="chapter2",
            amount="4.990000", **kw):
        r = {"chain": chain, "tx_hash": tx_hash, "log_index": idx, "block": 1, "kind": kind,
             "item": item, "amount": amount, "token": "USDC", "payer": _EIP55[0],
             "recipient": _EIP55[1], "source": "payment"}
        r.update(kw)
        return r

    def keyed(r):
        return receipts.receipt_key(r["chain"], r["tx_hash"], r["log_index"])

    r1, r2 = rec(), rec(tx_hash="0x" + "cd" * 32, idx=2, kind="tip", item=None,
                        amount="3.000000")
    for store, receipt in [({}, r1), ({keyed(r1): r1}, r1),
                           ({keyed(r1): r1}, rec(tx_hash=_EVM_TX.upper().replace("0X", "0x"),
                                                 amount="9.000000")),
                           ({keyed(r1): r1}, r2),
                           ({keyed(r1): r1}, rec(idx=8)),
                           ({}, rec(chain="solana", tx_hash=_SOL_SIG, idx=3)),
                           ({keyed(rec(chain="solana", tx_hash=_SOL_SIG, idx=3)): r1},
                            rec(chain="solana", tx_hash=_SOL_SIG.lower(), idx=3))]:
        cases.append(("add_receipt", {"store": store, "receipt": receipt}))
    for store, key in [({keyed(r1): r1, keyed(r2): r2}, keyed(r1)),
                       ({keyed(r1): r1}, "ethereum:nothing:1"), ({}, ""),
                       ({keyed(r1): r1}, _EVM_TX)]:
        cases.append(("remove_receipt", {"store": store, "key": key}))

    items = [
        {"item_id": "chapter2", "code": 2, "price": "4.99", "token": "USDC",
         "title": "Chapter 2", "min_price": "4.99"},
        {"item_id": "pwyw", "code": 3, "price": "10.00", "token": "USDC", "title": "Pay what",
         "min_price": "2.00"},
        {"item_id": "dlc", "code": 4, "price": "9.99", "token": "USDT", "title": "DLC",
         "min_price": "9.99"},
    ]

    def store_of(*rs):
        return {f"k{n}": r for n, r in enumerate(rs)}

    def own(*rs, store=None, its=items):
        cases.append(("compute_ownership", {"store": store if store is not None
                                            else store_of(*rs), "items": its}))

    own()
    own(rec())  # owns chapter2
    own(rec(amount="4.980000"))  # below price and min_price
    own(rec(item="pwyw", amount="2.000000"))  # below price, at min_price: owned
    own(rec(item="pwyw", amount="3.500000"))  # below price, above min_price: owned
    own(rec(item="pwyw", amount="1.990000"))  # below min_price
    own(rec(item="pwyw", amount="25.000000"))
    own(rec(item="ghost"))  # unknown item
    own(rec(item=None))
    own(rec(kind="tip", item=None, amount="1.000000"))
    own(rec(kind="tip", item=None, amount="0.500000"),
        rec(tx_hash="0x" + "11" * 32, kind="tip", item=None, amount="3.000000"),
        rec(tx_hash="0x" + "22" * 32, kind="tip", item=None, amount="10.250000"))
    own(rec(), rec(item="pwyw", amount="2.500000", tx_hash="0x" + "33" * 32),
        rec(item="ghost", tx_hash="0x" + "44" * 32),
        rec(kind="tip", item=None, amount="5.000000", tx_hash="0x" + "55" * 32),
        rec(item="dlc", amount="9.990000", tx_hash="0x" + "66" * 32))
    # the same unlock under two keys owns once; the same tip under two keys counts twice
    own(rec(), rec(amount="5.000000"))
    own(store={"ethereum:" + _EVM_TX + ":7": rec(kind="tip", item=None, amount="1.000000"),
               "ethereum:" + _EVM_TX.upper() + ":7": rec(kind="tip", item=None,
                                                         amount="1.000000")})
    # malformed entries are skipped, never fatal
    own(store={"a": None, "b": "text", "c": [], "d": 5, "e": {}, "f": {"kind": "unlock"},
               "g": {"kind": "tip"}, "h": {"kind": "tip", "amount": "abc"},
               "i": {"kind": "tip", "amount": None}, "j": {"kind": "tip", "amount": "NaN"},
               "k": {"kind": "tip", "amount": "Infinity"},
               "l": {"kind": "unlock", "item": "chapter2", "amount": "-Infinity"},
               "m": {"kind": "refund", "amount": "5.00"}, "n": {"amount": "5.00"},
               "o": {"kind": "unlock", "item": "chapter2", "amount": "sNaN"},
               "p": rec(kind="tip", item=None, amount="2.000000"),
               "q": {"kind": "tip", "amount": 4}, "r": {"kind": "tip", "amount": True},
               "s": {"kind": "tip", "amount": ""}, "t": {"kind": ["tip"], "amount": "1"}})
    own(rec(), its=[])
    own(rec(kind="tip", item=None, amount="1.5"), rec(kind="tip", item=None, amount="0",
                                                       tx_hash="0x" + "77" * 32))
    own(rec(item="chapter2", amount="4.99"), its=items[:1])
    return cases


topic("receipts", _receipt_cases())

# --- tx_ids ---------------------------------------------------------------------------------


def _tx_id_cases():
    cases = []
    evm_like = [
        _EVM_TX, _EVM_TX.upper().replace("0X", "0x"), "0x" + "aB" * 32, "  " + _EVM_TX + "  ",
        "\t" + _EVM_TX + "\n", "\u00a0" + _EVM_TX + "\u3000", "0X" + "ab" * 32, _EVM_TX[2:],
        "0x" + "ab" * 31, "0x" + "ab" * 33, "0x" + "ag" * 32, "0x" + "ab" * 16 + " " + "ab" * 16,
        "0x" + "0" * 64, "0x" + "f" * 64, "0x" + "F" * 64, "", " ", "0x", "0x" + "ab" * 5000,
        _EVM_TX + "\n" + _EVM_TX, "0x" + "\uff11" * 64,
    ]
    sol_like = [
        _SOL_SIG, "  " + _SOL_SIG + "\n", _SOL_SIG_B, base58.b58encode(b"\0" * 64),
        base58.b58encode(b"\xff" * 64), _SOL_SIG[:-1], _SOL_SIG + "1", _SOL_SIG.lower(),
        _SOL_SIG.upper(), USDC_MINT, _SOL_ACCOUNT, "0" + _SOL_SIG[1:], "1" * 10_000,
    ]
    other = ["garbage", "not a hash", "\u65e5\u672c", "0", "O" * 88, 5, None, True, [], {},
             [_EVM_TX]]
    for text in evm_like + other:
        cases.append(("normalize_tx_hash", {"text": text}))
    for text in sol_like + evm_like[:3] + other:
        cases.append(("normalize_signature", {"text": text}))
    for text in evm_like + sol_like + other:
        cases.append(("normalize_any_tx", {"text": text}))
    return cases


topic("tx_ids", _tx_id_cases())

# --- message catalogue (a generated source of its own, not a Tier 1 topic) ------------------

SOURCES["messages.json"] = lambda: {"messages": dict(sorted(MESSAGES.items()))}


# Registers the configuration vectors and their FUNCS adapters (needs _fn/topic from above).
from . import tier1_config  # noqa: E402,F401
