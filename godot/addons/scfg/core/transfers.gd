## A token transfer to the developer, in a form shared by every chain family. Mirrors
## scfg_core/transfers.py.
##
## Receipts and restore candidates are built from it. EVM fills it from an ERC-20 Transfer log
## (ScfgEvm.parse_transfer_log builds the TransferLog form, which adds block_hash); Solana from a
## transaction's token-balance changes.
##
## A Transfer is a Dictionary with Python's dataclass fields, in order:
## - token: token contract (EVM) or mint (Solana)
## - sender: payer
## - recipient: developer wallet (the owner, never a token account)
## - value: base units (an int or ScfgBigInt)
## - tx_hash: EVM: lowercase 0x hash; Solana: base58 signature, case preserved
## - log_index: EVM: log index; Solana: the developer token account's accountIndex
## - block_number: EVM: block number; Solana: slot
class_name ScfgTransfers
extends RefCounted

const TRANSFER_FIELDS: Array[String] = ["token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number"]


## Python `Transfer(**fields)`: every field is required and no other is allowed (TypeError).
## @fallible
static func make_transfer(fields: Dictionary) -> Variant:
	return ScfgModels.dataclass("Transfer", TRANSFER_FIELDS, {}, fields)
