"""Tier 1 configuration vectors: chain registry, build_config, build_chain, add_item and
prepare_payment. Validation failures are compared by `ConfigError.code`, never by message.

Input conventions (same file shape as tier1.py):
- A Config is always given as the kwargs of build_config (`config`); a custom chain inside
  it is the kwargs of build_chain (a non-dict entry is passed through untouched, which is
  how `chain.not_from_add_chain` is reached). Items are lists of add_item kwargs
  (`existing` / `items`), added in order.
- A float that JSON cannot carry exactly is written `{"float": "4.99"}` and rebuilt with
  float(); numbers that are plain JSON ints or strings are passed as they are.
- Outputs are `to_jsonable` of the Python dataclasses (Decimals become strings).
"""

from __future__ import annotations

from typing import Any, Callable

from scfg_core import chains as chains_mod
from scfg_core.config import add_item, build_chain, build_config
from scfg_core.payments import prepare_payment
from scfg_core.solana import pda
from scfg_core.solana.base58 import b58encode

from . import tier1
from .canonical import to_jsonable
from .tier1 import _EIP55, _WALLET, USDC_MINT, _find_on_curve, _fn

SOURCES: dict[str, Callable[[], dict]] = {}

# Codes that no pure input of the Tier 1 functions reaches, with the reason why.
UNREACHABLE_CODES: dict[str, str] = {
    "chain.token_unavailable": "Engine-level (Engine.start_session); covered by Tier 2 scenarios",
}


def _topic(name: str, cases: list[tuple[str, dict]]) -> None:
    def build() -> dict:
        return {"tier": 1, "topic": name,
                "cases": [{"fn": fn, "in": inputs, **tier1._compute(fn, inputs)}
                          for fn, inputs in cases]}
    SOURCES[f"tier1/{name}.json"] = build


# --- adapters --------------------------------------------------------------------------------


def _value(v: Any) -> Any:
    """{"float": "4.99"} -> 4.99; everything else unchanged."""
    if isinstance(v, dict) and set(v) == {"float"}:
        return float(v["float"])
    return v


def _chain_or_raw(entry):
    return build_chain(**entry) if isinstance(entry, dict) else entry


def _config(kwargs: dict):
    kw = dict(kwargs)
    if "custom_chains" in kw:
        kw["custom_chains"] = [_chain_or_raw(c) for c in kw["custom_chains"]]
    return build_config(**kw)


def _item_kwargs(d: dict) -> dict:
    kw = {k: _value(v) for k, v in d.items()}
    return kw


def _with_items(config_kw: dict, items: list):
    config = _config(config_kw)
    for item in items:
        kw = _item_kwargs(item)
        add_item(config, kw.pop("item_id"), **kw)
    return config


@_fn("get_chains")
def _get_chains(i):
    return to_jsonable(chains_mod.get_chains(i["testnet"]))


@_fn("build_chain")
def _build_chain(i):
    return to_jsonable(build_chain(**i))


@_fn("build_config")
def _build_config(i):
    return to_jsonable(_config(i))


@_fn("add_item")
def _add_item(i):
    config = _with_items(i["config"], i.get("existing", []))
    kw = _item_kwargs(i["item"])
    return to_jsonable(add_item(config, kw.pop("item_id"), **kw))


@_fn("prepare_payment")
def _prepare_payment(i):
    config = _with_items(i["config"], i.get("items", []))
    kw = {k: _value(i[k]) for k in ("item", "amount", "token", "title") if k in i}
    return to_jsonable(prepare_payment(config, i["kind"], **kw))


# --- fixtures --------------------------------------------------------------------------------

EVM = _EIP55[0]
EVM2 = _EIP55[1]
EVM3 = _EIP55[2]
SOL = _WALLET
SOL2 = b58encode(_find_on_curve("config/solana-2"))
SOL3 = b58encode(_find_on_curve("config/solana-3"))
NOT_ON_CURVE = pda.associated_token_address(SOL, USDC_MINT)  # a token account, not a wallet

CHAIN = {
    "key": "avalanche", "name": "Avalanche", "chain_id": 43114,
    "rpc": "https://api.avax.network/ext/bc/C/rpc",
    "tokens": {"USDC": "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e"},
    "confirmations": 5, "explorer_tx": "https://snowtrace.io/tx/{}",
}


def _c(**over) -> dict:
    """build_config kwargs: an EVM wallet unless overridden."""
    return {"wallet": EVM, **over}


