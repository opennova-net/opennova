extends GutTest

## Real child processes prove the public CLI exit contract without terminating GUT.

var _dirs: Array[String] = []
var _config: TestFs.Snapshot


func before_each() -> void:
	_config = TestFs.snapshot("user://opennova.cfg")


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	_config.restore()


## Run the game binary with `args` after `--`; `verbose` turns on Godot's
## --verbose log so print_verbose boot markers reach the captured output.
func _launch(args: PackedStringArray, expected_exit: int, verbose := false,
		default_game := true) -> String:
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-render-loop", "--disable-crash-handler", "--quit-after", "10",
	])
	if verbose:
		command.append("--verbose")
	command.append("--")
	command.append("--no-update")
	if default_game and not args.has("/game"):
		command.append_array(PackedStringArray(["/game", "jo"]))
	command.append_array(args)
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_eq(status, expected_exit, log_text)
	assert_false(log_text.contains("SCRIPT ERROR"), log_text)
	assert_false(log_text.contains("GDExtension dynamic library not found"), log_text)
	return log_text


func test_empty_opennova_install_reaches_bootstrap_without_game_data() -> void:
	var dir := TestFs.cache_dir(self, "empty_bootstrap")
	_dirs.append(dir)
	var log_text := _launch(["/game", "acidgambit", "--resource-dir", dir], 1, true)
	assert_string_contains(log_text, "OpenNova: content installation required")
	assert_false(log_text.contains("no menu found in resource dir"), log_text)


# ADR 0067 d2: without /game the folder's UPDATE.WIZ names the product. A wizard
# naming a game OpenNova does not play stops the launch before anything mounts; a
# JO or DFX2 wizard selects that game, and a folder holding only the wizard (the
# jointops and dfx2 zips, unzipped) offers that game's download (ADR 0067 d3).
func test_a_folder_wizard_selects_or_refuses_the_product_without_game() -> void:
	var foreign := TestFs.cache_dir(self, "foreign_wizard")
	_dirs.append(foreign)
	TestFs.write_text(self, foreign.path_join("UPDATE.WIZ"), "[HEADER]\r\nCMD=DFBHD.CMD\r\n")
	assert_string_contains(_launch(["--resource-dir", foreign], 2, false, false),
			"holds a game OpenNova does not play")
	for game in [["jo", "JOPTR.CMD", "Joint Operations"], ["dfx2", "DFX2.CMD", "Delta Force Xtreme 2"]]:
		var dir := TestFs.cache_dir(self, "%s_wizard" % game[0])
		_dirs.append(dir)
		TestFs.write_text(self, dir.path_join("UPDATE.WIZ"), "[HEADER]\r\nCMD=%s\r\n" % game[1])
		var log_text := _launch(["--resource-dir", dir], 1, true, false)
		assert_string_contains(log_text, "OpenNova: content installation required")
		assert_string_contains(log_text, "%s is not installed here" % game[2])


func test_unknown_or_missing_game_selection_is_a_usage_error() -> void:
	_launch(["/game", "unknown"], 2)
	_launch(["/game"], 2)


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
	WorldFixture.stage_shell_archives(self, dir, false)
	var cfg_path := dir.path_join("game.cfg")
	var text := "version = 29\r\nmpreset = 1\r\n"
	var file := FileAccess.open(cfg_path, FileAccess.WRITE)
	file.store_string(text)
	file.close()
	var log_text := _launch(["--working-dir", dir, "--resource-dir", dir], 0, true)
	assert_false(
		log_text.contains("OpenNova: menu up from"),
		"the process ends before the menu: %s" % log_text
	)
	assert_string_contains(log_text,
			"GameRuntimeRoot: game.cfg's mpreset ends the process at the start (exit code 0)",
			"the start's exit names itself (D-NET-310)")
	assert_eq(FileAccess.get_file_as_string(cfg_path), text, "game.cfg is left as read")
	assert_false(FileAccess.file_exists(dir.path_join("player.sav")), "no profile save")

	file = FileAccess.open(cfg_path, FileAccess.WRITE)
	file.store_string("version = 29\r\nmpreset = 0\r\n")
	file.close()
	log_text = _launch(["--working-dir", dir, "--resource-dir", dir], 0, true)
	assert_string_contains(log_text, "OpenNova: menu up from")
	assert_true(
		FileAccess.file_exists(dir.path_join("player.sav")), "the menu's start saved the profile"
	)
