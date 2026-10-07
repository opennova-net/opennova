extends GutTest

## Real child processes prove the public CLI exit contract without terminating GUT.

var _dirs: Array[String] = []
var _config: TestFs.Snapshot


func before_each() -> void:
	_config = TestFs.snapshot(ResourceDirSettings.CONFIG_PATH)
	ResourceDirSettings.set_game("jo")


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	_config.restore()


## Run the game binary with `args` after `--`; `verbose` turns on Godot's
## --verbose log so print_verbose boot markers reach the captured output.
func _launch(args: PackedStringArray, expected_exit: int, verbose := false) -> String:
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-render-loop", "--disable-crash-handler", "--quit-after", "10",
	])
	if verbose:
		command.append("--verbose")
	command.append("--")
	command.append_array(args)
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_eq(status, expected_exit, log_text)
	assert_false(log_text.contains("SCRIPT ERROR"), log_text)
	assert_false(log_text.contains("GDExtension dynamic library not found"), log_text)
	return log_text


func test_no_path_boots_the_bundled_menu() -> void:
	# ADR 0048: no --resource-dir mounts the bundled assets/ placeholder menu;
	# MainGame logs the marker once that menu is up.
	var log_text := _launch([], 0, true)
	assert_string_contains(log_text, "OpenNova: bundled menu up from")
	assert_false(log_text.contains("bundled assets not found"), log_text)
	assert_false(log_text.contains("no menu found in resource dir"), log_text)


func test_missing_argument_value_reports_usage() -> void:
	assert_string_contains(_launch(["--resource-dir"], 2), "--resource-dir needs a path")


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
