extends GutTest

## Integration tests for the NovaPffArchive Godot binding: open a synthetic PFF3 (no game data),
## inspect/extract/edit/save it, and reopen to confirm the round-trip. The C++ writer internals
## are covered by tests/pff; this exercises the Godot shim (model, decode path, file IO, Save-As).


func test_list_games_returns_the_profile_table() -> void:
	var games := NovaPffArchive.list_games()
	assert_eq(games.size(), 5, "Five shipping game profiles are exposed.")
	var names := []
	for g in games:
		var entry := g as Dictionary
		assert_true(entry.has("id") and entry.has("name"), "Each game has an id and a name.")
		names.append(String(entry["name"]))
	assert_true(names.has("Joint Operations"), "Joint Operations should be listed.")


func test_open_lists_entries() -> void:
	var path := _pff_dir().path_join("entries.pff")
	_write_pff(path, [
		{"name": "alpha.txt", "bytes": "hello"},
		{"name": "Bravo.dat", "bytes": "world!!"},
	])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK, arc.get_last_error())
	assert_eq(arc.get_entry_count(), 2)
	assert_true(arc.has_file("ALPHA.TXT"), "Lookup is case-insensitive.")
	var by_name := {}
	for e in arc.get_entries():
		var d := e as Dictionary
		by_name[String(d["name"])] = d
	assert_true(by_name.has("alpha.txt") and by_name.has("Bravo.dat"))
	assert_eq(int(by_name["alpha.txt"]["size"]), 5)
	assert_false(bool(by_name["Bravo.dat"]["encrypted"]), "Fixture entries are not encrypted.")


func test_read_entry_raw_and_decoded() -> void:
	var path := _pff_dir().path_join("read.pff")
	_write_pff(path, [{"name": "note.txt", "bytes": "plaintext"}])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)
	# A plain (non-SCR/BFC1) payload decodes to itself, so raw and decoded match here.
	assert_eq(arc.read_entry("note.txt", false).get_string_from_utf8(), "plaintext")
	assert_eq(arc.read_entry("note.txt", true).get_string_from_utf8(), "plaintext")
	assert_eq(arc.read_entry("missing.txt", true).size(), 0, "Missing entry yields empty bytes.")


func test_plaintext_scr0_music_script_survives_decode() -> void:
	# Plaintext MUS scripts carry their own "SCR0" magic, which is NOT an encrypted SCR
	# container (those are "SCR" + version byte <= 2). The decode path must pass the bytes
	# through unchanged; a build without the scr_is_scr version guard stripped the 4-byte
	# header and "decrypted" the payload, silently corrupting every extracted music script.
	var root := _pff_dir()
	var mus := "SCR0".to_ascii_buffer()
	mus.append_array(PackedByteArray([0, 1, 0, 0, 42, 7, 99, 1, 2, 3]))
	var path := root.path_join("mus.pff")
	_write_pff(path, [{"name": "gamemus.bin", "bytes": mus}])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)

	assert_eq(arc.read_entry("gamemus.bin", true), mus, "Decoded read returns plaintext SCR0 byte-identical.")
	var out := root.path_join("gamemus.out")
	assert_eq(arc.extract_to_status("gamemus.bin", out, true), 0, "SCR0 pass-through counts as decoded, not raw fallback.")
	assert_eq(FileAccess.get_file_as_bytes(out), mus, "Extracted file keeps its header and exact payload bytes.")


func test_extract_to_and_extract_all() -> void:
	var root := _pff_dir()
	var path := root.path_join("extract.pff")
	_write_pff(path, [
		{"name": "a.bin", "bytes": "AAAA"},
		{"name": "b.bin", "bytes": "BBBBBB"},
	])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)

	var single := root.path_join("a_out.bin")
	assert_eq(arc.extract_to("a.bin", single, true), OK, arc.get_last_error())
	assert_eq(FileAccess.get_file_as_string(single), "AAAA")

	var out_dir := root.path_join("all")
	assert_eq(DirAccess.make_dir_recursive_absolute(out_dir), OK)
	assert_eq(arc.extract_all(out_dir, true), OK)
	assert_eq(FileAccess.get_file_as_string(out_dir.path_join("a.bin")), "AAAA")
	assert_eq(FileAccess.get_file_as_string(out_dir.path_join("b.bin")), "BBBBBB")


