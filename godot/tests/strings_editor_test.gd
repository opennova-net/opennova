extends GutTest

## Document + engine-glue tests for the Strings workspace: byte-exact save,
## CRUD, undo/redo, validation, CSV round-trip, the .bin resource loader, and the
## NovaStrings runtime singleton.

const StringsEditorScript = preload("res://modtools/strings/strings_editor.gd")

const TMP_A := "user://rtxt_test_a.bin"
const TMP_B := "user://rtxt_test_b.bin"
const TMP_CSV := "user://rtxt_test.csv"
const TMP_LOADER := "user://rtxt_loader_test.bin"
const TMP_NS := "user://rtxt_ns_test.bin"


func after_all() -> void:
	for path in [TMP_A, TMP_B, TMP_CSV, TMP_LOADER, TMP_NS]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _sample_table() -> RtxtStringFile:
	var t := RtxtStringFile.new()
	t.add_section("menu_main")
	t.add_section("hud")
	t.add_entry("BTN_NEW_GAME", "{hot}New Game", 0, Vector2i(10, 20))
	t.add_entry("BTN_QUIT", "Quit", 0, Vector2i(10, 48))
	t.add_entry("HUD_AMMO", "", 1, Vector2i(-32, -16))
	return t


func test_save_reload_is_byte_exact() -> void:
	var t1 := _sample_table()
	assert_eq(t1.save_to_path(TMP_A), OK, "save should succeed")
	var bytes_a := FileAccess.get_file_as_bytes(TMP_A)

	var t2 := RtxtStringFile.new()
	assert_eq(t2.load_from_path(TMP_A), OK, "load should succeed")
	assert_eq(t2.save_to_path(TMP_B), OK, "re-save should succeed")
	var bytes_b := FileAccess.get_file_as_bytes(TMP_B)

	assert_eq(bytes_a, bytes_b, "an unedited table should re-serialize byte-for-byte")
	assert_eq(t2.get_entry_count(), 3, "all entries should reload")
	assert_eq(t2.get_string("btn_new_game"), "{hot}New Game", "lookup should be case-insensitive")
	assert_eq(t2.get_entry_position(2), Vector2i(-32, -16), "negative position hints should survive")


func test_crud_marks_dirty() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("menu")
	var idx: int = doc.add_entry("K", "V", 0, Vector2i(1, 2))
	assert_true(doc.is_dirty, "adding an entry should mark the document dirty")
	assert_eq(doc.string_table.get_entry_count(), 1)
	assert_eq(idx, 0)
	assert_eq(doc.string_table.get_entry_text(0), "V")
	assert_eq(doc.string_table.get_section_string_count(0), 1, "section count should track its entries")


func test_undo_redo_restores_state() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("menu")
	var idx: int = doc.add_entry("K", "V", 0, Vector2i())
	doc.remove_entry(idx)
	assert_eq(doc.string_table.get_entry_count(), 0, "entry should be removed")

	doc.undo()
	assert_eq(doc.string_table.get_entry_count(), 1, "undo should restore the entry")
	assert_eq(doc.string_table.get_entry_key(0), "K")

	doc.redo()
	assert_eq(doc.string_table.get_entry_count(), 0, "redo should remove it again")


func test_editing_session_is_one_undo_step() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("s")
	doc.add_entry("K", "old", 0, Vector2i())
	doc.begin_edit()
	doc.set_entry_text_live(0, "new")
	doc.commit_edit()
	assert_eq(doc.string_table.get_entry_text(0), "new", "live edit should land in the model")
	doc.undo()
	assert_eq(doc.string_table.get_entry_text(0), "old", "undo should restore the pre-session text")
	doc.redo()
	assert_eq(doc.string_table.get_entry_text(0), "new", "redo should reapply the edit")


func test_structural_ops_emit_structure_changed() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	watch_signals(doc)
	doc.add_section("s")
	doc.add_entry("K", "v", 0, Vector2i())
	assert_signal_emit_count(doc, "structure_changed", 2, "add section + add entry are structural")


func test_committed_edit_emits_edited_not_structure() -> void:
	# Live edits must not trigger a full rebuild — only the lightweight title sync.
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("s")
	doc.add_entry("K", "v", 0, Vector2i())
	watch_signals(doc)
	doc.begin_edit()
	doc.set_entry_text_live(0, "v2")
	doc.commit_edit()
	assert_signal_emitted(doc, "edited", "committing a session should mark the document edited")
	assert_signal_not_emitted(doc, "structure_changed", "a live edit must not trigger a structural rebuild")


