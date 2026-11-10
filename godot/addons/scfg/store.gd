## The player's payment state on disk: `user://scfg/state.json` (D6), global to the install and
## never inside a save slot. The twin of Ren'Py's `persistent.scfg_pending` /
## `persistent.scfg_receipts`. In a web export `user://` is backed by IndexedDB.
##
## The file is `{"version": 1, "pending": {...}, "receipts": {...}}`, the Engine's two stores as
## they are (conformance/schema/state.json).
##
## - `load(path)` returns `{"pending", "receipts", "warning", "error"}`. Both stores are
##   normalised with ScfgPy.from_json (Godot's JSON reader makes every number a float). A missing
##   file is a first launch: empty stores, no warning and no error. A file that is not that
##   document (not UTF-8, not JSON, another version, a store that is not an object) gives empty
##   stores and a warning naming the file. The bad file is renamed to `<path>.corrupt` (then
##   `.corrupt.2`, `.corrupt.3`, ...: a later corruption never replaces an earlier copy), so the
##   next save cannot overwrite what support might still recover (receipts also come back through
##   Restore). A file that exists but cannot be read right now (no permission, a sharing
##   violation, an antivirus lock, an I/O error) is not corrupt: it is left where it is, and
##   `error` is a RuntimeError ScfgErr naming it. The caller must then not save over it this run
##   (the autoload starts no payments; the next launch reads it again).
##   When `path` is missing but a `<path>.tmp` exists (a save stopped between writing it and the
##   rename), the `.tmp` is read instead.
## - `save(path, pending, receipts)` writes `<path>.tmp`, flushes and closes it, then renames it
##   over `path`. A crash or a failed write leaves the previous file whole. It returns null or a
##   RuntimeError ScfgErr. The directory is created when needed.
class_name ScfgStore
extends RefCounted

const VERSION := 1


static func _empty(warning: Variant = null, error: Variant = null) -> Dictionary:
	return {"pending": {}, "receipts": {}, "warning": warning, "error": error}


## True when a file is at `path`, readable or not. FileAccess.file_exists opens the file on Godot
## 4.3, so a file this process may not read would look missing (and be saved over).
static func _exists(path: String) -> bool:
	if FileAccess.file_exists(path):
		return true
	var dir := DirAccess.open(path.get_base_dir())
	return dir != null and dir.file_exists(path.get_file())


## The bytes of `source`, or a RuntimeError ScfgErr when it cannot be read.
## @fallible
static func _read(source: String) -> Variant:
	var f := FileAccess.open(source, FileAccess.READ)
	if f == null:
		return ScfgErr.make("RuntimeError", "cannot read %s: %s" % [source, error_string(FileAccess.get_open_error())])
	var length := f.get_length()
	var raw := f.get_buffer(length)
	var read_error := f.get_error()
	f.close()
	if raw.size() != length or not read_error in [OK, ERR_FILE_EOF]:
		return ScfgErr.make("RuntimeError", "cannot read %s: %s" % [
			source, error_string(read_error if read_error != OK else ERR_FILE_CANT_READ)])
	return raw


## The stores in `path` (see the header). Never fails: a bad file is a warning, an unreadable
## one an `error`.
static func load(path: String) -> Dictionary:
	var source := path
	if not _exists(path):
		# A rename that replaces the target by deleting it first can stop between the two
		# steps; the complete `.tmp` is then the newest state.
		if not _exists(path + ".tmp"):
			return _empty()
		source = path + ".tmp"
	var raw: Variant = _read(source)
	if raw is ScfgErr:
		return _empty(null, raw)
	var why := ""
	var doc: Variant = null
	var parsed := ScfgCoio.parse_body(raw)
	if not parsed[0]:
		why = "is not valid JSON"
	else:
		doc = ScfgPy.from_json(parsed[1])
		if not doc is Dictionary:
			why = "is not a JSON object"
		elif not (typeof(doc.get("version")) == TYPE_INT and doc["version"] == VERSION):
			why = "has an unknown version %s" % ScfgPy.py_repr(doc.get("version"))
		elif not (doc.get("pending") is Dictionary and doc.get("receipts") is Dictionary):
			why = "has no pending/receipts objects"
	if why == "":
		return {"pending": doc["pending"], "receipts": doc["receipts"], "warning": null, "error": null}
	var aside := path + ".corrupt"
	var n := 1
	while _exists(aside):
		n += 1
		aside = "%s.corrupt.%d" % [path, n]
	var kept := DirAccess.rename_absolute(source, aside) == OK
	return _empty("the payment state file %s %s; starting with no receipts or pending payments (%s). "
		% [source, why, "the file was kept as " + aside if kept else "it could not be moved aside"]
		+ "Players can bring purchases back with Restore.")


## Writes the stores to `path` atomically (see the header).
## @fallible
static func save(path: String, pending: Dictionary, receipts: Dictionary) -> Variant:
	var dir := path.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		var made := DirAccess.make_dir_recursive_absolute(dir)
		if made != OK:
			return ScfgErr.make("RuntimeError", "cannot create %s: %s" % [dir, error_string(made)])
	var tmp := path + ".tmp"
	var f := FileAccess.open(tmp, FileAccess.WRITE)
	if f == null:
		return ScfgErr.make("RuntimeError", "cannot write %s: %s" % [tmp, error_string(FileAccess.get_open_error())])
	# keys in insertion order, as Python's json.dump (store order is visible: reverify reports
	# removed receipts in it), and floats at full precision (created_at, hint_at)
	f.store_string(JSON.stringify({"version": VERSION, "pending": pending, "receipts": receipts}, "", false, true))
	f.flush()
	var write_error := f.get_error()
	f.close()
	if write_error != OK:
		DirAccess.remove_absolute(tmp)
		return ScfgErr.make("RuntimeError", "cannot write %s: %s" % [tmp, error_string(write_error)])
	var moved := DirAccess.rename_absolute(tmp, path)
	if moved != OK:
		return ScfgErr.make("RuntimeError", "cannot replace %s: %s" % [path, error_string(moved)])
	return null
