extends GutTest

## Document tests for the Sound workspace controller: build -> save -> reload
## round-trip, undo/redo, the no-op-edit guard, and a guarded byte-exact check
## against the real JO fixture (repo fixtures/lwf/00TRa.LWF) via the NovaLwfData
## GDExtension wrapper.

const SoundControllerScript = preload("res://modtools/sound/sound_controller.gd")

const TMP_DIR := "user://lwf_sound_test"


func after_all() -> void:
	var abs := ProjectSettings.globalize_path(TMP_DIR)
	var dir := DirAccess.open(abs)
	if dir != null:
		for f in dir.get_files():
			dir.remove(f)
		DirAccess.remove_absolute(abs)


func _new_ctrl() -> SoundController:
	var c := SoundControllerScript.new()
	add_child_autofree(c)
	return c


func test_build_save_reload_roundtrip() -> void:
	var c := _new_ctrl()
	c.new_profile(false)
	assert_eq(c.data.get_set_count(), 1, "new_profile seeds one default set")
	assert_eq(c.data.get_layer_count(0), 1, "new_profile seeds one default layer")

	var mi := c.add_member(0, 0)
	c.set_member_field_live(0, 0, mi, "wav_path", "Z00aR100.wav")
	c.set_member_field_live(0, 0, mi, "volume", 128)
	c.set_set_field_live(0, "name", "TESTSET")
	c.commit_edit()
	assert_true(c.is_dirty, "edits dirty the document")

	assert_eq(c.save_as(TMP_DIR), OK, "save_as should succeed")
	var path := c.current_path
	assert_false(c.is_dirty, "save clears dirty")

	var c2 := _new_ctrl()
	assert_eq(c2.open_lwf(path), OK, "reopen should succeed")
	assert_eq(c2.data.get_set_count(), 1)
	assert_eq(String(c2.data.get_set(0).get("name", "")), "TESTSET")
	var m := c2.data.get_member(0, 0, 0)
	assert_eq(String(m.get("wav_path", "")), "Z00aR100.wav")
	assert_eq(int(m.get("volume", 0)), 128)


func test_undo_redo() -> void:
	var c := _new_ctrl()
	c.new_profile(false)
	var before := c.data.get_set_count()
	c.add_set()
	assert_eq(c.data.get_set_count(), before + 1)
	assert_true(c.can_undo())
	c.undo()
	assert_eq(c.data.get_set_count(), before, "undo removes the added set")
	assert_true(c.can_redo())
	c.redo()
	assert_eq(c.data.get_set_count(), before + 1, "redo re-adds it")


func test_noop_edit_does_not_dirty() -> void:
	var c := _new_ctrl()
	c.new_profile(false)
	c.add_member(0, 0)
	assert_eq(c.save_as(TMP_DIR), OK)
	var path := c.current_path

	var c2 := _new_ctrl()
	assert_eq(c2.open_lwf(path), OK)
	assert_false(c2.is_dirty)
	var vol := int(c2.data.get_member(0, 0, 0).get("volume", 0))
	c2.set_member_field_live(0, 0, 0, "volume", vol)  # unchanged value
	c2.commit_edit()
	assert_false(c2.is_dirty, "re-setting a field to its current value must not dirty")


func test_real_fixture_byte_exact() -> void:
	# Repo fixtures/ live one level above res:// (the godot/ project dir).
	var path := ProjectSettings.globalize_path("res://").path_join("../fixtures/lwf/00TRa.LWF")
	if not FileAccess.file_exists(path):
		pass_test("repo fixture fixtures/lwf/00TRa.LWF not present; skipping")
		return
	var c := _new_ctrl()
	assert_eq(c.open_lwf(path), OK, "real .lwf should parse")
	assert_gt(c.data.get_set_count(), 0, "fixture has sound sets")
	var original := FileAccess.get_file_as_bytes(path)
	var encoded := c.data.to_bytes()
	assert_eq(encoded, original, "unmodified to_bytes() must equal the original bytes")
