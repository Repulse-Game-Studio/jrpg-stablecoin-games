## ScfgStore (addons/scfg/store.gd): user://scfg/state.json, read and written atomically.
##
## Every test works in its own directory under user://scfg-tests/ and removes it afterwards.
extends RefCounted

const T := preload("res://tests/t.gd")

const _DIR := "user://scfg-tests/store"


func _fresh_dir(name: String) -> String:
	var dir := _DIR.path_join(name)
	_remove_tree(dir)
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


static func _remove_tree(dir: String) -> void:
	if not DirAccess.dir_exists_absolute(dir):
		return
	for f: String in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(f))
	for d: String in DirAccess.get_directories_at(dir):
		_remove_tree(dir.path_join(d))
	DirAccess.remove_absolute(dir)


static func _write(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(text)
	f.close()


func _pending() -> Dictionary:
	return {"ps_1": {"id": "ps_1", "state": "awaiting", "cursor": 900, "start_block": 900, "hint_tx": null,
		"match": {"value": 4990000, "log_index": 3}}}


func _receipts() -> Dictionary:
	return {"arbitrum:0xab:3": {"chain": "arbitrum", "tx_hash": "0xab", "log_index": 3, "kind": "unlock",
		"item": "chapter2", "amount": "4.99", "token": "USDC", "block": 1234}}


func test_save_then_load_round_trips_with_ints() -> bool:
	var path := _fresh_dir("round").path_join("state.json")
	T.eq(ScfgStore.save(path, _pending(), _receipts()), null, "saved")
	var got := ScfgStore.load(path)
	T.eq(got.get("warning"), null, "no warning")
	T.eq(got.get("pending"), _pending(), "pending (ints stay ints)")
	T.eq(got.get("receipts"), _receipts(), "receipts (ints stay ints)")
	var doc: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	T.eq(ScfgPy.from_json(doc).get("version"), 1, "version 1")
	T.eq((doc as Dictionary).keys().size(), 3, "version, pending and receipts only")
	T.ok(not FileAccess.file_exists(path + ".tmp"), "no .tmp left behind")
	_remove_tree(_DIR)
	return T.done()


func test_save_creates_the_directory() -> bool:
	var path := _fresh_dir("mkdir").path_join("deeper/scfg/state.json")
	T.eq(ScfgStore.save(path, {}, {}), null, "saved")
	T.ok(FileAccess.file_exists(path), "written")
	_remove_tree(_DIR)
	return T.done()


func test_missing_file_is_empty_without_a_warning() -> bool:
	var path := _fresh_dir("missing").path_join("state.json")
	var got := ScfgStore.load(path)
	T.eq([got.get("pending"), got.get("receipts"), got.get("warning")], [{}, {}, null], "empty, quietly (a first launch)")
	_remove_tree(_DIR)
	return T.done()


func test_corrupt_file_is_empty_with_a_warning_and_kept_aside() -> bool:
	var dir := _fresh_dir("corrupt")
	var cases := {
		"not json": "{\"version\": 1, \"pending\": {",
		"not an object": "[1, 2]",
		"wrong version": "{\"version\": 2, \"pending\": {}, \"receipts\": {}}",
		"pending not an object": "{\"version\": 1, \"pending\": [], \"receipts\": {}}",
		"receipts missing": "{\"version\": 1, \"pending\": {}}",
		"not utf-8": "",
	}
	var n := 0
	for label: String in cases:
		n += 1
		var path := dir.path_join("state%d.json" % n)
		if label == "not utf-8":
			var f := FileAccess.open(path, FileAccess.WRITE)
			f.store_buffer(PackedByteArray([0x7b, 0xff, 0x7d]))
			f.close()
		else:
			_write(path, cases[label])
		var got := ScfgStore.load(path)
		T.eq([got.get("pending"), got.get("receipts")], [{}, {}], "%s: empty stores" % label)
		T.ok(got.get("warning") is String and (got["warning"] as String).contains(path), "%s: a warning naming the file (%s)" % [
			label, got.get("warning")])
		T.ok(FileAccess.file_exists(path + ".corrupt"), "%s: the bad file is kept as .corrupt" % label)
		T.ok(not FileAccess.file_exists(path), "%s: and moved out of the way" % label)
	_remove_tree(_DIR)
	return T.done()


func test_atomic_write_leaves_old_file_on_failure() -> bool:
	var path := _fresh_dir("atomic").path_join("state.json")
	T.eq(ScfgStore.save(path, _pending(), _receipts()), null, "first save")
	# a directory where the temporary file goes: the write fails before the rename
	DirAccess.make_dir_recursive_absolute(path + ".tmp")
	var failed: Variant = ScfgStore.save(path, {}, {})
	T.is_err(failed, "RuntimeError", "the failed write is an error")
	var got := ScfgStore.load(path)
	T.eq(got.get("receipts"), _receipts(), "the old file is intact")
	T.eq(got.get("pending"), _pending(), "the old pending too")
	_remove_tree(_DIR)
	return T.done()


func test_a_failed_rename_leaves_the_target_untouched() -> bool:
	var dir := _fresh_dir("rename")
	var path := dir.path_join("state.json")
	# a directory at the target: the .tmp is written, then the rename fails
	DirAccess.make_dir_recursive_absolute(path)
	_write(path.path_join("keep.txt"), "old")
	var failed: Variant = ScfgStore.save(path, _pending(), _receipts())
	T.is_err(failed, "RuntimeError", "the failed rename is an error")
	T.ok(failed is ScfgErr and (failed as ScfgErr).message.begins_with("cannot replace"), "at the rename (%s)" % [failed])
	T.eq(FileAccess.get_file_as_string(path.path_join("keep.txt")), "old", "what was there is untouched")
	_remove_tree(_DIR)
	return T.done()


func test_load_falls_back_to_a_complete_tmp() -> bool:
	var dir := _fresh_dir("tmp_fallback")
	var path := dir.path_join("state.json")
	T.eq(ScfgStore.save(dir.path_join("other.json"), _pending(), _receipts()), null, "a complete file")
	DirAccess.rename_absolute(dir.path_join("other.json"), path + ".tmp")
	var got := ScfgStore.load(path)
	T.eq([got.get("pending"), got.get("receipts"), got.get("warning")], [_pending(), _receipts(), null],
		"a save stopped before its rename: the .tmp is read")
	T.eq(ScfgStore.save(path, {}, {}), null, "a later save")
	_write(path + ".tmp", "{\"version\": 1, \"pend")  # a torn write beside a good file
	got = ScfgStore.load(path)
	T.eq([got.get("pending"), got.get("receipts"), got.get("warning")], [{}, {}, null], "the file wins over a .tmp")
	_remove_tree(_DIR)
	return T.done()


## Makes `path` unreadable (no permissions); false when that does not stop this process from
## reading it (running as root), so the caller skips.
static func _make_unreadable(path: String) -> bool:
	FileAccess.set_unix_permissions(path, 0)
	if FileAccess.open(path, FileAccess.READ) == null:
		return true
	FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
	return false


func test_unreadable_file_is_left_untouched() -> bool:
	var dir := _fresh_dir("unreadable")
	var path := dir.path_join("state.json")
	T.eq(ScfgStore.save(path, _pending(), _receipts()), null, "a good file")
	var before := FileAccess.get_file_as_bytes(path)
	if not _make_unreadable(path):
		T.skip("this process reads files without permission (root)")
		_remove_tree(_DIR)
		return T.done()
	var got := ScfgStore.load(path)
	FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
	T.eq([got.get("pending"), got.get("receipts"), got.get("warning")], [{}, {}, null],
		"no stores and no corruption warning")
	T.is_err(got.get("error"), "RuntimeError", "an error instead")
	T.ok(got.get("error") is ScfgErr and (got["error"] as ScfgErr).message.contains(path),
		"naming the file (%s)" % [got.get("error")])
	T.ok(not FileAccess.file_exists(path + ".corrupt"), "not moved aside as corrupt")
	T.eq(FileAccess.get_file_as_bytes(path), before, "the file is untouched")
	got = ScfgStore.load(path)
	T.eq([got.get("pending"), got.get("receipts"), got.get("error")], [_pending(), _receipts(), null],
		"read again once it is readable (the next launch)")
	_remove_tree(_DIR)
	return T.done()


func test_later_corruption_keeps_earlier_copies() -> bool:
	var dir := _fresh_dir("corrupt_twice")
	var path := dir.path_join("state.json")
	_write(path, "first bad")
	ScfgStore.load(path)
	_write(path, "second bad")
	var got := ScfgStore.load(path)
	_write(path, "third bad")
	ScfgStore.load(path)
	T.eq(FileAccess.get_file_as_string(path + ".corrupt"), "first bad", "the first copy is kept")
	T.eq(FileAccess.get_file_as_string(path + ".corrupt.2"), "second bad", "the second beside it")
	T.eq(FileAccess.get_file_as_string(path + ".corrupt.3"), "third bad", "and the third")
	T.ok(got.get("warning") is String and (got["warning"] as String).contains(path + ".corrupt.2"),
		"the warning names where it went (%s)" % [got.get("warning")])
	T.ok(not FileAccess.file_exists(path), "the bad file is out of the way")
	_remove_tree(_DIR)
	return T.done()
