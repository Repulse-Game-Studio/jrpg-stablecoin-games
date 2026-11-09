## Reads conformance vector files.
##
## Godot's JSON parser turns every number into a float, so the vectors keep anything that could
## reach 2**53 as a decimal string and bytes as lowercase hex in `*_hex` fields. This loader
## hands those strings over untouched: callers parse them (ScfgBigInt.from_decimal,
## String.hex_decode) and never route them through float.
extends RefCounted


## The `cases` of `<dir>/tier1/<topic>.json`, or [] (with an error printed) if the file is
## missing, is not valid JSON, or is not the Tier 1 file for that topic.
static func load_topic(dir: String, topic: String) -> Array:
	var path := dir.path_join("tier1").path_join(topic + ".json")
	var text := FileAccess.get_file_as_string(path)
	if text == "":
		push_error("vectors: cannot read %s (%s)" % [path, error_string(FileAccess.get_open_error())])
		return []
	var json := JSON.new()
	if json.parse(text) != OK:
		push_error("vectors: %s line %d: %s" % [path, json.get_error_line(), json.get_error_message()])
		return []
	var data: Variant = json.data
	if not (data is Dictionary and str(data.get("topic")) == topic
			and typeof(data.get("tier")) == TYPE_FLOAT and data.get("tier") == 1.0
			and data.get("cases") is Array):
		push_error("vectors: %s is not the Tier 1 file for topic %s" % [path, topic])
		return []
	return data["cases"]
