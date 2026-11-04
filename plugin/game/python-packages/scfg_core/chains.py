"""Registry of supported chains and stablecoin contracts.

Token addresses come from Circle's and Tether's official contract lists and are
pinned by tests. Solana mints come from those same lists and are compared exactly
(base58), never case-folded. Tokens are always identified by contract address, never
by symbol, so a look-alike token called "USDC" can never be mistaken for the real one.
Testnet entries use the same keys so a game can switch with ``testnet=True``.

Developers can add further EVM chains of their own (config.build_chain, exposed as
scfg.add_chain); those never enter these dicts, they live on the Config.

RPC lists and max_log_range values were measured against the live public
endpoints (tools/check_rpcs.py). The first endpoint should be the most generous.

Log sources: on most chains a token's Transfer events come from its own contract. On
Arc, USDC is the native gas coin: a plain native send logs a Transfer only at a system
emitter address (18 decimals), while an ERC-20 transfer() logs there *and* at the 0x3600
interface (6 decimals). Such a token names the emitter as its log_address, and
Chain.parse_transfer reads only that stream, floored to 6-decimal units, so every
payment is seen exactly once however the player sent it.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace

from .evm import TransferLog, parse_transfer_log, to_checksum_address
from .solana.base58 import normalize_pubkey

ARC_USDC = "0x3600000000000000000000000000000000000000"
ARC_SYSTEM_EMITTER = "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE"
ARC_MIN_FEE_WEI = 20_000_000_000  # Arc's mempool silently drops anything below 20 gwei


@dataclass(frozen=True)
class Token:
    address: str
    decimals: int = 6
    # Where this token's Transfer events are logged, when that is not its own contract
    # (Arc's system emitter), and the decimals of the values logged there.
    log_address: str = ""
    log_decimals: int = 0

    def __post_init__(self):
        # EVM addresses are always EIP-55 checksummed, so comparisons with parsed logs never
        # depend on case. Solana mints are base58 and compared exactly.
        if self.address.startswith("0x"):
            object.__setattr__(self, "address", to_checksum_address(self.address))
        else:
            object.__setattr__(self, "address", normalize_pubkey(self.address))
        if self.log_address:
            object.__setattr__(self, "log_address", to_checksum_address(self.log_address))
            if self.log_decimals < self.decimals:
                raise ValueError("log_decimals must be at least decimals")

    @property
    def log_source(self) -> str:
        """The contract address whose Transfer events record this token's payments."""
        return self.log_address or self.address


@dataclass(frozen=True)
class Chain:
    key: str
    chain_id: int
    name: str
    rpc: tuple[str, ...]
    confirmations: int
    max_log_range: int
    explorer_tx: str
    native_symbol: str
    fee_hint: str
    tokens: dict[str, Token] = field(default_factory=dict)
    testnet: bool = False
    family: str = "evm"
    wallet_chain: str = ""
    native_token: str = ""  # the token in `tokens` that is also the gas coin (Arc: "USDC")
    min_fee_wei: int = 0  # maxFeePerGas floor the checkout page enforces (0: none)

    def explorer_url(self, tx_hash: str) -> str:
        return self.explorer_tx.format(tx_hash)

    def parse_transfer(self, log: dict) -> TransferLog | None:
        """A raw log as a payment in one of this chain's tokens, or None.

        Only logs from a token's log source count. Values logged with more decimals than
        the token has are floored to its units; a log that floors to 0 is not a payment.
        The result always carries the token's canonical address.
        """
        t = parse_transfer_log(log)
        if t is None:
            return None
        token = next((tk for tk in self.tokens.values() if tk.log_source == t.token), None)
        if token is None:
            return None
        if not token.log_address:
            return t
        value = t.value // 10 ** (token.log_decimals - token.decimals)
        if value <= 0:
            return None
        return replace(t, token=token.address, value=value)


