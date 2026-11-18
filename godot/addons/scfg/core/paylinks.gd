## Payment links (EIP-681) and QR code geometry. Mirrors scfg_core/paylinks.py.
##
## The QR matrix comes from the GDScript port of the vendored Project Nayuki generator
## (qrcodegen.gd, from scfg_core/_qrcodegen.py). Rendering is left to the engine: `qr_rects`
## merges each row's dark modules into horizontal runs, so the screen draws a few hundred
## rectangles instead of thousands of single squares.
class_name ScfgPaylinks
extends RefCounted

## The buy page offered to players who have no stablecoin yet (configure's `onramp`): a template
## whose {placeholders} are filled in for the network the player picked. The default is
## thirdweb's hosted Buy page, which needs no account or key; the player connects their own wallet
## there and buys with a card, then pays as usual. tokenEditable=false keeps the page on the
## network and token the game asked for, and paymentMethods=card offers card payment only.
## tokenAddress is not in thirdweb's iframe docs, but the hosted page honours it (paylinks.py).
const ONRAMP_URL := "https://thirdweb.com/bridge/buy-widget?chain={chain_id}&tokenAddress={token_address}&amount={amount}&tokenEditable=false&paymentMethods=card"
const ONRAMP_FIELDS: Array[String] = ["amount", "chain", "chain_id", "token", "token_address"]
## 6-decimal units in a cent: the buy page's amount is rounded up to whole cents.
const UNITS_PER_CENT := 10_000
## On a chain whose gas coin is the token (Arc), the payment's fee is paid in it too: a cent.
const ONRAMP_GAS_UNITS := UNITS_PER_CENT
## Before a session exists: the most the item code and nonce add to the price (0.009999).
const ONRAMP_EXACT_UNITS := 9_999


## ERC-20 transfer request that mobile wallets understand when scanned or opened. Python's
## keyword arguments travel in `opts`: token_address, chain_id, recipient, units (an int or
## ScfgBigInt); a missing one is a TypeError. Values are formatted as Python's f-string does.
## @fallible
static func eip681_transfer_uri(opts: Dictionary) -> Variant:
	for key: String in ["token_address", "chain_id", "recipient", "units"]:
		if not opts.has(key):
			return ScfgErr.make("TypeError", "eip681_transfer_uri() missing 1 required keyword-only argument: '%s'" % key)
	return "ethereum:%s@%s/transfer?address=%s&uint256=%s" % [ScfgPy.py_str(opts["token_address"]),
		ScfgPy.py_str(opts["chain_id"]), ScfgPy.py_str(opts["recipient"]), ScfgPy.py_str(opts["units"])]


## The amount suggested on the buy page: the payment (6-decimal units), plus a cent for the fee
## where the token is the gas coin, rounded up to whole cents: 4993742 -> "5.00".
static func onramp_amount(units: int, gas_is_token: bool) -> String:
	var total := units + (ONRAMP_GAS_UNITS if gas_is_token else 0)
	var cents := total / UNITS_PER_CENT + (1 if total % UNITS_PER_CENT > 0 else 0)  # ceil
	return "%d.%02d" % [cents / 100, cents % 100]


## Python's ONRAMP_PLACEHOLDER (`\{([^{}]*)\}`) over `template`: the literal text and the
## placeholder names in turn, [text, name, text, ..., text].
static func onramp_split(template: String) -> PackedStringArray:
	var out := PackedStringArray()
	var start := 0
	var i := 0
	while i < template.length():
		if template[i] == "{":
			var j := i + 1
			while j < template.length() and template[j] != "{" and template[j] != "}":
				j += 1
			if j < template.length() and template[j] == "}":
				out.append(template.substr(start, i - start))
				out.append(template.substr(i + 1, j - i - 1))
				start = j + 1
				i = j
		i += 1
	out.append(template.substr(start))
	return out


## Fill a validated onramp template (ScfgConfig's onramp url). Every value is made of URL-safe
## characters (a chain key, digits, a 0x address, a token symbol, an amount), so none is escaped.
## A placeholder outside ONRAMP_FIELDS is a KeyError, as in Python.
## @fallible
static func onramp_url(template: String, chain: String, chain_id: Variant, token: String, token_address: String,
		amount: String) -> Variant:
	var values := {"amount": amount, "chain": chain, "chain_id": ScfgPy.py_str(chain_id), "token": token,
		"token_address": token_address}
	var parts := onramp_split(template)
	var out := parts[0]
	for n in range(1, parts.size(), 2):
		if not values.has(parts[n]):
			return ScfgErr.make("KeyError", ScfgPy.py_repr(parts[n]))
		out += values[parts[n]] + parts[n + 1]
	return out


## Square matrix of modules (an Array of rows, each an Array of bools), true = dark. Medium
## error correction. A non-str text is a TypeError; text too long for any version a
## DataTooLongError (a ValueError).
## @fallible
static func qr_matrix(text: Variant) -> Variant:
	if not (text is String or text is StringName):
		return ScfgErr.make("TypeError", "expected string or bytes-like object, got '%s'" % ScfgPy.py_type(text))
	var made: Variant = ScfgQrcodegen.QrCode.encode_text(str(text), ScfgQrcodegen.Ecc.MEDIUM)
	if made is ScfgErr:
		return made
	var qr: ScfgQrcodegen.QrCode = made
	var n := qr.get_size()
	var out := []
	for y in n:
		var row := []
		row.resize(n)
		for x in n:
			row[x] = qr.get_module(x, y)
		out.append(row)
	return out


## [size in modules including the quiet-zone border, [[x, y, width], ...] dark runs].
## @fallible
static func qr_rects(text: Variant, border: int = 2) -> Variant:
	var made: Variant = qr_matrix(text)
	if made is ScfgErr:
		return made
	var matrix: Array = made
	var n := matrix.size()
	var runs := []
	for y in n:
		var row: Array = matrix[y]
		var x := 0
		while x < n:
			if row[x]:
				var start := x
				while x < n and row[x]:
					x += 1
				runs.append([start + border, y + border, x - start])
			else:
				x += 1
	return [n + 2 * border, runs]