def _chain(**over) -> dict:
    return {**CHAIN, **over}


def _drop(d: dict, *keys: str) -> dict:
    return {k: v for k, v in d.items() if k not in keys}


# --- chains ----------------------------------------------------------------------------------

_topic("chains", [("get_chains", {"testnet": False}), ("get_chains", {"testnet": True})])

# --- config ----------------------------------------------------------------------------------

_CONFIG_VALID = [
    _c(),
    _c(wallet=EVM.lower()),
    {"solana_wallet": SOL},
    {"wallet": EVM, "solana_wallet": SOL},
    _c(testnet=True),
    {"wallet": EVM, "solana_wallet": SOL, "testnet": True},
    _c(chains=["base", "arbitrum"]),
    _c(chains=["polygon", "base", "ethereum", "arc"]),
    {"wallet": EVM, "solana_wallet": SOL, "chains": ["solana", "base"]},
    {"solana_wallet": SOL, "chains": ["solana"], "tokens": ["USDT"]},
    _c(tokens=["USDC"]),
    _c(tokens=["USDT"]),
    _c(tokens=["USDT", "USDC"]),
    _c(chains=["base", "arbitrum"],
       methods={"qr": {"wallet_button": False, "chains": ["base"]},
                "browser": {"timeout_min": 45}, "manual": {"allow_tx_paste": False}}),
    _c(methods={"browser": {"enabled": False}, "manual": {"enabled": False}}),
    _c(methods={"qr": {"enabled": False}, "browser": {"enabled": False}}),
    _c(rpc_urls={"base": "https://base.example.org",
                 "arbitrum": ["https://arb.example.org", "http://127.0.0.1:8545"]}),
    _c(rpc_urls={"ethereum": "http://localhost:8545/rpc"}),
    _c(confirmations={"base": 3, "ethereum": 1}),
    {"wallet": EVM, "solana_wallet": SOL, "rpc_urls": {"solana": ["https://sol.example.org"]}},
    {"wallet": EVM, "solana_wallet": SOL,
     "previous_wallets": [EVM2, SOL2, EVM.lower(), EVM2.lower(), SOL, SOL2]},
    _c(previous_wallets=[]),
    _c(accent="#abc"),
    _c(accent="#0E7C74"),
    _c(accent="#AABBCCDD"),
    _c(logo="images/logo.png"),
    _c(logo="gui/Logo.JPG"),
    _c(logo="a.webp"),
    _c(testnet=False, chains=["arc"], tokens=["USDC"]),
]


def _config_errors():
    ok = {"wallet": EVM, "solana_wallet": SOL}
    return [
        {},
        {"wallet": None},
        {"wallet": "0x123"},
        {"wallet": "not an address"},
        {"wallet": 5},
        {"solana_wallet": "not base58 0OIl"},
        {"solana_wallet": NOT_ON_CURVE},
        _c(previous_wallets=["0xnothex"]),
        {"wallet": EVM, "previous_wallets": [NOT_ON_CURVE]},
        {"wallet": EVM, "previous_wallets": ["short"]},
        _c(chains=[]),
        _c(chains=["nope"]),
        _c(chains=["base", "base"]),
        _c(chains=["solana"]),
        {"solana_wallet": SOL, "chains": ["base"]},
        _c(tokens=[]),
        _c(tokens=["DAI"]),
        _c(chains=["base"], tokens=["USDT"]),
        {"solana_wallet": SOL, "tokens": ["USDT"], "testnet": True},
        _c(methods={"paypal": {"enabled": True}}),
        _c(methods={"qr": {"colour": "red"}}),
        _c(methods={"qr": {"enabled": "yes"}}),
        _c(methods={"manual": {"enabled": 1}}),
        _c(methods={"browser": {"timeout_min": 0}}),
        _c(methods={"browser": {"timeout_min": -5}}),
        _c(methods={"browser": {"timeout_min": "30"}}),
        _c(methods={"browser": {"timeout_min": True}}),
        _c(methods={"browser": {"enabled": False}, "qr": {"enabled": False},
                    "manual": {"enabled": False}}),
        _c(chains=["base"], methods={"qr": {"chains": ["polygon"]}}),
        _c(rpc_urls={"polygon": "https://x.example.org"}, chains=["base"]),
        _c(rpc_urls={"nope": "https://x.example.org"}),
        _c(rpc_urls={"base": []}),
        _c(rpc_urls={"base": 5}),
        _c(rpc_urls={"base": "http://example.org"}),
        _c(rpc_urls={"base": ["https://ok.example.org", "ftp://x"]}),
        _c(rpc_urls={"base": "http://127.0.0.1.evil.org"}),
        _c(rpc_urls={"base": "http://127.0.0.1:8545\n"}),   # full match: no trailing newline
        _c(rpc_urls={"base": "http://localhost\n"}),
        _c(confirmations={"polygon": 2}, chains=["base"]),
        {**ok, "confirmations": {"solana": 5}},
        _c(confirmations={"base": 0}),
        _c(confirmations={"base": True}),
        _c(confirmations={"base": "3"}),
        _c(logo=""),
        _c(logo="/etc/logo.png"),
        _c(logo="../logo.png"),
        _c(logo="a/../b.png"),
        _c(logo="images\\logo.png"),
        _c(logo="C:logo.png"),
        _c(logo=5),
        _c(logo="images/logo.svg"),
        _c(logo="images/logo"),
        _c(accent="red"),
        _c(accent="#12"),
        _c(accent="#12345"),
        _c(accent="0e7c74"),
        _c(accent=5),
        _c(accent="#0e7c74\n"),       # full match: no trailing newline
        _c(accent="#abc\n"),
    ]


