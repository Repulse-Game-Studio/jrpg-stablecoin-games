## Hex, UTF-8, base64 and concatenation helpers for PackedByteArray. No Python twin: Python
## uses `bytes.fromhex`, `.hex()`, `.encode()` and `base64` directly.
class_name ScfgBytes
extends RefCounted


## Hex (either case, an optional lowercase `0x` prefix) to bytes. Odd length or a non-hex
## character (whitespace included) is a ValueError; a non-string is a TypeError.
## @fallible
static func hex_to_bytes(s: Variant) -> Variant:
	if not (s is String or s is StringName):
		return ScfgErr.make("TypeError", "expected a hex string, not '%s'" % ScfgPy.py_type(s))
	var body := str(s)
	if body.begins_with("0x"):
		body = body.substr(2)
	if body.length() % 2 != 0:
		return ScfgErr.make("ValueError", "odd-length hex: %s" % ScfgPy.py_repr(s))
	for i in body.length():
		var c := body.unicode_at(i)
		if not ((c >= 0x30 and c <= 0x39) or (c >= 0x61 and c <= 0x66) or (c >= 0x41 and c <= 0x46)):
			return ScfgErr.make("ValueError", "non-hex character in %s" % ScfgPy.py_repr(s))
	return body.hex_decode()


## Lowercase hex without a prefix (Python `b.hex()`).
static func bytes_to_hex(b: PackedByteArray) -> String:
	return b.hex_encode()


## UTF-8 bytes of a string (Python `s.encode()`). A Godot String cannot hold a lone surrogate
## (it becomes U+FFFD on the way in), so unlike Python this cannot fail.
static func utf8(s: String) -> PackedByteArray:
	return s.to_utf8_buffer()


## Standard base64 with `=` padding (Python `base64.b64encode(b).decode("ascii")`).
static func base64(b: PackedByteArray) -> String:
	return Marshalls.raw_to_base64(b) if not b.is_empty() else ""


## The concatenation of an Array of PackedByteArray.
static func concat(parts: Array) -> PackedByteArray:
	var out := PackedByteArray()
	for p: PackedByteArray in parts:
		out.append_array(p)
	return out
