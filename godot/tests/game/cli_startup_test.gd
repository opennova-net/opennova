extends GutTest

## Real child processes prove the public CLI exit contract without terminating GUT.

var _dirs: Array[String] = []
var _saved_config := PackedByteArray()
var _had_config := false


func before_each() -> void:
	_had_config = FileAccess.file_exists(ResourceDirSettings.CONFIG_PATH)
	if _had_config:
		_saved_config = FileAccess.get_file_as_bytes(ResourceDirSettings.CONFIG_PATH)
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game("jo")


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	if _had_config:
		var config := FileAccess.open(ResourceDirSettings.CONFIG_PATH, FileAccess.WRITE)
		config.store_buffer(_saved_config)
		config.close()
	elif FileAccess.file_exists(ResourceDirSettings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(ResourceDirSettings.CONFIG_PATH))


func _launch(args: PackedStringArray, expected_exit: int) -> String:
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-render-loop", "--disable-crash-handler", "--quit-after", "10", "--",
	])
	command.append_array(args)
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_eq(status, expected_exit, log_text)
	assert_false(log_text.contains("SCRIPT ERROR"), log_text)
	assert_false(log_text.contains("GDExtension dynamic library not found"), log_text)
	return log_text


func test_missing_path_reports_usage_even_with_a_saved_directory() -> void:
	ConfigStore.write(ResourceDirSettings.CONFIG_PATH, "resources", "resource_dir", RuntimeFixture.directory())
	var log_text := _launch([], 2)
	assert_string_contains(log_text, "Usage: opennova.exe -- --resource-dir")


func test_missing_argument_value_reports_usage() -> void:
	assert_string_contains(_launch(["--resource-dir"], 2), "--resource-dir")


func test_nonexistent_path_fails_without_starting_a_mission() -> void:
	_launch(["--resource-dir", RuntimeFixture.directory().path_join("missing"),
			"--mission", "mnml.bms"], 1)


func test_loose_directory_requires_the_explicit_flag() -> void:
	_launch(["--resource-dir", RuntimeFixture.directory()], 1)
	_launch(["--resource-dir", RuntimeFixture.directory(), "--loose-root", "/d"], 0)


func test_packed_directory_with_spaces_boots_from_cli() -> void:
	var dir := OS.get_cache_dir().path_join("opennova cli packed %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	WorldFixture.stage_shell_archives(self, dir, false)
	_launch(["--resource-dir", dir], 0)
