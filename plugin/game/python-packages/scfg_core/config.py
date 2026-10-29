"""Developer configuration: validation of configure()/add_item() arguments.

Every mistake raises ConfigError at startup, so it surfaces while the developer
is testing instead of when a player tries to pay.
"""

from __future__ import annotations

import copy
import re
from dataclasses import dataclass, field, replace

from .amounts import parse_amount
from .chains import DISPLAY_ORDER, Chain, Token, get_chains
from .errors import ConfigError
from .evm import normalize_address
from .models import Item
from .rpc import MIN_LOG_RANGE
from .solana.base58 import b58decode, normalize_pubkey
from .solana.ed25519 import is_on_curve

TOKENS = ("USDC", "USDT")
DEFAULT_METHODS = {
    "browser": {"enabled": True, "timeout_min": 30},
    "qr": {"enabled": True, "wallet_button": True, "chains": None},
    "manual": {"enabled": True, "allow_tx_paste": True},
}
_ITEM_ID = re.compile(r"^[a-z0-9_-]{1,64}$")
_CHAIN_KEY = re.compile(r"^[a-z0-9_-]{1,32}$")
_NATIVE_SYMBOL = re.compile(r"^[A-Za-z0-9]{1,11}$")
# Plain http is only accepted for a node on this computer (e.g. a local Anvil test chain).
_LOCAL_HTTP = re.compile(r"^http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/|$)")
FEE_HINTS = ("lowest", "low", "high")

DEFAULT_ACCENT = "#0e7c74"
INK = "#17202e"
# Raster formats only: an SVG served from the checkout page's origin could run script.
LOGO_TYPES = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
              ".webp": "image/webp"}
_HEX_COLOR = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")


@dataclass
class Config:
    wallet: str | None   # EVM payout address; None for a Solana-only game
    testnet: bool
    chains: tuple[str, ...]
    tokens: tuple[str, ...]
    methods: dict[str, dict]
    rpc_urls: dict[str, tuple[str, ...]]
    confirmations: dict[str, int]
    previous_wallets: tuple[str, ...] = ()
    solana_wallet: str | None = None
    previous_solana_wallets: tuple[str, ...] = ()
    logo: str | None = None          # Ren'Py image path, shown on the screens and the checkout page
    accent: str = DEFAULT_ACCENT     # #rrggbb used for selected options
    custom_chains: dict[str, Chain] = field(default_factory=dict)  # add_chain(), this network
    items: dict[str, Item] = field(default_factory=dict)

    @property
    def registry(self) -> dict[str, Chain]:
        """Every chain this game knows on its network: built-in, then the developer's own."""
        return {**get_chains(self.testnet), **self.custom_chains}

    @property
    def accepted_wallets(self) -> tuple[str, ...]:
        """EVM: current wallet first, then wallets used by earlier versions of the game."""
        return ((self.wallet,) if self.wallet else ()) + self.previous_wallets

    def accepted_wallets_for(self, family: str) -> tuple[str, ...]:
        if family == "solana":
            current = (self.solana_wallet,) if self.solana_wallet else ()
            return current + self.previous_solana_wallets
        return self.accepted_wallets

    def wallet_for(self, chain) -> str | None:
        return self.solana_wallet if chain.family == "solana" else self.wallet

    def item_by_code(self, code: int) -> Item | None:
        for item in self.items.values():
            if item.code == code:
                return item
        return None


def _address(value, what: str) -> str:
    try:
        return normalize_address(value)
    except ValueError as e:
        raise ConfigError(f"{what}: {e}") from None


def _solana_address(value, what: str) -> str:
    try:
        address = normalize_pubkey(value)
    except ValueError:
        raise ConfigError(f"{what}: not a Solana address: {value!r}") from None
    if not is_on_curve(b58decode(address)):
        raise ConfigError(f"{what}: {address} looks like a token account or program address; "
                          "use your wallet address")
    return address


def _any_wallet(value, what: str) -> tuple[str, str]:
    """(family, normalized address): 0x addresses are EVM, anything else Solana."""
    if isinstance(value, str) and value.strip().startswith("0x"):
        return "evm", _address(value.strip(), what)
    return "solana", _solana_address(value, what)


