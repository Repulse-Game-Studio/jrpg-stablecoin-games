"""Legacy Solana transactions, serialized without any library.

Message layout: header (3 bytes) | compact-u16 #keys | keys (32 bytes each) |
recent blockhash (32) | compact-u16 #instructions | per instruction: program index,
compact-u16 #accounts, account indexes, compact-u16 #data, data.
Account order (as the Solana SDK compiles it): fee payer, then writable signers,
readonly signers, writable non-signers, readonly non-signers, each group sorted by
raw key bytes. Program ids are readonly non-signers.
"""

from __future__ import annotations

from dataclasses import dataclass

from .base58 import decode_fixed
from .pda import TOKEN_PROGRAM_ID

COMPUTE_BUDGET_PROGRAM_ID = "ComputeBudget111111111111111111111111111111"


@dataclass(frozen=True)
class AccountMeta:
    pubkey: str
    is_signer: bool
    is_writable: bool


@dataclass(frozen=True)
class Instruction:
    program_id: str
    accounts: tuple[AccountMeta, ...]
    data: bytes


def compact_u16(n: int) -> bytes:
    if not 0 <= n <= 0xFFFF:
        raise ValueError(f"compact-u16 out of range: {n}")
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def compile_message(payer: str, instructions: list[Instruction], recent_blockhash: str) -> bytes:
    flags: dict[bytes, list] = {}

    def add(key: str, signer: bool, writable: bool) -> None:
        f = flags.setdefault(decode_fixed(key, 32), [False, False])
        f[0] = f[0] or signer
        f[1] = f[1] or writable

    for ix in instructions:
        for meta in ix.accounts:
            add(meta.pubkey, meta.is_signer, meta.is_writable)
        add(ix.program_id, False, False)
    payer_raw = decode_fixed(payer, 32)
    flags.pop(payer_raw, None)

    def group(signer: bool, writable: bool) -> list:
        return sorted(k for k, (s, w) in flags.items() if s == signer and w == writable)

    ws, rs, wu, ru = group(True, True), group(True, False), group(False, True), group(False, False)
    keys = [payer_raw] + ws + rs + wu + ru
    index = {k: i for i, k in enumerate(keys)}
    out = bytearray([1 + len(ws) + len(rs), len(rs), len(ru)])
    out += compact_u16(len(keys)) + b"".join(keys) + decode_fixed(recent_blockhash, 32)
    out += compact_u16(len(instructions))
    for ix in instructions:
        out.append(index[decode_fixed(ix.program_id, 32)])
        out += compact_u16(len(ix.accounts))
        out += bytes(index[decode_fixed(m.pubkey, 32)] for m in ix.accounts)
        out += compact_u16(len(ix.data)) + ix.data
    return bytes(out)


def unsigned_transaction(message: bytes) -> bytes:
    """Empty signature slots + message, ready for a wallet to sign."""
    signers = message[0]
    return compact_u16(signers) + bytes(64 * signers) + message


def set_compute_unit_limit(units: int) -> Instruction:
    return Instruction(COMPUTE_BUDGET_PROGRAM_ID, (), bytes([2]) + units.to_bytes(4, "little"))


def set_compute_unit_price(micro_lamports: int) -> Instruction:
    return Instruction(COMPUTE_BUDGET_PROGRAM_ID, (),
                       bytes([3]) + micro_lamports.to_bytes(8, "little"))


def transfer_checked(*, source: str, mint: str, destination: str, owner: str, amount: int,
                     decimals: int, extra=()) -> Instruction:
    """SPL Token TransferChecked; `extra` keys (e.g. a Solana Pay reference) are appended
    read-only so the payment can be found with getSignaturesForAddress(extra key)."""
    accounts = (AccountMeta(source, False, True), AccountMeta(mint, False, False),
                AccountMeta(destination, False, True), AccountMeta(owner, True, False))
    accounts += tuple(AccountMeta(k, False, False) for k in extra)
    data = bytes([12]) + amount.to_bytes(8, "little") + bytes([decimals])
    return Instruction(TOKEN_PROGRAM_ID, accounts, data)
