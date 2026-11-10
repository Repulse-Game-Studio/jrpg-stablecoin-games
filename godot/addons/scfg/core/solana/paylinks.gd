## Solana Pay transfer-request links (https://docs.solanapay.com/spec). Mirrors
## scfg_core/solana/paylinks.py.
class_name ScfgSolanaPaylinks
extends RefCounted

## Python's urllib.parse._ALWAYS_SAFE: the bytes quote() never escapes.
const _ALWAYS_SAFE := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-~"
const _HEX := "0123456789ABCDEF"


## The amount always carries 6 decimals, so the item code in it survives. Python's keyword
## arguments travel in `opts`: recipient, units (an int or ScfgBigInt), mint, reference, label,
## message (a missing one is a TypeError). Parameters whose value is falsy (an empty label) are
## left out; every value is percent-encoded with `quote(v, safe="")`.
## @fallible
static func solana_pay_uri(opts: Dictionary) -> Variant:
	for key: String in ["recipient", "units", "mint", "reference", "label", "message"]:
		if not opts.has(key):
			return ScfgErr.make("TypeError", "solana_pay_uri() missing 1 required keyword-only argument: '%s'" % key)
	var amount: Variant = ScfgAmounts.format_units(opts["units"])
	if amount is ScfgErr:
		return amount
	var params := [["amount", amount], ["spl-token", opts["mint"]], ["reference", opts["reference"]],
		["label", opts["label"]], ["message", opts["message"]]]
	var parts := PackedStringArray()
	for kv: Array in params:
		if not ScfgPy.py_truthy(kv[1]):
			continue
		var q: Variant = quote(kv[1])
		if q is ScfgErr:
			return q
		parts.append("%s=%s" % [kv[0], q])
	var recipient: Variant = opts["recipient"]
	return "solana:%s?%s" % [str(recipient) if (recipient is String or recipient is StringName)
		else ScfgPy.py_repr(recipient), "&".join(parts)]


## Python `urllib.parse.quote(v, safe="")`: the UTF-8 bytes of a str (or the bytes of a
## PackedByteArray), every byte outside A-Z a-z 0-9 _ . - ~ written as %XX (uppercase hex). Any
## other type is a TypeError.
## @fallible
static func quote(v: Variant) -> Variant:
	var raw: PackedByteArray
	if v is String or v is StringName:
		raw = str(v).to_utf8_buffer()
	elif v is PackedByteArray:
		raw = v
	else:
		return ScfgErr.make("TypeError", "quote_from_bytes() expected bytes")
	var out := ""
	for b in raw:
		if b < 0x80 and _ALWAYS_SAFE.contains(String.chr(b)):
			out += String.chr(b)
		else:
			out += "%" + _HEX[b >> 4] + _HEX[b & 15]
	return out
