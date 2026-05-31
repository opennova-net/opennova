extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const TEST_ROOT := "opennova_state_test"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()


func after_each() -> void:
	if _had_state_config:
		var f := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if f != null:
			f.store_buffer(_saved_state_config)
			f.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func test_terrain_editor_path_state_preserves_shared_resource_directory() -> void:
	var root := OS.get_cache_dir().path_join(TEST_ROOT).path_join("root_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	NovaResourceDirSettings.set_resource_dir(root)

	var editor = autofree(TerrainEditorScript.new())
	editor._remember_open_path(root.path_join("Dvxi5.trn"))

	assert_eq(NovaResourceDirSettings.get_resource_dir(), root, "Saving terrain editor path state must preserve the shared resource directory.")


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