def _rpc_urls(value, what: str) -> tuple[str, ...]:
    urls = [value] if isinstance(value, str) else value
    if not isinstance(urls, (list, tuple)) or not urls:
        raise ConfigError(f"{what} must be a URL or a non-empty list of URLs")
    for u in urls:
        if not isinstance(u, str) or not (u.startswith("https://") or _LOCAL_HTTP.match(u)):
            raise ConfigError(f"rpc url must start with https://: {u!r}")
    return tuple(urls)


def _positive_int(value, what: str, minimum: int = 1) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ConfigError(f"{what} must be an int >= {minimum}")
    return value


def build_chain(key, *, name, chain_id, rpc, tokens, confirmations, explorer_tx,
                native_symbol="ETH", fee_hint="low", max_log_range=1000,
                testnet=False) -> Chain:
    """A developer-defined EVM chain (scfg.add_chain). Validated like the rest of
    the configuration, so a typo fails at startup rather than when a player pays.

    Only EVM chains work: payments are found through ERC-20 Transfer logs over
    Ethereum JSON-RPC. The tokens must have 6 decimals, because the item code and
    nonce live in the last four decimals of the amount.
    """
    if not isinstance(key, str) or not _CHAIN_KEY.match(key):
        raise ConfigError(f"invalid chain key {key!r} (use a-z, 0-9, _ and -)")
    what = f"chain {key!r}"
    if not isinstance(name, str) or not name.strip() or len(name) > 32:
        raise ConfigError(f"{what}: name must be a non-empty string of at most 32 characters")
    chain_id = _positive_int(chain_id, f"{what}: chain_id")
    rpc = _rpc_urls(rpc, f"{what}: rpc")
    if not isinstance(tokens, dict) or not tokens:
        raise ConfigError(f"{what}: tokens must be a dict like {{'USDC': '0x...'}}")
    parsed: dict[str, Token] = {}
    for symbol, address in tokens.items():
        if symbol not in TOKENS:
            raise ConfigError(f"{what}: unknown token {symbol!r} (supported: USDC, USDT)")
        parsed[symbol] = Token(_address(address, f"{what}: {symbol} address"))
    confirmations = _positive_int(confirmations, f"{what}: confirmations")
    if (not isinstance(explorer_tx, str) or not explorer_tx.startswith("https://")
            or explorer_tx.count("{}") != 1):
        raise ConfigError(f"{what}: explorer_tx must be an https:// URL with {{}} where the "
                          f"transaction hash goes, like 'https://snowtrace.io/tx/{{}}'")
    try:
        explorer_tx.format("0x")
    except (IndexError, KeyError, ValueError):
        raise ConfigError(f"{what}: explorer_tx may contain no braces besides {{}}") from None
    if not isinstance(native_symbol, str) or not _NATIVE_SYMBOL.match(native_symbol):
        raise ConfigError(f"{what}: native_symbol must be letters/digits, like 'AVAX'")
    if fee_hint not in FEE_HINTS:
        raise ConfigError(f"{what}: fee_hint must be one of {', '.join(FEE_HINTS)}")
    max_log_range = _positive_int(max_log_range, f"{what}: max_log_range", MIN_LOG_RANGE)
    return Chain(key=key, chain_id=chain_id, name=name.strip(), rpc=rpc,
                 confirmations=confirmations, max_log_range=max_log_range,
                 explorer_tx=explorer_tx, native_symbol=native_symbol, fee_hint=fee_hint,
                 tokens=parsed, testnet=bool(testnet))


def _custom_chains(custom, testnet: bool) -> tuple[dict[str, Chain], set[str]]:
    """(chains for this network keyed by key, keys only defined for the other network)."""
    builtin = get_chains(testnet)
    mine: dict[str, Chain] = {}
    other: set[str] = set()
    for chain in custom or ():
        if not isinstance(chain, Chain):
            raise ConfigError("custom chains must be created with scfg.add_chain()")
        if chain.key in get_chains(False) or chain.key in get_chains(True):
            raise ConfigError(f"chain {chain.key!r} is built in; use rpc_urls/confirmations "
                              "to change it, or pick another key")
        if chain.testnet != testnet:
            other.add(chain.key)
            continue
        if chain.key in mine:
            raise ConfigError(f"chain {chain.key!r} added twice")
        for c in list(builtin.values()) + list(mine.values()):
            if c.chain_id == chain.chain_id:
                raise ConfigError(f"chain {chain.key!r} has the same chain_id "
                                  f"({chain.chain_id}) as {c.key!r}")
        mine[chain.key] = chain
    return mine, other - set(mine)


