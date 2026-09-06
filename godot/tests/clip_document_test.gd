extends GutTest

# ClipDocument over the committed anim fixtures: the parse answers in the
# file's own words and a document writes itself back byte for byte.

const IDLE := "res://../fixtures/anim/idle.bad"


func test_idle_fixture_loads_and_rewrites_byte_exact() -> void:
	var document := ClipDocument.new()
	assert_eq(document.load_from_path(IDLE), OK, document.get_last_error())
	assert_true(document.is_loaded())
	assert_eq(document.get_fps(), 30)
	assert_eq(document.get_frame_count(), 8)
	assert_true(document.is_loop())
	assert_false(document.has_translations())
	assert_eq(document.get_bone_count(), 1)
	assert_eq(document.get_bone_name(0), "root")
	assert_eq(document.get_bone_parent(0), -1)
	assert_eq(document.get_channel_key_count(0), 8)
	assert_eq(document.get_channel_frame_length(0, 3), 1)
	assert_true(document.get_channel_rotation(0, 0).is_normalized())
	assert_eq(document.get_event_count(), 9)
	assert_eq(document.get_translation(0, 0), Vector3.ZERO, "no translation block")
	var committed := FileAccess.get_file_as_bytes(IDLE)
	assert_eq(document.to_bytes(), committed, "write(parse(idle.bad)) is idle.bad")


func test_out_of_range_reads_are_inert() -> void:
	var document := ClipDocument.new()
	assert_eq(document.load_from_path(IDLE), OK)
	assert_eq(document.get_bone_name(5), "")
	assert_eq(document.get_channel_rotation(0, 99), Quaternion())
	assert_eq(document.get_event_trigger(-1), 0)


func test_garbage_bytes_are_refused() -> void:
	var document := ClipDocument.new()
	assert_ne(document.load_from_bytes(PackedByteArray([1, 2, 3])), OK)
	assert_false(document.is_loaded())
	assert_true(document.to_bytes().is_empty())
	assert_ne(document.get_last_error(), "")
