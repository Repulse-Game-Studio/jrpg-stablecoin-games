"""EVM helpers: addresses (EIP-55) and event topics."""

from __future__ import annotations

import re
from dataclasses import dataclass

from .keccak import keccak256
from .transfers import Transfer

_HEX40 = re.compile(r"^0x[0-9a-fA-F]{40}$")

TRANSFER_TOPIC = "0x" + keccak256(b"Transfer(address,address,uint256)").hex()


def to_checksum_address(address: str) -> str:
    """Return the EIP-55 mixed-case form of a 20-byte hex address (any input case)."""
    if not isinstance(address, str) or not _HEX40.match(address):
        raise ValueError(f"not a 20-byte hex address: {address!r}")
    lower = address[2:].lower()
    digest = keccak256(lower.encode("ascii")).hex()
    return "0x" + "".join(
        char.upper() if int(digest[i], 16) >= 8 else char for i, char in enumerate(lower)
    )


def is_checksum_address(address: str) -> bool:
    """True only if the address is already exactly in EIP-55 form. Never raises."""
    try:
        return to_checksum_address(address) == address
    except ValueError:
        return False


def normalize_address(address: str) -> str:
    """Validate a user-supplied address and return its checksummed form.

    Single-case input carries no checksum, so it is accepted and converted.
    Mixed-case input claims a checksum, so it must be correct.
    """
    checksummed = to_checksum_address(address)
    body = address[2:]
    if body == body.lower() or body == body.upper():
        return checksummed
    if checksummed != address:
        raise ValueError(f"bad EIP-55 checksum: {address}")
    return checksummed


def address_topic(address: str) -> str:
    """The address left-padded to 32 bytes, as it appears in indexed event topics."""
    return "0x" + "0" * 24 + to_checksum_address(address)[2:].lower()


@dataclass(frozen=True)
class TransferLog:
    """One ERC-20 Transfer event, normalized."""

    token: str  # checksummed contract address
    sender: str  # checksummed
    recipient: str  # checksummed
    value: int  # base units
    tx_hash: str  # lowercase
    log_index: int
    block_number: int
    block_hash: str  # lowercase

    def to_transfer(self) -> Transfer:
        return Transfer(self.token, self.sender, self.recipient, self.value, self.tx_hash,
                        self.log_index, self.block_number)


def _topic_to_address(topic: str) -> str:
    return to_checksum_address("0x" + topic[-40:])


def parse_transfer_log(log: dict) -> TransferLog | None:
    """Parse a raw JSON-RPC log into a TransferLog, or None if it isn't an ERC-20 Transfer.

    ERC-721 Transfer events share topic0 but have 4 topics (the token id is indexed),
    so requiring exactly 3 topics filters them out.
    """
    topics = log.get("topics") or []
    if log.get("removed"):
        return None
    if len(topics) != 3 or topics[0].lower() != TRANSFER_TOPIC:
        return None
    data = log.get("data") or "0x"
    return TransferLog(
        token=to_checksum_address(log["address"]),
        sender=_topic_to_address(topics[1]),
        recipient=_topic_to_address(topics[2]),
        value=int(data, 16) if data not in ("0x", "") else 0,
        tx_hash=log["transactionHash"].lower(),
        log_index=int(log["logIndex"], 16),
        block_number=int(log["blockNumber"], 16),
        block_hash=log["blockHash"].lower(),
    )
