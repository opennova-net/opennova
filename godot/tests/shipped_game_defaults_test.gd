extends GutTest

# A shipped download works with zero setup: ONED discovers the assets/ directory
# beside its executable. Discovery is only a view default; choosing a directory is the
# action that persists ONED state.


func _fresh_dir(label: String) -> String:
	var dir := OS.get_cache_dir().path_join("shipped_%s_%d" % [label, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


func test_bundled_assets_dir_requires_an_assets_sibling() -> void:
	var base := _fresh_dir("base")
	assert_eq(OnedApp.bundled_assets_dir(base), "",
			"no assets/ beside the exe -> no default (a dev run)")
	DirAccess.make_dir_recursive_absolute(base.path_join("assets"))
	assert_eq(OnedApp.bundled_assets_dir(base), base.path_join("assets"),
			"a shipped layout's assets/ sibling is the default resource directory")


func test_bundled_assets_discovery_never_persists() -> void:
	var config_path := OnedSettings.CONFIG_PATH
	var existed_before := FileAccess.file_exists(config_path)
	var bytes_before := FileAccess.get_file_as_bytes(config_path) \
			if existed_before else PackedByteArray()
	var base := _fresh_dir("exe")
	DirAccess.make_dir_recursive_absolute(base.path_join("assets"))

	assert_eq(OnedApp.bundled_assets_dir(base), base.path_join("assets"))
	assert_eq(FileAccess.file_exists(config_path), existed_before,
			"probing the bundled default does not create ONED settings")
	if existed_before:
		assert_eq(FileAccess.get_file_as_bytes(config_path), bytes_before,
				"probing the bundled default does not rewrite ONED settings")
