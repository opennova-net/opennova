extends GutTest

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")

const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_document_pair.sbf"
const PAIR_SCRIPT := "user://music_document_pair.bin"


func after_each() -> void:
	_remove_user_file(PAIR_BANK)
	_remove_user_file(PAIR_SCRIPT)
	_remove_user_file("user://test_doc_save.sbf")
	_remove_user_file("user://test_doc_save.bin")


func test_open_bank():
	var doc = MusicEditorDocument.new()
	assert_false(doc.bank_loaded())
	var err = doc.open_bank(BANK_FIXTURE)
	assert_eq(err, OK)
	assert_true(doc.bank_loaded())
	assert_eq(doc.bank_path, BANK_FIXTURE)
	assert_false(doc.is_dirty(), "fresh open is clean")


func test_open_script():
	var doc = MusicEditorDocument.new()
	var err = doc.open_script(SCRIPT_FIXTURE)
	assert_eq(err, OK)
	assert_true(doc.script_loaded())


func test_close_pair_resets_state():
	var doc = MusicEditorDocument.new()
	doc.open_bank(BANK_FIXTURE)
	doc.close_pair()
	assert_false(doc.bank_loaded())
	assert_eq(doc.bank_path, "")
	assert_false(doc.is_dirty())


func test_open_pair_finds_sibling_from_sbf():
	var doc = MusicEditorDocument.new()
	var err = doc.open_pair(_copy_temp_pair())
	assert_eq(err, OK)
	assert_true(doc.bank_loaded())
	assert_true(doc.script_loaded(), "sibling .bin found and loaded")


func test_open_pair_finds_sibling_from_bin():
	var doc = MusicEditorDocument.new()
	_copy_temp_pair()
	var err = doc.open_pair(PAIR_SCRIPT)
	assert_eq(err, OK)
	assert_true(doc.script_loaded())
	assert_true(doc.bank_loaded(), "sibling .sbf found and loaded")


func test_open_pair_missing_primary_returns_error():
	var doc = MusicEditorDocument.new()
	var orphan = "res://nonexistent/nope.sbf"
	var err = doc.open_pair(orphan)
	assert_ne(err, OK, "missing primary returns error")


func test_save_to_disk_writes_clean_pair():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	# Write to user:// to avoid clobbering fixtures
	doc.bank_path = "user://test_doc_save.sbf"
	doc.script_path = "user://test_doc_save.bin"
	var err = doc.save_to_disk()
	assert_eq(err, OK)
	assert_true(FileAccess.file_exists(doc.bank_path))
	assert_true(FileAccess.file_exists(doc.script_path))
	assert_false(doc.is_dirty(), "save clears dirty flags")


func test_reorder_track_marks_dirty():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	doc.reorder_track(0, 5)
	assert_true(doc._bank_dirty)


func test_rename_track_updates_name_and_dirty():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	doc.rename_track(0, &"NEWNAME01")
	assert_true(doc._bank_dirty)
	var entries = doc.bank.get_entries()
	assert_eq(entries[0]["name"], "NEWNAME01")


func test_delete_track_shrinks_count():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var before = doc.bank.get_entry_count()
	doc.delete_track(0)
	assert_eq(doc.bank.get_entry_count(), before - 1)
	assert_true(doc._bank_dirty)


func test_add_track_grows_count_and_marks_dirty():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var before = doc.bank.get_entry_count()
	var samples = PackedFloat32Array()
	samples.resize(8192)
	for i in range(samples.size()):
		samples[i] = 0.0
	doc.add_track(&"NEWTRACK", samples)
	assert_eq(doc.bank.get_entry_count(), before + 1)
	assert_true(doc._bank_dirty)
	var entries = doc.bank.get_entries()
	assert_eq(entries[before]["name"], "NEWTRACK")


func test_undo_redo_reorder():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var first_name_before: String = doc.bank.get_entries()[0]["name"]
	doc.reorder_track(0, 1)
	var first_after: String = doc.bank.get_entries()[0]["name"]
	assert_ne(first_after, first_name_before)
	assert_true(doc.can_undo_bank())
	doc.undo_bank()
	var first_undone: String = doc.bank.get_entries()[0]["name"]
	assert_eq(first_undone, first_name_before)
	assert_true(doc.can_redo_bank())
	doc.redo_bank()
	assert_eq(doc.bank.get_entries()[0]["name"], first_after)


func test_undo_redo_rename():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var original: String = doc.bank.get_entries()[0]["name"]
	doc.rename_track(0, &"RENAMED01")
	assert_eq(doc.bank.get_entries()[0]["name"], "RENAMED01")
	doc.undo_bank()
	assert_eq(doc.bank.get_entries()[0]["name"], original)
	doc.redo_bank()
	assert_eq(doc.bank.get_entries()[0]["name"], "RENAMED01")


func _copy_temp_pair() -> String:
	_copy_fixture(BANK_FIXTURE, PAIR_BANK)
	_copy_fixture(SCRIPT_FIXTURE, PAIR_SCRIPT)
	return PAIR_BANK


func _copy_fixture(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "Fixture should be readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	assert_not_null(dst, "Fixture destination should be writable: %s" % dst_path)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _remove_user_file(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)

