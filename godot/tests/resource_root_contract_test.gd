extends GutTest


func test_runtime_and_modtools_do_not_ship_resource_roots() -> void:
	var forbidden_dirs := [
		"res://" + "assets",
		"res://modtools/" + "assets",
	]
	for dir_path in forbidden_dirs:
		assert_false(
			DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(dir_path)),
			"%s must not exist; game data belongs in the configured resource root." % dir_path
		)


func test_resource_root_resolves_only_top_level_files() -> void:
	var root := _make_flat_root("flat_resolve")
	_write_file(root.path_join("Alpha.TRN"), "trn")
	DirAccess.make_dir_recursive_absolute(root.path_join("terrains"))
	_write_file(root.path_join("terrains/Dvxi5.trn"), "nested")

	var resources := NovaResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)

	assert_eq(_norm(resources.resolve_file("alpha.trn")), _norm(root.path_join("Alpha.TRN")))
	assert_eq(resources.resolve_file("terrains/alpha.trn"), "", "Resource names must be flat basenames, not nested paths.")
	assert_string_contains(resources.get_last_error(), "flat filename", "Pathful lookups should explain the flat resource-root contract.")
	assert_eq(resources.resolve_file("Dvxi5.trn"), "", "Nested files are not part of the flat resource root.")

	var trns := resources.list_files(".trn")
	assert_eq(trns.size(), 1)
	assert_eq(String(trns[0]).get_file(), "Alpha.TRN")


func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join("opennova_resource_root_contract"))


func _make_flat_root(name: String) -> String:
	var root := OS.get_cache_dir().path_join("opennova_resource_root_contract").path_join("%s_%d" % [name, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	return root


func _write_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func _norm(path: String) -> String:
	return path.replace("\\", "/").to_lower()


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
