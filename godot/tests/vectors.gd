## Reads conformance vector files.
##
## Godot's JSON parser turns every number into a float, so the vectors keep anything that could
## reach 2**53 as a decimal string and bytes as lowercase hex in `*_hex` fields. This loader
## hands those strings over untouched: callers parse them (ScfgBigInt.from_decimal,
## ScfgBytes.hex_to_bytes) and never route them through float. Numbers stay floats here: convert
## them with ScfgPy.json_int where Python holds an int.
##
## Every loader is all-or-nothing: on any unreadable, invalid or misnamed file it prints an
## error (push_error) and returns an empty result, so a caller that checks for a non-empty
## result fails instead of silently testing less. A test that fails a load on purpose passes
## `quiet = true`, which skips the push_error (a passing run prints no ERROR lines).
extends RefCounted


## The `cases` of `<dir>/tier1/<topic>.json`, or [] (with an error printed) if the file is
## missing, is not valid JSON, or is not the Tier 1 file for that topic.
static func load_topic(dir: String, topic: String, quiet: bool = false) -> Array:
	var data: Variant = _read_json(dir.path_join("tier1").path_join(topic + ".json"), quiet)
	if data == null:
		return []
	if not (data is Dictionary and typeof(data.get("topic")) == TYPE_STRING and data["topic"] == topic
			and typeof(data.get("tier")) == TYPE_FLOAT and data["tier"] == 1.0
			and data.get("cases") is Array):
		_fail(quiet, "vectors: tier1/%s.json is not the Tier 1 file for topic %s" % [topic, topic])
		return []
	return data["cases"]


## {topic: cases} for every `<dir>/tier1/*.json`, in file-name order; {} if any file is bad.
static func load_tier1(dir: String, quiet: bool = false) -> Dictionary:
	var tier1 := dir.path_join("tier1")
	if not DirAccess.dir_exists_absolute(tier1):
		_fail(quiet, "vectors: no directory %s" % tier1)
		return {}
	var names := Array(DirAccess.get_files_at(tier1))
	names.sort()
	var out := {}
	for name: String in names:
		if not name.ends_with(".json"):
			continue
		var topic := name.get_basename()
		var cases := load_topic(dir, topic, quiet)
		if cases.is_empty():
			return {}
		out[topic] = cases
	return out


## Every `<dir>/tier2/**/*.json` as {"path": "tier2/<group>/<id>.json", "vector": {...}}, sorted
## by path. Each vector must be a Tier 2 scenario whose `id` is its path without `tier2/` and
## `.json`, with a `steps` array; [] if any file is bad.
static func load_tier2(dir: String, quiet: bool = false) -> Array:
	var root := dir.path_join("tier2")
	if not DirAccess.dir_exists_absolute(root):
		_fail(quiet, "vectors: no directory %s" % root)
		return []
	var paths: Array[String] = []
	_json_files(root, "tier2", paths)
	paths.sort()
	var out := []
	for rel in paths:
		var data: Variant = _read_json(dir.path_join(rel), quiet)
		var id := rel.trim_prefix("tier2/").trim_suffix(".json")
		if not (data is Dictionary and typeof(data.get("tier")) == TYPE_FLOAT and data["tier"] == 2.0
				and typeof(data.get("id")) == TYPE_STRING and data["id"] == id
				and data.get("steps") is Array):
			_fail(quiet, "vectors: %s is not the Tier 2 scenario %s" % [rel, id])
			return []
		out.append({"path": rel, "vector": data})
	return out


static func _json_files(abs_dir: String, rel_dir: String, out: Array[String]) -> void:
	for f: String in DirAccess.get_files_at(abs_dir):
		if f.ends_with(".json"):
			out.append(rel_dir.path_join(f))
	for d: String in DirAccess.get_directories_at(abs_dir):
		_json_files(abs_dir.path_join(d), rel_dir.path_join(d), out)


## The parsed JSON of a file, or null (with an error printed).
static func _read_json(path: String, quiet: bool = false) -> Variant:
	if not FileAccess.file_exists(path):
		_fail(quiet, "vectors: cannot read %s (no such file)" % path)
		return null
	var text := FileAccess.get_file_as_string(path)
	if text == "":
		_fail(quiet, "vectors: cannot read %s (%s)" % [path, error_string(FileAccess.get_open_error())])
		return null
	var json := JSON.new()
	if json.parse(text) != OK:
		_fail(quiet, "vectors: %s line %d: %s" % [path, json.get_error_line(), json.get_error_message()])
		return null
	return json.data


static func _fail(quiet: bool, message: String) -> void:
	if not quiet:
		push_error(message)
