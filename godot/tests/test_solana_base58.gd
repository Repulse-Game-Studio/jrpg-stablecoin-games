extends RefCounted

const T := preload("res://tests/t.gd")

const TOKEN := "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"


func _b(hex: String) -> PackedByteArray:
	return ScfgBytes.hex_to_bytes(hex)  # err-ok: test literals are valid hex


# tests/unit/test_solana_base58.py::test_known_vectors
func test_known_vectors() -> bool:
	var table := [
		[PackedByteArray(), ""], [PackedByteArray([0]), "1"], [PackedByteArray([0, 0, 1]), "112"],
		[PackedByteArray(_zeros(32)), "1".repeat(32)], ["hello world".to_ascii_buffer(), "StV1DL6CwTryKyV"],
	]
	for row: Array in table:
		T.eq(ScfgSolanaBase58.b58encode(row[0]), row[1], "encode %s" % row[1])
		if row[1] != "":
			T.eq(ScfgSolanaBase58.b58decode(row[1]), row[0], "decode %s" % row[1])
	return T.done()


func _zeros(n: int) -> PackedByteArray:
	var z := PackedByteArray()
	z.resize(n)
	return z


# keys and signatures from random.Random(7) and solders' str(Pubkey) / str(Signature)
func test_matches_solders() -> bool:
	var key := _b("a54dca182530bb1d6d132cded6237b2ed91e3f721fcb1971174494d6493c9d5c")
	T.eq(ScfgSolanaBase58.b58encode(key), "C8H4v4c2eA6njjgzvWSrCpLdYg3hWSygoVsi4RkUrzjV", "pubkey")
	T.eq(ScfgSolanaBase58.b58decode("C8H4v4c2eA6njjgzvWSrCpLdYg3hWSygoVsi4RkUrzjV"), key, "pubkey back")
	var sig := _b("3460be31201e69fedaa0eee8b9997f5c7c2999fdafe593253cd654af4dfad71427a0aeb3fee9232f8af2211f9ee491c5b10becb5563bfc1e6f93427ecbc8fe29")
	var text := "23jngYy1nAztL6f9tnShXjnh4A4jKciAF9eYYXyGo8Ekd3ZLFfCoPmtAsj7XXAPj2oKo6HkKr8Hf3nKRDaaRmqhS"
	T.eq(ScfgSolanaBase58.b58encode(sig), text, "signature")
	T.eq(ScfgSolanaBase58.b58decode(text), sig, "signature back")
	return T.done()


func test_decode_rejects_bad_input() -> bool:
	for bad: Variant in ["", "0", "O", "I", "l", "abc def", "é", "+/=", " ", null, 5, true, [], 1.5]:
		T.is_err(ScfgSolanaBase58.b58decode(bad), "ValueError", "b58decode(%s)" % ScfgPy.py_repr(bad))
	return T.done()


func test_round_trips_every_length_and_leading_zeros() -> bool:
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for n in range(0, 70):
		var raw := PackedByteArray()
		for i in n:
			raw.append(rng.randi() % 256)
		for zeros in [0, 1, 3]:
			var data := _zeros(mini(zeros, n))
			data.append_array(raw.slice(mini(zeros, n)))
			var text := ScfgSolanaBase58.b58encode(data)
			if n == 0:
				T.eq(text, "", "empty")
				continue
			T.eq(ScfgSolanaBase58.b58decode(text), data, "round trip n=%d zeros=%d" % [n, zeros])
	return T.done()