func test_validation_flags_in_section_duplicate_and_empty() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("s")
	doc.add_entry("DUP", "a", 0, Vector2i())
	doc.add_entry("dup", "b", 0, Vector2i())  # case-insensitive duplicate, same section
	doc.add_entry("", "c", 0, Vector2i())     # empty key

	var report: Dictionary = doc.validate()
	assert_false(report["ok"], "duplicate + empty keys should fail validation")
	var issues: Dictionary = report["issues_by_index"]
	assert_false(issues.has(0), "the FIRST occurrence is the one the game resolves — not an issue")
	assert_true(issues.has(1), "the later in-section duplicate is unreachable in-game")
	assert_true(issues.has(2), "the empty-key row should be flagged")
	assert_string_contains(report["summary"], "duplicate")
	assert_string_contains(report["summary"], "empty")


func test_validation_allows_same_key_across_sections() -> void:
	# Retail tables legitimately reuse one key in several sections (lookups are
	# section-scoped, like the game's [orig: TextResource_FindEntryBySectionAndKey
	# @ 0x75D250]); that must not be flagged.
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("menu")
	doc.add_section("hud")
	doc.add_entry("SHARED", "menu text", 0, Vector2i())
	doc.add_entry("SHARED", "hud text", 1, Vector2i())

	var report: Dictionary = doc.validate()
	assert_true(report["ok"], "cross-section key reuse is valid retail data: %s" % report["summary"])
	assert_eq(doc.string_table.get_string_in_section("menu", "shared"), "menu text")
	assert_eq(doc.string_table.get_string_in_section("HUD", "SHARED"), "hud text")


func test_move_entry_to_section_keeps_grouping_and_undoes() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("a")
	doc.add_section("b")
	doc.add_entry("K1", "1", 0, Vector2i())
	doc.add_entry("K2", "2", 0, Vector2i())
	doc.add_entry("K3", "3", 1, Vector2i())

	var new_index: int = doc.move_entry_to_section(0, 1)
	assert_eq(new_index, 2, "moved entry should land at the end of its new section's run")
	assert_eq(doc.selected_index, new_index, "selection should follow the moved entry")
	assert_true(doc.string_table.is_grouped(), "moving must preserve the grouping invariant")
	assert_eq(doc.string_table.get_entry_key(2), "K1")
	assert_eq(doc.string_table.get_section_string_count(0), 1)
	assert_eq(doc.string_table.get_section_string_count(1), 2)

	doc.undo()
	assert_eq(doc.string_table.get_entry_key(0), "K1", "undo should restore the original order")
	assert_eq(doc.string_table.get_section_string_count(0), 2)


func test_ungrouped_file_is_flagged_and_normalizable() -> void:
	# Mutations keep tables grouped, so forge an ungrouped file by patching the
	# section_index of the FIRST entry directly in the serialized bytes
	# (entry i's section dword sits at 16 + 16*i + 8).
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("a")
	doc.add_section("b")
	doc.add_entry("K1", "1", 0, Vector2i())
	doc.add_entry("K2", "2", 0, Vector2i())
	doc.add_entry("K3", "3", 1, Vector2i())
	var bytes: PackedByteArray = doc.string_table.to_byte_array()
	bytes[16 + 8] = 1  # first entry now claims section b while sitting before section a's run

	var doc2 = add_child_autofree(StringsEditorScript.new())
	assert_eq(doc2.open_strings_bytes(bytes, "user://forged.bin"), OK, "ungrouped files still load (faithful read)")
	assert_false(doc2.string_table.is_grouped())
	var report: Dictionary = doc2.validate()
	assert_false(report["ok"], "ungrouped entries break the game's index-by-string-count lookup")
	assert_false(bool(report["grouped"]))
	assert_string_contains(report["summary"], "grouped")

	doc2.normalize_grouping()
	assert_true(doc2.string_table.is_grouped(), "normalize should restore grouping")
	assert_eq(doc2.string_table.get_section_string_count(0), 1)
	assert_eq(doc2.string_table.get_section_string_count(1), 2)
	doc2.undo()
	assert_false(doc2.string_table.is_grouped(), "normalize should be one undoable step")