def _logo(value) -> str | None:
    if value is None:
        return None
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or value.startswith("/") or ".." in value.split("/")):
        raise ConfigError("logo must be a path inside the game folder, "
                          f"like 'images/logo.png': {value!r}")
    logo_content_type(value)
    return value


def logo_content_type(path: str) -> str:
    ext = path[path.rfind("."):].lower() if "." in path else ""
    if ext not in LOGO_TYPES:
        raise ConfigError(f"logo must be a .png, .jpg or .webp image: {path!r}")
    return LOGO_TYPES[ext]


def _accent(value) -> str:
    if value is None:
        return DEFAULT_ACCENT
    if not isinstance(value, str) or not _HEX_COLOR.match(value):
        raise ConfigError(f"accent must be a color like '#0e7c74': {value!r}")
    digits = value[1:]
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    return "#" + digits[:6].lower()   # alpha, if any, is dropped


def _luminance(color: str) -> float:
    """WCAG relative luminance of a #rrggbb color."""
    def channel(i):
        c = int(color[i:i + 2], 16) / 255
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5)


def text_on(color: str) -> str:
    """White or ink, whichever has the higher WCAG contrast on `color` (#rrggbb)."""
    lum = _luminance(color)
    white = 1.05 / (lum + 0.05)
    ink = (lum + 0.05) / (_luminance(INK) + 0.05)
    return "#ffffff" if white >= ink else INK


def _merge_methods(methods: dict | None, chains: tuple[str, ...]) -> dict[str, dict]:
    merged = copy.deepcopy(DEFAULT_METHODS)
    for name, opts in (methods or {}).items():
        if name not in merged:
            raise ConfigError(f"unknown method: {name!r}")
        for key, value in opts.items():
            if key not in merged[name]:
                raise ConfigError(f"unknown option {key!r} for method {name!r}")
            merged[name][key] = value
    for name, opts in merged.items():
        if not isinstance(opts["enabled"], bool):
            raise ConfigError(f"methods.{name}.enabled must be True or False")
    timeout = merged["browser"]["timeout_min"]
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not timeout > 0:
        raise ConfigError("methods.browser.timeout_min must be a number of minutes above 0")
    if not any(o["enabled"] for o in merged.values()):
        raise ConfigError("at least one payment method must be enabled")
    qr_chains = merged["qr"]["chains"]
    if qr_chains is not None:
        for c in qr_chains:
            if c not in chains:
                raise ConfigError(f"methods.qr.chains contains disabled chain {c!r}")
    return merged