_topic("config", [("build_config", i) for i in _CONFIG_VALID + _config_errors()])

# --- custom chains ---------------------------------------------------------------------------

_TESTNET_CHAIN = _chain(key="fuji", name="Avalanche Fuji", chain_id=43113, testnet=True)

_CHAIN_VALID = [
    CHAIN,
    _chain(native_symbol="AVAX", fee_hint="lowest", max_log_range=2000),
    _chain(rpc=["https://a.example.org", "http://127.0.0.1:8545"], fee_hint="high"),
    _chain(tokens={"USDC": "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e",
                   "USDT": "0x9702230a8ea53601f5cd2dc00fdbc13d4df4a8c7"}),
    _chain(name="  Padded  ", max_log_range=10),
    _chain(testnet=True),
    _chain(key="a-b_9", name="N" * 32),
]

_CHAIN_ERRORS = [
    _chain(key=""),
    _chain(key="Avalanche"),
    _chain(key="k" * 33),
    _chain(key=5),
    _chain(name=""),
    _chain(name="   "),
    _chain(name="N" * 33),
    _chain(name=5),
    _chain(chain_id=0),
    _chain(chain_id="43114"),
    _chain(chain_id=True),
    _chain(rpc=[]),
    _chain(rpc=5),
    _chain(rpc="http://example.org"),
    _chain(rpc="http://127.0.0.1:8545\n"),       # full match: no trailing newline
    _chain(tokens={}),
    _chain(tokens=["USDC"]),
    _chain(tokens={"DAI": "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e"}),
    _chain(tokens={"USDC": "0x1234"}),
    _chain(confirmations=0),
    _chain(confirmations=True),
    _chain(explorer_tx="http://snowtrace.io/tx/{}"),
    _chain(explorer_tx="https://snowtrace.io/tx/"),
    _chain(explorer_tx="https://snowtrace.io/{}/{}"),
    _chain(explorer_tx=5),
    _chain(explorer_tx="https://snowtrace.io/{x}/{}"),
    _chain(explorer_tx="https://snowtrace.io/tx/{}/{"),
    _chain(explorer_tx="https://snowtrace.io/tx/{}?a={0}"),
    _chain(native_symbol=""),
    _chain(native_symbol="TOOLONGSYMBOL"),
    _chain(native_symbol="A-B"),
    _chain(native_symbol="AVAX\n"),       # full match: no trailing newline
    _chain(key="avalanche\n"),
    _chain(fee_hint="medium"),
    _chain(max_log_range=9),
    _chain(max_log_range="1000"),
]