func test_extract_async_selected_and_all() -> void:
	var root := _pff_dir()
	var path := root.path_join("async.pff")
	_write_pff(path, [
		{"name": "a.bin", "bytes": "AAAA"},
		{"name": "b.bin", "bytes": "BBBBBB"},
		{"name": "c.bin", "bytes": "CC"},
	])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)

	# Selected names: only a.bin and c.bin should land.
	var sel_dir := root.path_join("sel")
	assert_eq(DirAccess.make_dir_recursive_absolute(sel_dir), OK)
	assert_eq(arc.extract_async(PackedStringArray(["a.bin", "c.bin"]), sel_dir, true), OK, arc.get_last_error())
	while arc.is_extract_running():
		await get_tree().process_frame
	arc.wait_for_extract_completion()
	assert_eq(arc.get_extract_progress_done(), 2, "Both selected entries processed.")
	assert_eq(arc.get_extract_ok_count(), 2, "Both extracted cleanly.")
	assert_eq(arc.get_extract_failed_count(), 0)
	assert_eq(FileAccess.get_file_as_string(sel_dir.path_join("a.bin")), "AAAA")
	assert_eq(FileAccess.get_file_as_string(sel_dir.path_join("c.bin")), "CC")
	assert_false(FileAccess.file_exists(sel_dir.path_join("b.bin")), "Unselected entry is not extracted.")

	# Empty name list means "every entry".
	var all_dir := root.path_join("all")
	assert_eq(DirAccess.make_dir_recursive_absolute(all_dir), OK)
	assert_eq(arc.extract_async(PackedStringArray(), all_dir, true), OK)
	while arc.is_extract_running():
		await get_tree().process_frame
	arc.wait_for_extract_completion()
	assert_eq(arc.get_extract_progress_total(), 3, "Total spans every entry.")
	assert_eq(arc.get_extract_ok_count(), 3, "All three extracted.")
	assert_eq(FileAccess.get_file_as_string(all_dir.path_join("b.bin")), "BBBBBB")


func test_extract_async_reports_raw_fallback() -> void:
	# An undecodable BFC1 entry must still be written (counted raw), not dropped.
	var root := _pff_dir()
	var bad := "BFC1".to_ascii_buffer()
	bad.append_array(PackedByteArray([0, 0, 1, 0, 255, 255, 255, 255, 255, 255, 255, 255]))
	var path := root.path_join("asyncraw.pff")
	_write_pff(path, [{"name": "broken.dat", "bytes": bad}, {"name": "plain.txt", "bytes": "hello"}])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)

	var out := root.path_join("out")
	assert_eq(DirAccess.make_dir_recursive_absolute(out), OK)
	assert_eq(arc.extract_async(PackedStringArray(), out, true), OK)
	while arc.is_extract_running():
		await get_tree().process_frame
	arc.wait_for_extract_completion()
	assert_eq(arc.get_extract_raw_count(), 1, "Undecodable entry counted as saved-raw.")
	assert_eq(arc.get_extract_ok_count(), 1, "Decodable entry counted ok.")
	assert_eq(FileAccess.get_file_as_bytes(out.path_join("broken.dat")), bad, "Raw bytes written verbatim.")


