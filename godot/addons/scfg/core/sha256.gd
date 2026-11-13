## SHA-256 (Python `hashlib.sha256(data).digest()`), through Godot's HashingContext, which is
## available in every export including web.
class_name ScfgSha256
extends RefCounted


## The 32-byte SHA-256 digest of `data`. Cannot fail.
static func sha256(data: PackedByteArray) -> PackedByteArray:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	if not data.is_empty():  # update() rejects an empty buffer
		ctx.update(data)
	return ctx.finish()
