## Payment links (EIP-681) and QR code geometry. Mirrors scfg_core/paylinks.py.
##
## The QR matrix comes from the GDScript port of the vendored Project Nayuki generator
## (qrcodegen.gd, from scfg_core/_qrcodegen.py). Rendering is left to the engine: `qr_rects`
## merges each row's dark modules into horizontal runs, so the screen draws a few hundred
## rectangles instead of thousands of single squares.
class_name ScfgPaylinks
extends RefCounted


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
