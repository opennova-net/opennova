extends GutTest

const WORLD_TEST_ROOT := "nova_world_test"


func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	_write_fixture_file(root.path_join("Dvxi5.trn"), "terrain_name \"Dvxi5\"\n")

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	assert_eq(world.load_world(root), ERR_FILE_NOT_FOUND, "Runtime global root must contain full_00.env next to Dvxi5.trn.")


func _make_world() -> NovaWorld:
	var world := NovaWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	return world


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


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
