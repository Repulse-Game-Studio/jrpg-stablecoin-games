extends RefCounted

const T := preload("res://tests/t.gd")


func _hex(data: PackedByteArray) -> String:
	return ScfgBytes.bytes_to_hex(ScfgSha256.sha256(data))


# python3 -c "import hashlib; print(hashlib.sha256(b'abc').hexdigest())"
func test_sha256_known_answers() -> bool:
	T.eq(_hex(PackedByteArray()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty")
	T.eq(_hex("abc".to_ascii_buffer()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc")
	T.eq(_hex("a".repeat(55).to_ascii_buffer()),
		"9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "55 bytes (one block)")
	T.eq(_hex("a".repeat(56).to_ascii_buffer()),
		"b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "56 bytes (padding spills over)")
	T.eq(_hex("a".repeat(64).to_ascii_buffer()),
		"ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "64 bytes")
	T.eq(_hex("a".repeat(200).to_ascii_buffer()),
		"c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5", "200 bytes")
	T.eq(ScfgSha256.sha256(PackedByteArray([1])).size(), 32, "32 bytes")
	return T.done()
