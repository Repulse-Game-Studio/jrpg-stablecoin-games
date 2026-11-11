## EVM helpers: addresses (EIP-55) and event topics. Mirrors scfg_core/evm.py.
##
## A TransferLog is a Dictionary with the fields of Python's dataclass, in its order: token,
## sender, recipient (checksummed), value (an ScfgBigInt, the raw logged uint256), tx_hash
## (lowercase), log_index, block_number (ints below 2^53), block_hash (lowercase).
class_name ScfgEvm
extends RefCounted

## "0x" + keccak256(b"Transfer(address,address,uint256)").hex(), pinned by test_evm.gd.
const TRANSFER_TOPIC := "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"

const TRANSFER_LOG_FIELDS: Array[String] = ["token", "sender", "recipient", "value", "tx_hash", "log_index",
		"block_number", "block_hash"]


## The EIP-55 mixed-case form of a 20-byte hex address (any input case). The whole string must
## be `0x` and 40 hex digits (Python's fullmatch): a non-string, a `0X` prefix, surrounding space,
## a trailing newline or a wrong length is a ValueError.
## @fallible
static func to_checksum_address(address: Variant) -> Variant:
	if not (address is String or address is StringName):
		return ScfgErr.make("ValueError", "not a 20-byte hex address: %s" % ScfgPy.py_repr(address))
	var text := str(address)
	if not _is_hex_word(text, 40):
		return ScfgErr.make("ValueError", "not a 20-byte hex address: %s" % ScfgPy.py_repr(address))
	var lower := text.substr(2).to_lower()
	var digest := ScfgBytes.bytes_to_hex(ScfgKeccak.keccak256(lower.to_ascii_buffer()))
	var out := "0x"
	for i in lower.length():
		var ch := lower[i]
		out += ch.to_upper() if digest.unicode_at(i) >= 0x38 else ch  # hex digit value >= 8
	return out


## True only if the address is already exactly in EIP-55 form. Never fails (Python: catches
## ValueError).
static func is_checksum_address(address: Variant) -> bool:
	var checksummed: Variant = to_checksum_address(address)
	if checksummed is ScfgErr:
		return false
	return checksummed == address


## Validate a user-supplied address and return its checksummed form. Single-case input carries
## no checksum, so it is accepted and converted; mixed-case input claims a checksum, so it must
## be correct.
## @fallible
static func normalize_address(address: Variant) -> Variant:
	var checksummed: Variant = to_checksum_address(address)
	if checksummed is ScfgErr:
		return checksummed
	var body := str(address).substr(2)
	if body == body.to_lower() or body == body.to_upper():
		return checksummed
	if checksummed != address:
		return ScfgErr.make("ValueError", "bad EIP-55 checksum: %s" % address)
	return checksummed


## The address left-padded to 32 bytes, as it appears in indexed event topics.
## @fallible
static func address_topic(address: Variant) -> Variant:
	var checksummed: Variant = to_checksum_address(address)
	if checksummed is ScfgErr:
		return checksummed
	return "0x" + "0".repeat(24) + (checksummed as String).substr(2).to_lower()


