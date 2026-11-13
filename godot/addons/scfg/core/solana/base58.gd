## Base58 (Bitcoin alphabet), as used for Solana addresses and signatures. Mirrors
## scfg_core/solana/base58.py.
class_name ScfgSolanaBase58
extends RefCounted

const ALPHABET := "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


## Base58 text of `data`: each leading zero byte is a "1", the rest is the big-endian number.
static func b58encode(data: PackedByteArray) -> String:
	var zeros := 0
	while zeros < data.size() and data[zeros] == 0:
		zeros += 1
	# schoolbook base conversion: `digits` holds base-58 digits, least significant first
	var digits := PackedInt32Array()
	for i in range(zeros, data.size()):
		var carry: int = data[i]
		for j in digits.size():
			carry += digits[j] << 8
			digits[j] = carry % 58
			carry /= 58
		while carry > 0:
			digits.append(carry % 58)
			carry /= 58
	var out := "1".repeat(zeros)
	for j in range(digits.size() - 1, -1, -1):
		out += ALPHABET[digits[j]]
	return out


## The bytes of base58 `text`. A non-string, an empty string or a character outside the alphabet
## is a ValueError. Each leading "1" is a zero byte.
## @fallible
static func b58decode(text: Variant) -> Variant:
	if not (text is String or text is StringName) or str(text) == "":
		return ScfgErr.make("ValueError", "not base58: %s" % ScfgPy.py_repr(text))
	var s := str(text)
	var zeros := 0
	while zeros < s.length() and s[zeros] == "1":
		zeros += 1
	# `bytes` holds the number's bytes, least significant first
	var bytes := PackedInt32Array()
	for i in s.length():
		var v := ALPHABET.find(s[i])
		if v < 0:
			return ScfgErr.make("ValueError", "invalid base58 character %s" % ScfgPy.py_repr(s[i]))
		var carry := v
		for j in bytes.size():
			carry += bytes[j] * 58
			bytes[j] = carry & 0xff
			carry >>= 8
		while carry > 0:
			bytes.append(carry & 0xff)
			carry >>= 8
	var out := PackedByteArray()
	out.resize(zeros)
	out.fill(0)
	# the number's own bytes follow the zeros; "1"s only add zeros, so they contribute no bytes
	# (a leading zero digit leaves `bytes` empty, which is why the zeros are counted apart)
	for j in range(bytes.size() - 1, -1, -1):
		out.append(bytes[j])
	return out


## The longest base58 text of `length` bytes: 44 characters for 32, 88 for 64 (Python's
## max_encoded_length: ceil(length * log(256) / log(58))).
static func max_encoded_length(length: float) -> int:
	return ceili(length * log(256.0) / log(58.0))


## `b58decode(text)` that must be exactly `length` bytes (else ValueError). A JSON `length`
## arrives as a float, so any number equal to the byte count passes; anything else is a ValueError
## (Python: a length that does not equal `len(raw)`).
## Decoding is quadratic in the text's length, so text longer than any encoding of `length`
## bytes (max_encoded_length) is refused before it is decoded.
## @fallible
static func decode_fixed(text: Variant, length: Variant) -> Variant:
	var numeric := (length is int or length is float) and typeof(length) != TYPE_BOOL
	if numeric and (text is String or text is StringName) and str(text).length() > max_encoded_length(length):
		return ScfgErr.make("ValueError", "too long for %s bytes: %d characters" % [ScfgPy.py_repr(length), str(text).length()])
	var raw: Variant = b58decode(text)
	if raw is ScfgErr:
		return raw
	var bytes: PackedByteArray = raw
	if not numeric or bytes.size() != length:
		return ScfgErr.make("ValueError", "expected %s bytes, got %d: %s" % [
			ScfgPy.py_repr(length), bytes.size(), ScfgPy.py_repr(text)])
	return bytes


## A base58 address, stripped (Python `str.strip`) and re-encoded: exactly 32 bytes, else ValueError.
## @fallible
static func normalize_pubkey(text: Variant) -> Variant:
	if not (text is String or text is StringName):
		return ScfgErr.make("ValueError", "not an address: %s" % ScfgPy.py_repr(text))
	var raw: Variant = decode_fixed(ScfgPy.py_strip(str(text)), 32)
	if raw is ScfgErr:
		return raw
	return b58encode(raw)


## A base58 transaction signature, stripped and re-encoded: exactly 64 bytes, else ValueError.
## @fallible
static func normalize_signature(text: Variant) -> Variant:
	if not (text is String or text is StringName):
		return ScfgErr.make("ValueError", "not a signature: %s" % ScfgPy.py_repr(text))
	var raw: Variant = decode_fixed(ScfgPy.py_strip(str(text)), 64)
	if raw is ScfgErr:
		return raw
	return b58encode(raw)
