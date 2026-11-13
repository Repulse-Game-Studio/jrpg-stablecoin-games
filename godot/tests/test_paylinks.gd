extends RefCounted

const T := preload("res://tests/t.gd")


## sha256 of the matrix rows ("0"/"1" per module) joined by "\n": the known answers below come
## from the vendored Python generator (scfg_core/_qrcodegen.py).
func _matrix_hash(qr: Variant) -> String:
	if not qr is ScfgQrcodegen.QrCode:
		return "<%s>" % str(qr)
	var q: ScfgQrcodegen.QrCode = qr
	var rows := PackedStringArray()
	for y in q.get_size():
		var row := ""
		for x in q.get_size():
			row += "1" if q.get_module(x, y) else "0"
		rows.append(row)
	return ScfgBytes.bytes_to_hex(ScfgSha256.sha256("\n".join(rows).to_utf8_buffer()))


# tests/unit/test_paylinks.py::test_eip681_uri
func test_eip681_uri() -> bool:
	var uri: Variant = ScfgPaylinks.eip681_transfer_uri({"token_address": "0xaf88d065e77c8cC2239327C5EDb3A432268e5831",
		"chain_id": 42161, "recipient": "0x52908400098527886E0F7030069857D2E4169EE7", "units": 4993742})
	T.eq(uri, "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer"
		+ "?address=0x52908400098527886E0F7030069857D2E4169EE7&uint256=4993742", "the EIP-681 link")
	var big: Variant = ScfgPaylinks.eip681_transfer_uri({"token_address": "0xT", "chain_id": 1, "recipient": "0xR",
		"units": ScfgBigInt.from_decimal("18446744073709551616")})
	T.eq(big, "ethereum:0xT@1/transfer?address=0xR&uint256=18446744073709551616", "ScfgBigInt units")
	T.is_err(ScfgPaylinks.eip681_transfer_uri({"token_address": "0xT", "chain_id": 1, "recipient": "0xR"}),
		"TypeError", "a missing keyword argument")
	return T.done()


# tests/unit/test_paylinks.py::test_qr_matrix_is_square_with_finder_patterns
func test_qr_matrix_is_square_with_finder_patterns() -> bool:
	var m: Variant = ScfgPaylinks.qr_matrix("hello")
	T.ok(m is Array and m.size() == 21, "version 1")
	if m is Array:
		var square := true
		for row: Array in m:
			square = square and row.size() == m.size()
		T.ok(square, "square")
		var outline := true
		for i in 7:
			outline = outline and m[0][i] == true and m[i][0] == true
		T.ok(outline, "a solid 7x7 finder outline in the top-left corner")
		var row10 := ""
		for v: bool in m[10]:
			row10 += "1" if v else "0"
		T.eq(row10, "110101101101010100111", "row 10 as Python draws it")
	T.is_err(ScfgPaylinks.qr_matrix(5), "TypeError", "text must be a str")
	T.is_err(ScfgPaylinks.qr_matrix("a".repeat(2332)), "ValueError", "too long for version 40 (DataTooLongError)")
	return T.done()


# tests/unit/test_paylinks.py::test_qr_rects_cover_exactly_the_dark_modules
func test_qr_rects_cover_exactly_the_dark_modules() -> bool:
	var text := "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer?uint256=1"
	var m: Array = ScfgPaylinks.qr_matrix(text)
	var got: Array = ScfgPaylinks.qr_rects(text, 2)
	T.eq(got[0], m.size() + 4, "size includes the quiet zone")
	var painted := {}
	for r: Array in got[1]:
		for dx: int in r[2]:
			painted[Vector2i(r[0] + dx - 2, r[1] - 2)] = true
	var dark := {}
	for y in m.size():
		for x in m.size():
			if m[y][x]:
				dark[Vector2i(x, y)] = true
	T.ok(painted.size() == dark.size() and painted.keys().all(func(k: Vector2i) -> bool: return dark.has(k)),
		"the runs paint exactly the dark modules")
	T.ok(got[1].size() < dark.size(), "runs are merged")
	var plain: Array = ScfgPaylinks.qr_rects("hello")
	T.eq(plain[0], 25, "border defaults to 2")
	T.eq(ScfgPaylinks.qr_rects("hello", 0)[1].slice(0, 3), [[0, 0, 7], [12, 0, 1], [14, 0, 7]], "border 0")
	return T.done()


