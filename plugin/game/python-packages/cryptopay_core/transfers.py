"""A token transfer to the developer, in a form shared by every chain family.

Receipts and restore candidates are built from it. EVM fills it from an ERC-20
Transfer log; Solana from a transaction's token-balance changes.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Transfer:
    token: str         # token contract (EVM) or mint (Solana)
    sender: str        # payer
    recipient: str     # developer wallet (the owner, never a token account)
    value: int         # base units
    tx_hash: str       # EVM: lowercase 0x hash; Solana: base58 signature, case preserved
    log_index: int     # EVM: log index; Solana: the developer token account's accountIndex
    block_number: int  # EVM: block number; Solana: slot
