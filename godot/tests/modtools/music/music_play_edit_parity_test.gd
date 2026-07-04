extends GutTest

# Structured play-edit parity at the document level: insert/remove adjust the
# model by exactly one play, undo/redo round-trip, and a no-op insert+remove is
# byte-identical. The C++ mus_structured_play_edit_test proves the same at the
# libs/mus level; this guards the GDScript transform + the can_edit_plays gate.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_edit_pair.sbf"
const PAIR_SCRIPT := "user://music_edit_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _doc() -> MusicEditorDocument:
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	return doc


func _win000_plays(doc) -> int:
	var nm := StringName(doc.mus_script.get_default_script_name())
	for s in doc.mus_script.get_section_model(nm):
		if String(s.get("name", "")) == "Win000":
			return (s.get("plays", []) as Array).size()
	return -1


func test_can_edit_plays_true_for_shipped_script():
	var doc = _doc()
	assert_true(doc.can_edit_plays(), "shipped gamescript compiles cleanly -> editable")


func test_insert_play_adds_one_to_model():
	var doc = _doc()
	assert_eq(_win000_plays(doc), 6, "Win000 baseline 6 plays")
	assert_true(doc.insert_play(&"Win000", 0), "insert succeeds")
	assert_eq(_win000_plays(doc), 7, "model gains exactly one play")


func test_insert_then_undo_redo_round_trips():
	var doc = _doc()
	assert_true(doc.insert_play(&"Win000", 0))
	assert_eq(_win000_plays(doc), 7)
	assert_true(doc.can_undo(), "edit is undoable through the shared history")
	doc.undo()
	assert_eq(_win000_plays(doc), 6, "undo reverts the play")
	doc.redo()
	assert_eq(_win000_plays(doc), 7, "redo re-applies it")


func test_noop_insert_remove_is_byte_stable():
	var doc = _doc()
	# Baseline = the names-less canonical bytes the write path targets.
	var nm := StringName(doc.mus_script.get_default_script_name())
	var seed: String = doc.mus_script.get_decompiled_text(nm)
	var expected: PackedByteArray = doc.mus_script.compile_text(seed).get("file_bytes", PackedByteArray())
	assert_gt(expected.size(), 0, "names-less baseline compiles to bytes")
	assert_true(doc.insert_play(&"Win000", 0))
	assert_true(doc.remove_play(&"Win000", 0))
	assert_eq(doc._compiled_file_bytes, expected,
		"insert + remove re-encodes byte-identical to the baseline canonical bytes")


func test_insert_into_missing_section_is_a_noop():
	var doc = _doc()
	assert_false(doc.insert_play(&"NoSuchSection", 0), "unknown section -> no edit")


func test_noop_on_existing_track_is_byte_stable():
	# Multiplayerstart already plays sound_0 x20 then sound_1. The first cut got
	# this wrong (insert-after-last + remove-first reordered the score); the
	# symmetric remove-last must keep insert+remove byte-identical.
	var doc = _doc()
	var nm := StringName(doc.mus_script.get_default_script_name())
	var seed: String = doc.mus_script.get_decompiled_text(nm)
	var expected: PackedByteArray = doc.mus_script.compile_text(seed).get("file_bytes", PackedByteArray())
	assert_gt(expected.size(), 0, "baseline compiles")
	assert_true(doc.insert_play(&"Multiplayerstart", 0))
	assert_true(doc.remove_play(&"Multiplayerstart", 0))
	assert_eq(doc._compiled_file_bytes, expected, "no-op on an already-present track is byte-identical")


func test_remove_play_drops_one_and_undoes():
	var doc = _doc()
	assert_eq(_win000_plays(doc), 6)
	assert_true(doc.remove_play(&"Win000", 2), "remove track 2 (present in Win000)")
	assert_eq(_win000_plays(doc), 5, "exactly one play removed")
	assert_true(doc.can_undo(), "remove is undoable")
	doc.undo()
	assert_eq(_win000_plays(doc), 6, "undo restores the removed play")


func test_insert_rejects_out_of_range_track():
	var doc = _doc()
	assert_false(doc.insert_play(&"Win000", 256), "track > 255 cannot be a one-byte play operand")
	assert_eq(_win000_plays(doc), 6, "no play added on a rejected track")


func test_write_path_no_ops_without_a_script():
	var doc = MusicEditorDocument.new()  # nothing opened
	assert_false(doc.can_edit_plays(), "no script loaded -> not editable")
	assert_false(doc.insert_play(&"Win000", 0), "insert no-ops with no script")
	assert_false(doc.remove_play(&"Win000", 0), "remove no-ops with no script")


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