## Parse a raw JSON-RPC log (a Dictionary) into a TransferLog, or null (Python's None) if it
## is not an ERC-20 Transfer. ERC-721 Transfer events share topic0 but have 4 topics (the token
## id is indexed), so exactly 3 topics are required.
##
## Fields are read in Python's order, so the first error is Python's: a missing key is a
## KeyError; hex integers are `int(text, 16)` (a prefix, a sign, spaces and underscores as
## Python takes them; a non-string is a TypeError); a transactionHash/blockHash/topic that is
## not a string is a TypeError (Python would raise AttributeError on `.lower()`; the M1 port
## made it a TypeError, and a value must never pass through unconverted); a `log` that is not
## an object is an AttributeError. logIndex and blockNumber of 2^53 or more are a ValueError:
## they would not survive as an exact JSON number. A negative `data` is an OverflowError (an
## ScfgBigInt is unsigned; no chain emits one).
## @fallible
static func parse_transfer_log(log: Variant) -> Variant:
	if not log is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'get'" % ScfgPy.py_type(log))
	var d: Dictionary = log
	var topics: Variant = d.get("topics")
	if not ScfgPy.py_truthy(topics):
		topics = []
	if ScfgPy.py_truthy(d.get("removed")):
		return null
	if not topics is Array:
		return ScfgErr.make("TypeError", "log topics must be a list, not '%s'" % ScfgPy.py_type(topics))
	var tl: Array = topics
	if tl.size() != 3:
		return null
	var topic0: Variant = _lower_string(tl[0], "topic")
	if topic0 is ScfgErr:
		return topic0
	if topic0 != TRANSFER_TOPIC:
		return null
	var data: Variant = d.get("data")
	if not (data is String or data is StringName) or not _is_hex_word(str(data), 64):
		return null  # not exactly the 32-byte value ("0x" and 64 hex digits): not an ERC-20 Transfer

	var address: Variant = ScfgPy.py_get(d, "address")
	if address is ScfgErr:
		return address
	var token: Variant = to_checksum_address(address)
	if token is ScfgErr:
		return token
	var sender: Variant = _topic_to_address(tl[1])
	if sender is ScfgErr:
		return sender
	var recipient: Variant = _topic_to_address(tl[2])
	if recipient is ScfgErr:
		return recipient
	var value: Variant = _value(data)
	if value is ScfgErr:
		return value
	var tx_hash: Variant = _hash_field(d, "transactionHash")
	if tx_hash is ScfgErr:
		return tx_hash
	var log_index: Variant = _safe_hex_int(d, "logIndex")
	if log_index is ScfgErr:
		return log_index
	var block_number: Variant = _safe_hex_int(d, "blockNumber")
	if block_number is ScfgErr:
		return block_number
	var block_hash: Variant = _hash_field(d, "blockHash")
	if block_hash is ScfgErr:
		return block_hash
	return {
		"token": token,
		"sender": sender,
		"recipient": recipient,
		"value": value,
		"tx_hash": tx_hash,
		"log_index": log_index,
		"block_number": block_number,
		"block_hash": block_hash,
	}


## `to_checksum_address("0x" + topic[-40:])`: the last 40 characters of an indexed topic.
## @fallible
static func _topic_to_address(topic: Variant) -> Variant:
	if not (topic is String):
		return ScfgErr.make("TypeError", "log topic must be a string, not '%s'" % ScfgPy.py_type(topic))
	var text: String = topic
	return to_checksum_address("0x" + text.substr(maxi(0, text.length() - 40)))


## `value=int(data, 16)`, as an ScfgBigInt. `data` is one 32-byte word (_is_hex_word).
## @fallible
static func _value(data: Variant) -> Variant:
	var n: Variant = ScfgPy.py_hex_int(data)
	if n is ScfgErr:
		return n
	if n is ScfgBigInt:
		return n
	if n < 0:
		return ScfgErr.make("OverflowError", "negative log value %d is not supported by the port" % n)
	return ScfgBigInt.from_int(n)


## `log[key].lower()`, the key a required string.
## @fallible
static func _hash_field(d: Dictionary, key: String) -> Variant:
	var v: Variant = ScfgPy.py_get(d, key)
	if v is ScfgErr:
		return v
	return _lower_string(v, "log " + key)


## `v.lower()`, with a non-string a TypeError.
## @fallible
static func _lower_string(v: Variant, what: String) -> Variant:
	if not (v is String):
		return ScfgErr.make("TypeError", "%s must be a string, not '%s'" % [what, ScfgPy.py_type(v)])
	return (v as String).to_lower()


## `int(log[key], 16)` as an int below 2^53 (ScfgPy.TWO_53): log_index and block_number must stay
## exact as an int and as a JSON number.
## @fallible
static func _safe_hex_int(d: Dictionary, key: String) -> Variant:
	var v: Variant = ScfgPy.py_get(d, key)
	if v is ScfgErr:
		return v
	var n: Variant = ScfgPy.py_hex_int(v)
	if n is ScfgErr:
		return n
	if n is ScfgBigInt or n >= ScfgPy.TWO_53:
		return ScfgErr.make("ValueError", "log %s exceeds 2^53: %s" % [key, ScfgPy.py_repr(v)])
	return n


## Whether `text` is exactly `0x` and `digits` hex digits (either case).
static func _is_hex_word(text: String, digits: int) -> bool:
	if text.length() != digits + 2 or not text.begins_with("0x"):
		return false
	for i in range(2, digits + 2):
		if not _is_hex_char(text.unicode_at(i)):
			return false
	return true


static func _is_hex_char(c: int) -> bool:
	return (c >= 0x30 and c <= 0x39) or (c >= 0x61 and c <= 0x66) or (c >= 0x41 and c <= 0x46)
