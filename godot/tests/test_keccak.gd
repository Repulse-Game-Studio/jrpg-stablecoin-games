extends RefCounted

const T := preload("res://tests/t.gd")


func _hex(data: PackedByteArray) -> String:
	return ScfgBytes.bytes_to_hex(ScfgKeccak.keccak256(data))


func _rep(ch: String, n: int) -> PackedByteArray:
	return ch.repeat(n).to_ascii_buffer()


# tests/unit/test_keccak.py::test_keccak256_vectors
func test_keccak256_vectors() -> bool:
	T.eq(_hex(PackedByteArray()), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470", "empty")
	T.eq(_hex("abc".to_ascii_buffer()), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45", "abc")
	T.eq(_hex("Transfer(address,address,uint256)".to_ascii_buffer()),
		"ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef", "Transfer event signature")
	# 200 bytes spans two 136-byte blocks: exercises multi-block absorption
	T.eq(_hex(_rep("a", 200)), "96ea54061def936c4be90b518992fdc6f12f535068a256229aca54267b4d084d", "two blocks")
	return T.done()


func test_keccak256_returns_32_bytes_and_does_not_touch_its_input() -> bool:
	var data := PackedByteArray([120])
	T.eq(ScfgKeccak.keccak256(data).size(), 32, "32 bytes")
	T.eq(data, PackedByteArray([120]), "the input is not padded in place")
	return T.done()
