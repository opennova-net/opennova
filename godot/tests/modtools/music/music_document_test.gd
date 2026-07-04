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
	_remove_user_file("user://test_new_save.sbf")
	_remove_user_file("user://test_new_save.bin")


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


func test_compile_script_replaces_in_memory_script_before_save():
	var doc = MusicEditorDocument.new()
	var err := doc.open_script(SCRIPT_FIXTURE)
	assert_eq(err, OK, "open_script succeeded")
	var src := "script customscript\nsection Begin\n{\n  Var00 = 42\n  done\n}\n"
	doc.set_script_text(&"gamescript", src)
	var errors: Array = doc.compile_script()
	assert_eq(errors.size(), 0, "compile succeeds")
	assert_eq(doc.mus_script.get_default_script_name(), "customscript",
		"compiled script name is applied immediately, before save")
	var sections: PackedStringArray = doc.mus_script.get_section_names(&"customscript")
	assert_eq(sections.size(), 1, "compiled section table replaces the fixture table")
	assert_eq(String(sections[0]), "Begin")
	assert_true(doc._script_dirty, "compile updates runnable resource but save still owns persistence")


# C1 regression: with a bank loaded, the lazy compile-buffer seed must use the
# NAMES-LESS decompile (the proven-recompilable form), NOT the names-aware one
# (real bank names as bind identifiers, which need not recompile). A names-aware
# seed could make the live VM run -- or Save write -- a script that won't compile.
func test_seed_uses_names_less_decompile_when_bank_loaded():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	assert_true(doc.bank_loaded() and doc.script_loaded(), "pair loaded")
	var name := StringName(doc.mus_script.get_default_script_name())
	var names_less: String = doc.mus_script.get_decompiled_text(name)
	# Empty buffer on a fresh open -> compile_script triggers the lazy seed.
	var errors: Array = doc.compile_script()
	assert_eq(errors.size(), 0, "seeded buffer compiles with a bank loaded")
	assert_eq(doc._compiled_script_text, names_less,
		"compile buffer seeded from the names-less decompile, not names-aware")


func test_undo_redo_reorder():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var first_name_before: String = doc.bank.get_entries()[0]["name"]
	doc.reorder_track(0, 1)
	var first_after: String = doc.bank.get_entries()[0]["name"]
	assert_ne(first_after, first_name_before)
	assert_true(doc.can_undo())
	doc.undo()
	var first_undone: String = doc.bank.get_entries()[0]["name"]
	assert_eq(first_undone, first_name_before)
	assert_true(doc.can_redo())
	doc.redo()
	assert_eq(doc.bank.get_entries()[0]["name"], first_after)


func test_undo_redo_rename():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var original: String = doc.bank.get_entries()[0]["name"]
	doc.rename_track(0, &"RENAMED01")
	assert_eq(doc.bank.get_entries()[0]["name"], "RENAMED01")
	doc.undo()
	assert_eq(doc.bank.get_entries()[0]["name"], original)
	doc.redo()
	assert_eq(doc.bank.get_entries()[0]["name"], "RENAMED01")


# --- Create from scratch (new_project) -----------------------------------

func test_new_project_creates_editable_script_and_bank():
	var doc = MusicEditorDocument.new()
	var err = doc.new_project()
	assert_eq(err, OK, "new_project succeeds")
	assert_true(doc.script_loaded(), "a fresh script is loaded")
	assert_true(doc.bank_loaded(), "a fresh empty bank is loaded")
	assert_true(doc.is_dirty(), "a brand-new unsaved project is dirty")
	assert_true(doc.can_author(), "the fresh script compiles and is editable")
	assert_eq(doc.bank.get_entry_count(), 0, "the new bank starts empty")
	var name := StringName(doc.mus_script.get_default_script_name())
	var sections: PackedStringArray = doc.mus_script.get_section_names(name)
	assert_eq(sections.size(), 1, "one start state")
	assert_eq(String(sections[0]), "Begin", "the start state is named Begin")


func test_new_project_first_section_is_entry():
	var doc = MusicEditorDocument.new()
	doc.new_project()
	var name := StringName(doc.mus_script.get_default_script_name())
	var ast: Array = doc.mus_script.get_program_ast(name)
	assert_eq(ast.size(), 1, "one section in the AST")
	assert_true(bool(ast[0].get("is_entry", false)), "the first state is the start")


func test_new_project_then_author_states_and_play():
	var doc = MusicEditorDocument.new()
	doc.new_project()
	# Add a second state on the fresh script.
	assert_true(doc.add_section(&"Combat"), "add_section works on a from-scratch script")
	var name := StringName(doc.mus_script.get_default_script_name())
	assert_eq(doc.mus_script.get_section_names(name).size(), 2, "two states now")
	# Import a track, then drop a play of it onto the start state.
	var samples := PackedFloat32Array()
	samples.resize(8192)
	doc.add_track(&"TONE", samples)
	assert_eq(doc.bank.get_entry_count(), 1, "track imported into the new bank")
	assert_true(doc.insert_play(&"Begin", 0), "insert_play works on the from-scratch script")


func test_new_project_saves_and_reopens_roundtrip():
	var doc = MusicEditorDocument.new()
	doc.new_project()
	var samples := PackedFloat32Array()
	samples.resize(8192)
	doc.add_track(&"TONE", samples)
	doc.script_path = "user://test_new_save.bin"
	doc.bank_path = "user://test_new_save.sbf"
	var err = doc.save_to_disk()
	assert_eq(err, OK, "a brand-new project saves to disk")
	assert_true(FileAccess.file_exists(doc.script_path), ".bin written")
	assert_true(FileAccess.file_exists(doc.bank_path), ".sbf written")
	assert_false(doc.is_dirty(), "save clears dirty flags")
	# Reopen the freshly-written pair in a clean document.
	var doc2 = MusicEditorDocument.new()
	var e2 = doc2.open_pair("user://test_new_save.bin")
	assert_eq(e2, OK, "the saved pair reopens")
	assert_true(doc2.script_loaded(), "script reloads")
	assert_true(doc2.bank_loaded(), "bank reloads")
	var name2 := StringName(doc2.mus_script.get_default_script_name())
	assert_eq(String(doc2.mus_script.get_section_names(name2)[0]), "Begin",
		"the start state round-trips")
	assert_eq(doc2.bank.get_entry_count(), 1, "the imported track round-trips")


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