_CUSTOM_CONFIG = [
    _c(custom_chains=[CHAIN]),
    _c(custom_chains=[CHAIN], chains=["avalanche", "base"]),
    _c(custom_chains=[CHAIN], chains=["base", "avalanche"], tokens=["USDC"]),
    _c(custom_chains=[CHAIN, _chain(key="other", chain_id=999)]),
    _c(custom_chains=[CHAIN], rpc_urls={"avalanche": "https://mine.example.org"},
       confirmations={"avalanche": 2}),
    _c(custom_chains=[CHAIN], methods={"qr": {"chains": ["avalanche"]}}),
    _c(testnet=True, custom_chains=[_TESTNET_CHAIN]),
    _c(testnet=True, custom_chains=[_TESTNET_CHAIN], chains=["fuji", "base"]),
    # a chain added only for the other network is ignored when it is not used
    _c(custom_chains=[_TESTNET_CHAIN]),
    _c(testnet=True, custom_chains=[CHAIN]),
    _c(custom_chains=[_TESTNET_CHAIN, CHAIN], chains=["avalanche"]),
    _c(custom_chains=[CHAIN, _TESTNET_CHAIN], chains=["base"]),
    # the same key may exist once per network
    _c(custom_chains=[CHAIN, _chain(testnet=True, chain_id=43113)]),
    _c(custom_chains=[]),
    {"solana_wallet": SOL, "custom_chains": [CHAIN]},
    # both families: custom EVM chains come before Solana in the default order
    {"wallet": EVM, "solana_wallet": SOL, "custom_chains": [CHAIN]},
    # a chain id of a mainnet built-in is free on testnet
    _c(testnet=True, custom_chains=[_chain(key="fork", chain_id=1, testnet=True)]),
    _c(testnet=True, custom_chains=[_chain(testnet=True)], chains=["avalanche"], tokens=["USDC"]),
]

_CUSTOM_CONFIG_ERRORS = [
    _c(custom_chains=[_TESTNET_CHAIN], chains=["fuji"]),
    _c(testnet=True, custom_chains=[CHAIN], chains=["avalanche"]),
    _c(custom_chains=[_chain(key="base")]),
    _c(custom_chains=[_chain(key="solana", chain_id=1234)]),
    _c(testnet=True, custom_chains=[_chain(key="ethereum", testnet=True)]),
    _c(custom_chains=[CHAIN, CHAIN]),
    _c(custom_chains=[CHAIN, _chain(key="other")]),
    _c(custom_chains=[_chain(chain_id=8453)]),
    _c(testnet=True, custom_chains=[_chain(chain_id=84532, testnet=True)]),
    _c(custom_chains=["avalanche"]),
    _c(custom_chains=[5]),
    _c(custom_chains=[_chain(chain_id=0)]),
    _c(custom_chains=[CHAIN], rpc_urls={"avalanche": "http://bad.example.org"}),
    _c(custom_chains=[CHAIN], confirmations={"avalanche": 0}),
    _c(custom_chains=[CHAIN], chains=["avalanche"], tokens=["USDT"]),
]

_topic("custom_chains",
       [("build_chain", i) for i in _CHAIN_VALID + _CHAIN_ERRORS]
       + [("build_config", i) for i in _CUSTOM_CONFIG + _CUSTOM_CONFIG_ERRORS])

# --- items -----------------------------------------------------------------------------------

_BOTH = {"wallet": EVM, "solana_wallet": SOL}


def _item(item_id="chapter-2", **over) -> dict:
    return {"item_id": item_id, "code": 1, "price": "4.99", "token": "USDC",
            "title": "Chapter 2", **over}


def _a(item: dict, config=None, existing=None) -> dict:
    out = {"config": config or _c(), "item": item}
    if existing:
        out["existing"] = existing
    return out


_ITEM_VALID = [
    _a(_item()),
    _a(_item(price=5)),
    _a(_item(price="0.5", code=99)),
    _a(_item(price="4.99", min_price="1.00")),
    _a(_item(price="10", min_price=10)),
    _a(_item(token="USDT")),
    _a(_item("a")),
    _a(_item("under_score-9")),
    _a(_item("x" * 64)),
    _a(_item(title="T" * 80)),
    _a(_item(title="Kapitel Zwei — über")),
    _a(_item("second", code=2), existing=[_item()]),
    _a(_item(token="USDC"), config=_BOTH),
    _a(_item(price="1000000")),
    _a(_item(price="0.01")),
    _a(_item(price=" 4.99 ")),
    _a(_item(), config=_c(chains=["avalanche"], custom_chains=[CHAIN])),
]