MAINNET: dict[str, Chain] = {
    "ethereum": Chain(
        key="ethereum", chain_id=1, name="Ethereum",
        rpc=("https://ethereum-rpc.publicnode.com", "https://eth.drpc.org"),
        confirmations=12, max_log_range=125,
        explorer_tx="https://etherscan.io/tx/{}", native_symbol="ETH", fee_hint="high",
        tokens={
            "USDC": Token("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"),
            "USDT": Token("0xdAC17F958D2ee523a2206206994597C13D831ec7"),
        },
    ),
    "arbitrum": Chain(
        key="arbitrum", chain_id=42161, name="Arbitrum",
        # publicnode only serves very recent Arbitrum blocks without a token: last resort.
        rpc=("https://arb1.arbitrum.io/rpc", "https://arbitrum.drpc.org", "https://1rpc.io/arb",
             "https://arbitrum-one-rpc.publicnode.com"),
        confirmations=20, max_log_range=2000,
        explorer_tx="https://arbiscan.io/tx/{}", native_symbol="ETH", fee_hint="low",
        tokens={
            "USDC": Token("0xaf88d065e77c8cC2239327C5EDb3A432268e5831"),
            "USDT": Token("0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9"),
        },
    ),
    "polygon": Chain(
        key="polygon", chain_id=137, name="Polygon",
        rpc=("https://polygon-bor-rpc.publicnode.com", "https://polygon.drpc.org",
             "https://1rpc.io/matic"),
        confirmations=32, max_log_range=500,
        explorer_tx="https://polygonscan.com/tx/{}", native_symbol="POL", fee_hint="lowest",
        tokens={
            "USDC": Token("0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359"),
            "USDT": Token("0xc2132D05D31c914a87C6611C10748AEb04B58e8F"),
        },
    ),
    "base": Chain(
        key="base", chain_id=8453, name="Base",
        rpc=("https://mainnet.base.org", "https://base-rpc.publicnode.com", "https://base.drpc.org",
             "https://1rpc.io/base"),
        confirmations=10, max_log_range=125,
        explorer_tx="https://basescan.org/tx/{}", native_symbol="ETH", fee_hint="low",
        tokens={"USDC": Token("0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913")},
    ),
    "arc": Chain(
        key="arc", chain_id=5042, name="Arc",
        # dRPC caps getLogs at a few dozen blocks, so it is never first, and never last
        # either: the last endpoint is the one handed to a wallet that adds the network.
        rpc=("https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io",
             "https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io"),
        confirmations=1, max_log_range=9_999,  # BFT finality: one block is final
        explorer_tx="https://explorer.arc.io/tx/{}", native_symbol="USDC", fee_hint="lowest",
        native_token="USDC", min_fee_wei=ARC_MIN_FEE_WEI,
        tokens={"USDC": Token(ARC_USDC, log_address=ARC_SYSTEM_EMITTER, log_decimals=18)},
    ),
    "solana": Chain(
        key="solana", chain_id=0, name="Solana",
        # publicnode answers fast but keeps ~2 days of history; mainnet-beta keeps all of it.
        rpc=("https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"),
        confirmations=32, max_log_range=0,
        explorer_tx="https://solscan.io/tx/{}", native_symbol="SOL", fee_hint="lowest",
        family="solana", wallet_chain="solana:mainnet",
        tokens={
            "USDC": Token("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
            "USDT": Token("Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"),
        },
    ),
}

TESTNET: dict[str, Chain] = {
    "ethereum": Chain(
        key="ethereum", chain_id=11155111, name="Sepolia",
        rpc=("https://ethereum-sepolia-rpc.publicnode.com", "https://1rpc.io/sepolia"),
        confirmations=3, max_log_range=2000,
        explorer_tx="https://sepolia.etherscan.io/tx/{}", native_symbol="ETH", fee_hint="high",
        testnet=True,
        tokens={"USDC": Token("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238")},
    ),
    "arbitrum": Chain(
        key="arbitrum", chain_id=421614, name="Arbitrum Sepolia",
        rpc=("https://sepolia-rollup.arbitrum.io/rpc", "https://arbitrum-sepolia-rpc.publicnode.com"),
        confirmations=3, max_log_range=2000,
        explorer_tx="https://sepolia.arbiscan.io/tx/{}", native_symbol="ETH", fee_hint="low",
        testnet=True,
        tokens={"USDC": Token("0x75faf114eafb1BDbe2F0316DF893fd58CE46AA4d")},
    ),
    "polygon": Chain(
        key="polygon", chain_id=80002, name="Polygon Amoy",
        rpc=("https://polygon-amoy-bor-rpc.publicnode.com",),
        confirmations=3, max_log_range=2000,
        explorer_tx="https://amoy.polygonscan.com/tx/{}", native_symbol="POL", fee_hint="lowest",
        testnet=True,
        tokens={"USDC": Token("0x41E94Eb019C0762f9Bfcf9Fb1E58725BfB0e7582")},
    ),
    "base": Chain(
        key="base", chain_id=84532, name="Base Sepolia",
        rpc=("https://base-sepolia-rpc.publicnode.com", "https://sepolia.base.org"),
        confirmations=3, max_log_range=2000,
        explorer_tx="https://sepolia.basescan.org/tx/{}", native_symbol="ETH", fee_hint="low",
        testnet=True,
        tokens={"USDC": Token("0x036CbD53842c5426634e7929541eC2318f3dCF7e")},
    ),
    "arc": Chain(
        key="arc", chain_id=5042002, name="Arc Testnet",
        # Blockdaemon's testnet endpoint answers gzip unasked (urllib can't read it): left out.
        rpc=("https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io",
             "https://rpc.quicknode.testnet.arc.io"),
        confirmations=1, max_log_range=9_999,
        explorer_tx="https://explorer.testnet.arc.io/tx/{}", native_symbol="USDC",
        fee_hint="lowest", testnet=True, native_token="USDC", min_fee_wei=ARC_MIN_FEE_WEI,
        tokens={"USDC": Token(ARC_USDC, log_address=ARC_SYSTEM_EMITTER, log_decimals=18)},
    ),
    "solana": Chain(
        key="solana", chain_id=0, name="Solana Devnet",
        rpc=("https://api.devnet.solana.com",),
        confirmations=32, max_log_range=0,
        explorer_tx="https://solscan.io/tx/{}?cluster=devnet", native_symbol="SOL",
        fee_hint="lowest", testnet=True, family="solana", wallet_chain="solana:devnet",
        tokens={"USDC": Token("4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU")},
    ),
}

DISPLAY_ORDER: tuple[str, ...] = ("arbitrum", "base", "arc", "polygon", "ethereum")


def get_chains(testnet: bool) -> dict[str, Chain]:
    return TESTNET if testnet else MAINNET