def build_config(wallet=None, *, testnet=False, chains=None, tokens=None, methods=None,
                 rpc_urls=None, confirmations=None, previous_wallets=None,
                 logo=None, accent=None, custom_chains=None, solana_wallet=None) -> Config:
    """custom_chains: Chains from build_chain(); those of the other network are ignored."""
    testnet = bool(testnet)
    custom, other_network = _custom_chains(custom_chains, testnet)
    registry = {**get_chains(testnet), **custom}
    if wallet is None and solana_wallet is None:
        raise ConfigError("configure() needs wallet= (an EVM 0x address) and/or solana_wallet=")
    wallet = _address(wallet, "wallet") if wallet is not None else None
    sol = _solana_address(solana_wallet, "solana_wallet") if solana_wallet is not None else None
    prev: list[str] = []
    prev_sol: list[str] = []
    for w in previous_wallets or ():
        family, address = _any_wallet(w, "previous_wallets")
        current, seen = (sol, prev_sol) if family == "solana" else (wallet, prev)
        if address != current and address not in seen:
            seen.append(address)

    solana_keys = tuple(k for k, c in registry.items() if c.family == "solana")
    chains = ((DISPLAY_ORDER + tuple(custom) if wallet else ()) + (solana_keys if sol else ())
              if chains is None else tuple(chains))
    if not chains:
        raise ConfigError("chains must not be empty")
    for c in chains:
        if c in other_network:
            net = "mainnet" if testnet else "testnet"
            raise ConfigError(f"chain {c!r} was only added for {net}; add it again with "
                              f"testnet={testnet} to use it here")
        if c not in registry:
            raise ConfigError(f"unknown chain: {c!r}")
        family = registry[c].family
        if family == "solana" and sol is None:
            raise ConfigError(f"chain {c!r} needs solana_wallet=")
        if family == "evm" and wallet is None:
            raise ConfigError(f"chain {c!r} needs wallet= (an EVM 0x address)")
    if len(set(chains)) != len(chains):
        raise ConfigError("duplicate chain in chains")

    tokens = tuple(TOKENS if tokens is None else tokens)
    if not tokens:
        raise ConfigError("tokens must not be empty")
    for t in tokens:
        if t not in TOKENS:
            raise ConfigError(f"unknown token: {t!r}")

    merged = _merge_methods(methods, chains)

    urls: dict[str, tuple[str, ...]] = {}
    for c, lst in (rpc_urls or {}).items():
        if c not in chains:
            raise ConfigError(f"rpc_urls for unknown or disabled chain {c!r}")
        urls[c] = _rpc_urls(lst, f"rpc_urls[{c!r}]")

    confs: dict[str, int] = {}
    for c, n in (confirmations or {}).items():
        if c not in chains:
            raise ConfigError(f"confirmations for unknown or disabled chain {c!r}")
        if registry[c].family == "solana":
            raise ConfigError("Solana payments wait for finalization; "
                              f"confirmations can't be set for {c!r}")
        if isinstance(n, bool) or not isinstance(n, int) or n < 1:
            raise ConfigError("confirmations must be an int >= 1")
        confs[c] = n

    cfg = Config(wallet=wallet, testnet=testnet, chains=chains, tokens=tokens,
                 methods=merged, rpc_urls=urls, confirmations=confs, previous_wallets=tuple(prev),
                 solana_wallet=sol, previous_solana_wallets=tuple(prev_sol),
                 logo=_logo(logo), accent=_accent(accent), custom_chains=custom)
    if not enabled_chains(cfg):
        raise ConfigError("no enabled chain supports any enabled token")
    return cfg


def enabled_chains(config: Config) -> dict[str, Chain]:
    """Chains the player may pay on, in display order, filtered to enabled tokens,
    with developer RPC URLs tried first and confirmation overrides applied."""
    registry = config.registry
    out: dict[str, Chain] = {}
    for key in config.chains:
        base = registry[key]
        tokens = {t: v for t, v in base.tokens.items() if t in config.tokens}
        if not tokens:
            continue
        out[key] = replace(
            base,
            tokens=tokens,
            rpc=config.rpc_urls.get(key, ()) + base.rpc,
            confirmations=config.confirmations.get(key, base.confirmations),
        )
    return out


def chains_for_token(config: Config, token: str) -> list[str]:
    return [k for k, c in enabled_chains(config).items() if token in c.tokens]


def add_item(config: Config, item_id, *, code, price, token, title, min_price=None) -> Item:
    if not isinstance(item_id, str) or not _ITEM_ID.match(item_id):
        raise ConfigError(f"invalid item id {item_id!r} (use a-z, 0-9, _ and -)")
    if item_id in config.items:
        raise ConfigError(f"duplicate item id {item_id!r}")
    if isinstance(code, bool) or not isinstance(code, int) or not 1 <= code <= 99:
        raise ConfigError("item code must be an int in 1..99")
    if config.item_by_code(code) is not None:
        raise ConfigError(f"duplicate item code {code}")
    try:
        p = parse_amount(price)
        mp = p if min_price is None else parse_amount(min_price)
    except (TypeError, ValueError) as e:
        raise ConfigError(f"item {item_id!r}: {e}") from None
    if mp > p:
        raise ConfigError("min_price must not exceed price")
    if token not in config.tokens or not chains_for_token(config, token):
        raise ConfigError(f"token {token!r} is not available on any enabled chain")
    if not isinstance(title, str) or not title.strip() or len(title) > 80:
        raise ConfigError("title must be a non-empty string of at most 80 characters")
    item = Item(item_id=item_id, code=code, price=p, token=token, title=title, min_price=mp)
    config.items[item_id] = item
    return item