_ITEM_ERRORS = [
    _a(_item("")),
    _a(_item("chapter-2\n")),       # full match: no trailing newline
    _a(_item("Chapter")),
    _a(_item("has space")),
    _a(_item("x" * 65)),
    _a(_item(5)),
    _a(_item(None)),
    _a(_item("chapter-2", code=2), existing=[_item()]),
    _a(_item(code=0)),
    _a(_item(code=100)),
    _a(_item(code=-1)),
    _a(_item(code="1")),
    _a(_item(code=True)),
    _a(_item(code={"float": "1.5"})),
    _a(_item("two", code=1), existing=[_item()]),
    _a(_item(price="abc")),
    _a(_item(price="")),
    _a(_item(price=0)),
    _a(_item(price="-1")),
    _a(_item(price="0.001")),
    _a(_item(price="1000000.01")),
    _a(_item(price={"float": "4.99"})),
    _a(_item(price=True)),
    _a(_item(price=None)),
    _a(_item(min_price="abc")),
    _a(_item(min_price=0)),
    _a(_item(price="2", min_price="3")),
    _a(_item(min_price={"float": "1.5"})),
    _a(_item(token="DAI")),
    _a(_item(token=None)),
    _a(_item(token="USDT"), config=_c(tokens=["USDC"])),
    _a(_item(token="USDT"), config=_c(chains=["base"])),
    _a(_item(token="USDC"), config=_c(tokens=["USDT"])),
    _a(_item(token="USDT"), config=_c(chains=["avalanche"], custom_chains=[CHAIN])),
    _a(_item(token="USDT"), config={"solana_wallet": SOL, "testnet": True}),  # devnet: no USDT
    _a(_item(title="")),
    _a(_item(title="   ")),
    _a(_item(title="T" * 81)),
    _a(_item(title=5)),
]

_topic("items", [("add_item", i) for i in _ITEM_VALID + _ITEM_ERRORS])

# --- payments --------------------------------------------------------------------------------

_UNLOCK_ITEM = _item("chapter-2", min_price="2.00")
_CHAPTER = _item("chapter-3", code=3, price="3.50", token="USDT")


def _p(kind, config=None, items=None, **fields) -> dict:
    return {"config": config or _c(), "items": [_UNLOCK_ITEM] if items is None else items,
            "kind": kind, **fields}


_PAY_VALID = [
    _p("unlock", item="chapter-2"),
    _p("unlock", item="chapter-2", amount="3.25"),
    _p("unlock", item="chapter-2", amount="2"),
    _p("unlock", item="chapter-2", amount=7),
    _p("unlock", item="chapter-2", token="USDT"),
    _p("unlock", item="chapter-2", title="A better title"),
    _p("unlock", items=[_UNLOCK_ITEM, _CHAPTER], item="chapter-3"),
    _p("unlock", config=_BOTH, item="chapter-2"),
    _p("unlock", config=_c(chains=["base", "polygon"], tokens=["USDC"]), item="chapter-2"),
    _p("unlock", config={"solana_wallet": SOL}, item="chapter-2"),
    _p("tip", amount="1.00", token="USDC"),
    _p("tip", amount="0.50", token="USDC"),
    _p("tip", amount=5, token="USDT"),
    _p("tip", amount="2.5", token="USDC", title="Thanks!"),
    _p("tip", amount="1.00", token="USDC", item="ignored"),
    _p("tip", config=_BOTH, amount="3", token="USDC"),
    _p("tip", config=_c(chains=["arc", "base"]), amount="1", token="USDC", items=[]),
    _p("tip", config=_c(testnet=True), amount="1", token="USDC", items=[]),
]

_PAY_ERRORS = [
    _p("unlock", item="missing"),
    _p("unlock"),
    _p("unlock", item="chapter-2", amount="1.99"),
    _p("unlock", item="chapter-2", amount={"float": "4.99"}),
    _p("unlock", item="chapter-2", amount="abc"),
    _p("unlock", item="chapter-2", amount=True),
    _p("unlock", item="chapter-2", amount=0),
    _p("unlock", item="chapter-2", token="DAI"),
    _p("unlock", item="chapter-2", config=_c(tokens=["USDT"]), items=[_CHAPTER]),
    _p("tip", amount="0.49", token="USDC"),
    _p("tip", amount=0, token="USDC"),
    _p("tip", amount={"float": "2.5"}, token="USDC"),
    _p("tip", amount="abc", token="USDC"),
    _p("tip", amount="1", token="DAI"),
    _p("tip", amount="1", token="USDT", config=_c(tokens=["USDC"])),
    _p("tip", amount="1", token="USDT", config=_c(chains=["base"])),
    _p("tip", token="USDC"),
    _p("tip", amount="1"),
    _p("tip"),
    _p("donate", amount="1", token="USDC"),
    _p("", amount="1", token="USDC"),
    _p(None, amount="1", token="USDC"),
]

_topic("payments", [("prepare_payment", i) for i in _PAY_VALID + _PAY_ERRORS])