func test_fixed_lengths() -> bool:
	T.eq((ScfgSolanaBase58.decode_fixed(TOKEN, 32) as PackedByteArray).size(), 32, "32 bytes")
	T.is_err(ScfgSolanaBase58.decode_fixed(TOKEN, 64), "ValueError", "wrong length")
	T.is_err(ScfgSolanaBase58.decode_fixed(TOKEN, 31), "ValueError", "31")
	T.is_err(ScfgSolanaBase58.decode_fixed("", 32), "ValueError", "empty text")
	T.eq(ScfgSolanaBase58.decode_fixed("1", 1), PackedByteArray([0]), "length 1")
	T.is_err(ScfgSolanaBase58.decode_fixed("1", 0), "ValueError", "length 0")
	# a JSON length arrives as a float; any non-number never equals len(raw)
	T.eq((ScfgSolanaBase58.decode_fixed(TOKEN, 32.0) as PackedByteArray).size(), 32, "32.0")
	T.is_err(ScfgSolanaBase58.decode_fixed(TOKEN, 32.5), "ValueError", "32.5")
	T.is_err(ScfgSolanaBase58.decode_fixed(TOKEN, "32"), "ValueError", "a string length")
	T.is_err(ScfgSolanaBase58.decode_fixed(TOKEN, null), "ValueError", "a null length")
	T.eq(ScfgSolanaBase58.normalize_pubkey("  " + TOKEN + "\n"), TOKEN, "pubkey is stripped")
	T.eq(ScfgSolanaBase58.normalize_pubkey(" 　" + TOKEN), TOKEN, "Python's strip set")
	T.is_err(ScfgSolanaBase58.normalize_pubkey("112"), "ValueError", "short pubkey")
	T.is_err(ScfgSolanaBase58.normalize_pubkey(12), "ValueError", "int pubkey")
	T.is_err(ScfgSolanaBase58.normalize_pubkey(null), "ValueError", "None pubkey")
	var bytes64 := PackedByteArray()
	for i in 64:
		bytes64.append(i)
	var sig := ScfgSolanaBase58.b58encode(bytes64)
	T.eq(ScfgSolanaBase58.normalize_signature(" " + sig), sig, "signature is stripped")
	T.is_err(ScfgSolanaBase58.normalize_signature(TOKEN), "ValueError", "a pubkey is not a signature")
	T.is_err(ScfgSolanaBase58.normalize_signature(3), "ValueError", "int signature")
	return T.done()


# tests/unit/test_solana_base58.py::test_fixed_length_decoding_refuses_long_text_before_decoding
# Text longer than the longest encoding (44 / 88 characters) is refused at once: base58 decoding
# is quadratic, and a crafted stored id must not freeze the launch re-verification.
func test_fixed_length_decoding_refuses_long_text_before_decoding() -> bool:
	# The refusal is the length's, also for a text whose first character is not base58 (decoding
	# first would fail on that character instead): the length is checked before any decoding.
	for text: String in ["2".repeat(100000), "1".repeat(100000), "0" + "2".repeat(99999), "z".repeat(89), " " + "2".repeat(89)]:
		var label := "%s... x%d" % [text.substr(0, 3), text.length()]
		var start := Time.get_ticks_msec()
		for got: Variant in [ScfgSolanaBase58.decode_fixed(text, 64), ScfgSolanaBase58.decode_fixed(text, 32),
				ScfgSolanaBase58.normalize_signature(text), ScfgSolanaBase58.normalize_pubkey(text)]:
			T.is_err(got, "ValueError", label)
			T.ok(got is ScfgErr and (got as ScfgErr).message.begins_with("too long for "), "%s: refused for its length (%s)" % [
				label, (got as ScfgErr).message.substr(0, 40) if got is ScfgErr else got])
		T.ok(Time.get_ticks_msec() - start < 2000, "%s: refused at once" % label)
	return T.done()


# tests/unit/test_solana_base58.py::test_the_longest_encodings_still_decode
func test_the_longest_encodings_still_decode() -> bool:
	var ff64 := PackedByteArray()
	ff64.resize(64)
	ff64.fill(0xff)
	var ff32 := PackedByteArray()
	ff32.resize(32)
	ff32.fill(0xff)
	var sig := ScfgSolanaBase58.b58encode(ff64)
	var key := ScfgSolanaBase58.b58encode(ff32)
	T.eq([sig.length(), key.length()], [88, 44], "the longest encodings")
	T.eq(ScfgSolanaBase58.normalize_signature(sig), sig, "an 88-character signature")
	T.eq(ScfgSolanaBase58.normalize_pubkey(key), key, "a 44-character key")
	T.eq(ScfgSolanaBase58.normalize_signature("1".repeat(64)), "1".repeat(64), "64 zero bytes")
	T.is_err(ScfgSolanaBase58.decode_fixed("1".repeat(89), 64), "ValueError", "89 characters")
	return T.done()
