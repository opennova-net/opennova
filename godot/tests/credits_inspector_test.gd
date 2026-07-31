extends GutTest

const CreditsWorkspaceScript = preload("res://modtools/credits/credits_workspace.gd")
const KDA_SOURCE_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const TEMP_DIR := "user://test_credits_inspector"

var _kda_path: String = ""


func before_each() -> void:
	var abs := ProjectSettings.globalize_path(TEMP_DIR)
	DirAccess.make_dir_recursive_absolute(abs)
	_kda_path = TEMP_DIR.path_join("nlist_%d.kda" % Time.get_ticks_usec())
	var src := FileAccess.open(
		ProjectSettings.globalize_path(KDA_SOURCE_PATH), FileAccess.READ)
	if src != null:
		var dst := FileAccess.open(
			ProjectSettings.globalize_path(_kda_path), FileAccess.WRITE)
		if dst != null:
			dst.store_buffer(src.get_buffer(src.get_length()))
			dst.close()
		src.close()


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)
	_kda_path = ""


func _cleanup_dir(dir_path: String) -> void:
	var abs := ProjectSettings.globalize_path(dir_path)
	var da := DirAccess.open(abs)
	if da == null:
		return
	da.list_dir_begin()
	var fname := da.get_next()
	while fname != "":
		if not da.current_is_dir():
			da.remove(fname)
		fname = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs)


func test_build_inspector_renders_file_and_counts() -> void:
	var workspace = autofree(CreditsWorkspaceScript.new())
	var err = workspace.open_file(_kda_path)
	assert_eq(err, OK, "open_file should succeed for the bundled fixture")
	if err != OK:
		return

	var mount := Control.new()
	add_child_autofree(mount)
	workspace.build_inspector(mount)
	await get_tree().process_frame

	assert_gt(mount.get_child_count(), 0, "inspector mounts a child")
	var box := mount.get_child(0).get_node_or_null("Box")
	assert_not_null(box, "inspector has a Box VBox")
	var file_label: Label = box.get_node_or_null("FileLabel") as Label
	var entries_label: Label = box.get_node_or_null("EntriesLabel") as Label
	assert_not_null(file_label, "FileLabel exists")
	assert_not_null(entries_label, "EntriesLabel exists")
	assert_string_contains(file_label.text, "nlist", "file label shows the loaded basename")
	assert_string_contains(entries_label.text, "entries", "entries label includes a count line")


func test_inspector_refreshes_on_state_change() -> void:
	var workspace = autofree(CreditsWorkspaceScript.new())
	var err = workspace.new_current()
	assert_eq(err, OK, "new_current should succeed")

	var mount := Control.new()
	add_child_autofree(mount)
	workspace.build_inspector(mount)
	await get_tree().process_frame

	var entries_label: Label = mount.get_child(0).get_node("Box/EntriesLabel") as Label
	var initial_text := entries_label.text

	# Mutate the resource so state_changed fires.
	var entry := CbinTextEntry.new()
	workspace._document.resource.add_entry(entry)
	await get_tree().process_frame

	assert_ne(entries_label.text, initial_text, "entries label updates after add_entry")