func test_add_remove_save_roundtrip() -> void:
	var root := _pff_dir()
	var path := root.path_join("edit.pff")
	_write_pff(path, [
		{"name": "keep.txt", "bytes": "keep me"},
		{"name": "drop.txt", "bytes": "remove me"},
	])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)
	assert_false(arc.is_dirty(), "A freshly opened archive is clean.")

	var added_src := root.path_join("added_source.bin")
	_write_file(added_src, "freshly added")
	assert_eq(arc.add_file_from_disk(added_src, "added.bin", false), OK, arc.get_last_error())
	assert_true(arc.is_dirty(), "Adding a file marks the archive dirty.")
	assert_eq(arc.get_entry_count(), 3)

	assert_eq(arc.remove_entries(PackedStringArray(["drop.txt"])), OK)
	assert_eq(arc.get_entry_count(), 2)
	assert_false(arc.has_file("drop.txt"))

	var out := root.path_join("edited.pff")
	assert_eq(arc.save_as(out), OK, arc.get_last_error())
	assert_false(arc.is_dirty(), "Save-As clears the dirty flag.")

	var reopened := NovaPffArchive.new()
	assert_eq(reopened.open(out), OK)
	assert_eq(reopened.get_entry_count(), 2)
	assert_true(reopened.has_file("added.bin"))
	assert_false(reopened.has_file("drop.txt"), "Removed entry stays gone after resave.")
	assert_eq(reopened.read_entry("keep.txt", false).get_string_from_utf8(), "keep me", "Retained bytes survive the round-trip.")
	assert_eq(reopened.read_entry("added.bin", false).get_string_from_utf8(), "freshly added")


func test_save_as_refuses_overwriting_source() -> void:
	var path := _pff_dir().path_join("guard.pff")
	_write_pff(path, [{"name": "x.bin", "bytes": "x"}])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)
	assert_ne(arc.save_as(path), OK, "Save-As must refuse to overwrite the source archive.")
	assert_string_contains(arc.get_last_error().to_lower(), "source", "Error explains the refusal.")


func test_extract_to_status_raw_fallback() -> void:
	# An entry that claims BFC1 but is not a valid stream: decode fails, so extract must fall back to
	# writing the raw stored bytes (status 1) rather than silently skipping it. A decodable plaintext
	# entry reports status 0.
	var root := _pff_dir()
	var bad := "BFC1".to_ascii_buffer()
	bad.append_array(PackedByteArray([0, 0, 1, 0, 255, 255, 255, 255, 255, 255, 255, 255]))
	var path := root.path_join("raw.pff")
	_write_pff(path, [{"name": "broken.dat", "bytes": bad}, {"name": "plain.txt", "bytes": "hello"}])
	var arc := NovaPffArchive.new()
	assert_eq(arc.open(path), OK)

	var bad_out := root.path_join("broken.out")
	assert_eq(arc.extract_to_status("broken.dat", bad_out, true), 1, "undecodable entry saved as raw (status 1)")
	assert_eq(FileAccess.get_file_as_bytes(bad_out), bad, "raw bytes written verbatim, not dropped")
	assert_eq(arc.extract_to_status("plain.txt", root.path_join("plain.out"), true), 0, "decodable entry status 0")
	assert_eq(arc.get_last_undecoded_count(), 1, "one entry counted as saved-raw")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join("opennova_pff_binding"))


func _pff_dir() -> String:
	var dir := OS.get_cache_dir().path_join("opennova_pff_binding").path_join(str(Time.get_ticks_usec()))
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	return dir


func _write_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


# Builds a modern PFF3: header(20) | directory(36 each) | payloads. file_table_offset points at
# the directory right after the header (the engine and our reader both re-sort on load).
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "PFF fixture should be writable: %s" % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size

	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)

	for entry in entries:
		var bytes := _entry_bytes(entry)
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		var name_bytes := String(entry.name).to_utf8_buffer()
		for i in range(16):
			file.store_8(name_bytes[i] if i < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()

	for entry in entries:
		file.store_buffer(_entry_bytes(entry))
	file.close()


func _entry_bytes(entry: Dictionary) -> PackedByteArray:
	return entry.bytes if entry.bytes is PackedByteArray else String(entry.bytes).to_utf8_buffer()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