func test_csv_round_trip_preserves_text_with_commas_and_newlines() -> void:
	var doc = add_child_autofree(StringsEditorScript.new())
	doc.add_section("menu")
	doc.add_entry("MULTI", "line1\nline2", 0, Vector2i(3, 4))
	doc.add_entry("PLAIN", "hello, world", 0, Vector2i())
	assert_eq(doc.export_csv(TMP_CSV), OK)

	var doc2 = add_child_autofree(StringsEditorScript.new())
	assert_eq(doc2.import_csv(TMP_CSV), OK)
	assert_eq(doc2.string_table.get_entry_count(), 2, "both rows should import")
	assert_eq(doc2.string_table.get_entry_key(0), "MULTI")
	assert_eq(doc2.string_table.get_entry_text(0), "line1\nline2", "newlines should survive the CSV round-trip")
	assert_eq(doc2.string_table.get_entry_text(1), "hello, world", "embedded commas should survive")
	assert_eq(doc2.string_table.get_entry_position(0), Vector2i(3, 4))
	assert_eq(doc2.string_table.get_section_name(0), "menu", "section names should rebuild from the CSV")


func test_strip_hotkey_statics() -> void:
	assert_eq(RtxtStringFile.strip_hotkey("{hot}New Game"), "New Game")
	var d := RtxtStringFile.strip_hotkey_with_index("Save {hot}As")
	assert_eq(String(d["text"]), "Save As")
	assert_eq(int(d["index"]), 5)
	var none := RtxtStringFile.strip_hotkey_with_index("Plain")
	assert_eq(int(none["index"]), -1)


func test_resource_loader_recognizes_rtxt_bin() -> void:
	var t := _sample_table()
	assert_eq(t.save_to_path(TMP_LOADER), OK)
	var loaded = ResourceLoader.load(TMP_LOADER, "", ResourceLoader.CACHE_MODE_IGNORE)
	assert_not_null(loaded, "the .bin loader should produce a resource")
	assert_true(loaded is RtxtStringFile, "loaded resource should be an RtxtStringFile")
	assert_eq(loaded.get_string("BTN_QUIT"), "Quit")


func test_committed_fixture_loads_and_reserializes_byte_exact() -> void:
	var fixture := "res://fixtures/strings/menu.bin"
	assert_true(FileAccess.file_exists(fixture), "the menu.bin fixture should be committed")
	var table := RtxtStringFile.new()
	assert_eq(table.load_from_path(fixture), OK, "the fixture should parse")
	assert_eq(table.get_entry_count(), 6, "fixture has six entries")
	assert_eq(table.get_section_count(), 3, "fixture has three sections")
	assert_eq(table.get_section_name(0), "menu_main")
	assert_eq(table.get_string("BTN_NEW_GAME"), "{hot}New Game")
	assert_eq(table.get_entry_position(5), Vector2i(-32, -16), "negative HUD position should parse")

	# The fixture bytes must equal what the C++ writer emits, so a no-op save is
	# byte-identical to the committed file.
	assert_eq(table.save_to_path(TMP_B), OK)
	assert_eq(FileAccess.get_file_as_bytes(fixture), FileAccess.get_file_as_bytes(TMP_B),
		"re-saving the unedited fixture should reproduce it byte-for-byte")


func test_retail_fixture_cp1252_survives_string_roundtrip() -> void:
	# 00tra.bin is a committed retail mission table whose text contains cp1252
	# bytes (curly quotes etc.). Rewriting every entry's text/key through the
	# Godot String layer must reproduce the file byte-for-byte — i.e. the
	# cp1252 decode/encode in RtxtStringFile is lossless on real game data.
	var fixture_path := ProjectSettings.globalize_path("res://").path_join("../fixtures/rtxt/00tra.bin")
	assert_true(FileAccess.file_exists(fixture_path), "retail fixture should exist at %s" % fixture_path)
	var original := FileAccess.get_file_as_bytes(fixture_path)
	var table := RtxtStringFile.new()
	assert_eq(table.load_from_byte_array(original), OK, "retail mission table should parse")
	assert_eq(table.to_byte_array(), original, "unedited retail table should reserialize byte-for-byte")

	for i in table.get_entry_count():
		table.set_entry_text(i, table.get_entry_text(i))
		table.set_entry_key(i, table.get_entry_key(i))
	assert_eq(table.to_byte_array(), original,
		"rewriting every entry through String must not corrupt cp1252 text")


func test_nova_strings_singleton() -> void:
	var t := _sample_table()
	assert_eq(t.save_to_path(TMP_NS), OK)
	assert_eq(NovaStrings.load_table(TMP_NS), OK)
	assert_true(NovaStrings.has_string("btn_new_game"), "lookup should be case-insensitive")
	assert_eq(NovaStrings.get_string("BTN_NEW_GAME"), "{hot}New Game")
	assert_eq(NovaStrings.get_display_string("BTN_NEW_GAME"), "New Game", "display should strip the {hot} marker")
	NovaStrings.clear()
	assert_false(NovaStrings.is_loaded())
