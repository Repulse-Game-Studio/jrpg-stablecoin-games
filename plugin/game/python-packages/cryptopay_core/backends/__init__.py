"""Chain-family backends: everything that differs between EVM chains and Solana.

The engine, sessions, receipts and screens are family-neutral. Anything that touches a
chain's RPC, transaction-id format or payment matching goes through backend_for(chain).
Backends are created lazily, so importing this package never imports a backend module
(backends import restore/reverify, which import this package).
"""

from __future__ import annotations

_BACKENDS: dict = {}


def _load() -> dict:
    if not _BACKENDS:
        from .evm import EvmBackend
        from .solana import SolanaBackend

        _BACKENDS["evm"] = EvmBackend()
        _BACKENDS["solana"] = SolanaBackend()
    return _BACKENDS


def backend_for_family(family: str):
    try:
        return _load()[family]
    except KeyError:
        raise ValueError(f"unknown chain family {family!r}") from None


def backend_for(chain):
    return backend_for_family(chain.family)


def normalize_any_tx(text: str) -> str:
    """A transaction id in any supported family's format, normalized."""
    for backend in _load().values():
        try:
            return backend.normalize_tx(text)
        except ValueError:
            continue
    raise ValueError(f"not a transaction hash or signature: {text!r}")
