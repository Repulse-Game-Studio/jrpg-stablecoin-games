"""Shared, deterministic fixtures for scenario modules."""

from __future__ import annotations

import hashlib

from scfg_core.solana.base58 import b58encode

from ..tier1 import _EIP55
from ..tier1_config import SOL, SOL2, SOL3

DEV_EVM, PAYER_EVM, OLD_DEV_EVM = _EIP55[0], _EIP55[1], _EIP55[2]  # EIP-55 checksummed
DEV_SOL, PAYER_SOL, OLD_DEV_SOL = SOL, SOL2, SOL3                  # on-curve wallets

CHAPTER2 = {"item_id": "chapter2", "code": 37, "price": "4.99", "token": "USDC",
            "title": "Chapter 2"}


def tx_hash(n: int) -> str:
    """A well-formed lowercase EVM transaction hash, distinct per n."""
    return "0x" + hashlib.sha256(f"tx {n}".encode()).hexdigest()


def signature(n: int) -> str:
    """A well-formed base58 Solana signature (64 bytes), distinct per n."""
    return b58encode(hashlib.sha512(f"sig {n}".encode()).digest())


def covers(unit_file: str, *names: str) -> list[str]:
    """Pytest ids of tests in tests/unit/test_<unit_file>.py."""
    return [f"tests/unit/test_{unit_file}.py::test_{n}" for n in names]


AVAX_USDC = "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E"
# tests/unit/test_custom_chains.py avalanche(): a developer-added EVM chain (build_chain kwargs)
AVALANCHE = {"key": "avalanche", "name": "Avalanche", "chain_id": 43114,
             "rpc": ["https://my-node.example/avax", "https://api.avax.network/ext/bc/C/rpc"],
             "tokens": {"USDC": AVAX_USDC}, "confirmations": 5,
             "explorer_tx": "https://snowtrace.io/tx/{}", "native_symbol": "AVAX"}
