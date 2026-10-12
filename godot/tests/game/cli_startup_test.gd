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


func test_game_cfg_with_mpreset_ends_the_process_at_the_start() -> void:
	# The start's read of game.cfg in the working directory: a set mpreset ends
	# the process there with exit code 0 (docs/gamecfg/game-cfg-re.md, the
	# load; D-NET-385), before the menu mounts and with nothing written: the
	# file stays as it was and no profile is saved. The same directory without
	# the word boots the menu, which saves the profile (the control).
	var dir := OS.get_cache_dir().path_join("opennova_cli_mpreset_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	var cfg_path := dir.path_join("game.cfg")
	var text := "version = 29\r\nmpreset = 1\r\n"
	var file := FileAccess.open(cfg_path, FileAccess.WRITE)
	file.store_string(text)
	file.close()
	var log_text := _launch(["--working-dir", dir], 0, true)
	assert_false(
		log_text.contains("OpenNova: bundled menu up from"),
		"the process ends before the menu: %s" % log_text
	)
	assert_eq(FileAccess.get_file_as_string(cfg_path), text, "game.cfg is left as read")
	assert_false(FileAccess.file_exists(dir.path_join("player.sav")), "no profile save")

	file = FileAccess.open(cfg_path, FileAccess.WRITE)
	file.store_string("version = 29\r\nmpreset = 0\r\n")
	file.close()
	log_text = _launch(["--working-dir", dir], 0, true)
	assert_string_contains(log_text, "OpenNova: bundled menu up from")
	assert_true(
		FileAccess.file_exists(dir.path_join("player.sav")), "the menu's start saved the profile"
	)
