extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
# The test WRITES the runtime config (ResourceDirSettings.set_resource_dir) and the editor
# under test writes its own (OnedSettings). Both are snapshotted: an unisolated single-file
# rerun that restored only one of them would leave the developer's real resource dir pointing
# at a temp path this test then deletes.
const CONFIG_PATHS: Array[String] = [OnedSettings.CONFIG_PATH, ResourceDirSettings.CONFIG_PATH]
const TEST_ROOT := "opennova_state_test"

var _saved_configs := {}


func before_each() -> void:
	_saved_configs.clear()
	for path in CONFIG_PATHS:
		if FileAccess.file_exists(path):
			_saved_configs[path] = FileAccess.get_file_as_bytes(path)


func after_each() -> void:
	for path in CONFIG_PATHS:
		if _saved_configs.has(path):
			var f := FileAccess.open(path, FileAccess.WRITE)
			if f != null:
				f.store_buffer(_saved_configs[path])
				f.close()
		elif FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func test_terrain_editor_path_state_preserves_shared_resource_directory() -> void:
	var root := OS.get_cache_dir().path_join(TEST_ROOT).path_join("root_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	ResourceDirSettings.set_resource_dir(root)

	var editor = autofree(TerrainEditorScript.new())
	editor._remember_open_path(root.path_join("Dvxi5.trn"))

	assert_eq(ResourceDirSettings.get_resource_dir(), root, "Saving terrain editor path state must preserve the shared resource directory.")


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