## The port against the vendored Python generator: version, boosted ECC level, chosen mask and
## the whole matrix, over every segment mode, ECC level and an ECI segment.
func test_qrcodegen_matches_the_vendored_python() -> bool:
	var Qr := ScfgQrcodegen.QrCode
	var Ecc := ScfgQrcodegen.Ecc
	var table := [
		["hello", Ecc.MEDIUM, 1, 3, 6, "69eec149521d0daf8dd3263acf22740ee26b4a844714a559cabedbf5de3f1bad"],
		["", Ecc.MEDIUM, 1, 3, 6, "969aefbb2971ea7195db557ce9ecdb16564e637117888499d9eafde776166122"],
		["0123456789", Ecc.MEDIUM, 1, 3, 6, "05a39f0e789b7b755d47b3528d609d41785b067b203068788cad785a6da540f7"],
		["HELLO WORLD", Ecc.MEDIUM, 1, 2, 0, "7cb519f187a1f6d6294385b11fd9d65487509b897a600c9c5e6398297cd45222"],
		["3141592653589793238462643383279502884197", Ecc.MEDIUM, 2, 2, 0,
			"f53482499c4dd15c28e387dcc0c73b835a8103369ea9772a91c1e17a6a4f3c5a"],
		["HTTPS://EXAMPLE.COM/PAY?X=1", Ecc.MEDIUM, 3, 2, 1, "7dd1ddff2926a37f177725942e5c5b8ab9aecb9adb341eed4f3d047ecf20b109"],
		["Ruby Dreams", Ecc.LOW, 1, 2, 7, "8e94d9b5fbcfab946a58ae2a3e93f8d21f6eac04bd4b21240b5b4f77f4f7c925"],
		["Ruby Dreams", Ecc.QUARTILE, 1, 2, 7, "8e94d9b5fbcfab946a58ae2a3e93f8d21f6eac04bd4b21240b5b4f77f4f7c925"],
		["Ruby Dreams", Ecc.HIGH, 2, 3, 5, "10851f49d06e54b9ed7e256bac47101d96068c660ae736e871e136845ff07f6e"],
	]
	for row: Array in table:
		var qr: Variant = Qr.encode_text(row[0], row[1])
		T.ok(qr is ScfgQrcodegen.QrCode, "encode_text(%s)" % ScfgPy.py_repr(row[0]))
		if qr is ScfgQrcodegen.QrCode:
			T.eq([qr.get_version(), qr.get_error_correction_level(), qr.get_mask()], row.slice(2, 5),
				"version, ecc, mask of %s" % ScfgPy.py_repr(row[0]))
			T.eq(_matrix_hash(qr), row[5], "matrix of %s" % ScfgPy.py_repr(row[0]))
	var eci: Variant = Qr.encode_segments([ScfgQrcodegen.QrSegment.make_eci(26),
		ScfgQrcodegen.QrSegment.make_bytes("日本".to_utf8_buffer())], Ecc.LOW, 5, 10, 3, false)
	T.eq([eci.get_version(), eci.get_error_correction_level(), eci.get_mask()], [5, 0, 3], "an ECI segment, forced mask")
	T.eq(_matrix_hash(eci), "be34441e65ef8d7aa07202622d42bd898975f81df983430b14e8150e0e584d53", "ECI matrix")
	var data := PackedByteArray()
	for i in 256:
		data.append(i)
	var binary: Variant = Qr.encode_binary(data, Ecc.HIGH)
	T.eq([binary.get_version(), binary.get_error_correction_level(), binary.get_mask()], [17, 3, 2], "256 bytes at HIGH")
	T.eq(_matrix_hash(binary), "8bc3108651b1c1e458ddb084a8e4b8c53082a7d0ad536fc12b2647d4fd5fea1c", "version 17 matrix")
	T.is_err(Qr.encode_segments([], Ecc.LOW, 3, 2), "ValueError", "minversion > maxversion")
	T.is_err(Qr.encode_segments([], Ecc.LOW, 1, 40, 8), "ValueError", "mask out of range")
	T.is_err(ScfgQrcodegen.QrSegment.make_numeric("12a"), "ValueError", "non-numeric")
	T.is_err(ScfgQrcodegen.QrSegment.make_alphanumeric("abc"), "ValueError", "lowercase is not alphanumeric")
	T.is_err(ScfgQrcodegen.QrSegment.make_eci(1000000), "ValueError", "ECI value out of range")
	var too_long: Variant = Qr.encode_text("1".repeat(5597), Ecc.MEDIUM)
	T.ok(too_long is ScfgErr and (too_long as ScfgErr).kind == "DataTooLongError"
		and (too_long as ScfgErr).is_a("ValueError"), "DataTooLongError is a ValueError")
	var fits: Variant = Qr.encode_text("1".repeat(5596), Ecc.MEDIUM)
	T.ok(fits is ScfgQrcodegen.QrCode and fits.get_version() == 40, "the numeric capacity of version 40 at MEDIUM")
	return T.done()


## Version 32's alignment patterns are 26 modules apart (ISO/IEC 18004 Table E.1): the vendored
## Python's step formula gives that without upstream v1.8.0's `ver == 32` special case.
func test_qrcodegen_keeps_the_python_version_32_alignment() -> bool:
	var qr: Variant = ScfgQrcodegen.QrCode.encode_text("a".repeat(1455), ScfgQrcodegen.Ecc.MEDIUM)
	T.ok(qr is ScfgQrcodegen.QrCode, "1455 bytes")
	if qr is ScfgQrcodegen.QrCode:
		T.eq([qr.get_version(), qr.get_error_correction_level(), qr.get_mask()], [32, 1, 2], "version 32")
		T.eq(qr._get_alignment_pattern_positions(), [6, 34, 60, 86, 112, 138], "step 26, the standard's")
		T.eq(_matrix_hash(qr), "b5ccf4bc7905828b2cab393a32a9a5827a3a90cbc865cc3d9ef02fead2c1d41e", "version 32 matrix")
	return T.done()


## The longest link the payment sheet can produce (the Tier 1 qr "longest" case, version 15)
## must render well under a second.
func test_qr_of_the_longest_link_is_fast() -> bool:
	var label := "The Chronicles of Ruby Dreams and the Endless Winter Night ".repeat(2).substr(0, 64)
	var title := "Chapter Two: The Fall of the House of Ruby & the Rise of Dawn ".repeat(2).substr(0, 80)
	var uri: Variant = ScfgSolanaPaylinks.solana_pay_uri({"recipient": "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU",
		"units": 1000000009999, "mint": "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
		"reference": "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny", "label": label, "message": title})
	T.eq((uri as String).length(), 405, "the longest URI")
	var start := Time.get_ticks_usec()
	var m: Variant = ScfgPaylinks.qr_matrix(uri)
	var ms := (Time.get_ticks_usec() - start) / 1000.0
	T.ok(m is Array and m.size() == 77, "version 15")
	T.ok(ms < 1000.0, "qr_matrix of the longest link took %.0f ms" % ms)
	print("  qr_matrix(longest link): %.0f ms" % ms)
	return T.done()
