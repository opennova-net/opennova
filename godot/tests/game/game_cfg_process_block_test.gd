extends GutTest

## The game shell's process-lifetime game.cfg block (GameCfgStore over the
## native GameConfigBlock; docs/gamecfg/game-cfg-re.md, D-NET-335): a NovaWorld
## ServerCommand in the hosting wait (D-NET-383), the session create's mpreset
## exit (D-NET-385), and the shell's exit legs, the one that saves and the one
## that does not. The start's read and its exit run in a child process
## (cli_startup_test.gd).

const MainGameScript := preload("res://game/main_game.gd")

var _run_dir := ""
var _signals: Array = []


func before_each() -> void:
	_run_dir = OS.get_cache_dir().path_join("opennova_game_cfg_block_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()
	_signals.clear()


func after_each() -> void:
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)


func _cfg_path() -> String:
	return _run_dir.path_join(GameCfgStore.FILE_NAME)


func _saved_cfg() -> String:
	return FileAccess.get_file_as_string(_cfg_path())


func _record(name: String, value: Variant) -> void:
	_signals.append([name, value])


func test_the_block_keys_are_the_cfg_words() -> void:
	var keys := GameConfigBlock.word_keys()
	for key in [
		"SFXLevel",
		"DialogueLevel",
		"music_volume",
		"xhair_appearance",
		"xhair_color",
		"xhair_spread",
		"display_16x9",
		"enable_keyboardtips",
		"enable_gameplaytips",
		"object_polydetail",
		"texfilter_level",
		"particle_density",
		"texcompression_level",
		"hud_color_index",
		"hud_detail",
		"enable_slotmachine",
		"mpreset"
	]:
		assert_true(keys.has(key), "the block carries %s" % key)
	var block := GameCfgStore.block()
	assert_eq(block.get_path(), _cfg_path(), "game.cfg lives in the working directory")
	assert_false(block.is_file_read(), "no file: the defaults")
	assert_eq(block.get_word("mpreset"), 0)
	assert_eq(block.get_word("hud_color_index"), HudOverlay.hud_color_index_default())


func test_set_mp_reset_in_the_hosting_wait_is_stored_and_saved() -> void:
	# The world has no match yet (the hosting wait before the session create):
	# the command runs on a session-less context, where SetMPReset alone passes.
	var world := WorldFixture.make_world(self)
	var block := GameCfgStore.block()
	world.set_game_config(block)
	assert_null(world.get_sim(), "no match is bound")
	world.on_nw_host_server_command("SetServerName", "", PackedStringArray(["Renamed"]))
	assert_false(
		FileAccess.file_exists(_cfg_path()),
		"SetServerName needs the session: refused, nothing saved"
	)
	world.on_nw_host_server_command("TextChatServer", "", PackedStringArray(["hi"]))
	assert_false(FileAccess.file_exists(_cfg_path()))
	world.on_nw_host_server_command("SetMPReset", "", PackedStringArray(["1"]))
	assert_eq(block.get_word("mpreset"), 1, "the word lands in the process's block")
	assert_string_contains(_saved_cfg(), 'mpreset = "1"\r\n', "and game.cfg is saved at once")
	assert_false(_saved_cfg().contains("Renamed"), "the refused rename never reached the block")


func test_the_next_session_create_ends_the_process() -> void:
	# The block holds the word (a SetMPReset taken earlier): single player's
	# mission start creates a session, whose create ends the process with exit
	# code 0 in place of the load, reporting no load failure.
	var world := WorldFixture.make_world(self)
	var block := GameCfgStore.block()
	block.set_word("mpreset", 1)
	world.set_game_config(block)
	world.process_exit_requested.connect(func(code: int) -> void: _record("exit_code", code))
	world.load_failed.connect(func(reason: String) -> void: _record("load_failed", reason))
	var err := WorldFixture.load_mission(
		world, ProjectSettings.globalize_path(WorldFixture.BOOT_FIXTURE_DIR)
	)
	assert_ne(err, OK, "the load does not complete")
	assert_false(world.is_loaded(), "no mission exists past the create")
	var names := _signals.map(func(entry: Array) -> String: return String(entry[0]))
	assert_true(names.has("exit_code"), "the world requests the process exit")
	for entry in _signals:
		if entry[0] == "exit_code":
			assert_eq(entry[1], 0, "with exit code 0")
	assert_false(names.has("load_failed"), "a process exit is no load failure")
	assert_false(FileAccess.file_exists(_cfg_path()), "the create saves nothing")


func test_a_clear_word_lets_the_session_create_run() -> void:
	var world := WorldFixture.make_world(self)
	world.set_game_config(GameCfgStore.block())
	world.process_exit_requested.connect(func(code: int) -> void: _record("exit_code", code))
	var err := WorldFixture.load_mission(
		world, ProjectSettings.globalize_path(WorldFixture.BOOT_FIXTURE_DIR)
	)
	assert_eq(err, OK, "with mpreset clear the mission loads")
	assert_true(_signals.is_empty(), "and nothing asks the process to end")


func test_the_shell_process_exit_saves_nothing() -> void:
	# Out of the tree the SceneTree quit is skipped, so the shell's latch is the
	# observable: the exit writes neither game.cfg nor the profile, and a later
	# close request does not save either (the main loop's save never runs).
	var game: MainGame = MainGameScript.new()
	autofree(game)
	GameCfgStore.block().set_word("mpreset", 1)
	game.exit_process(0)
	assert_true(game.is_process_exit_pending())
	game.request_quit()
	assert_false(FileAccess.file_exists(_cfg_path()), "no game.cfg save")
	assert_false(FileAccess.file_exists(_run_dir.path_join("player.sav")), "no profile save")


func test_the_shell_quit_saves_the_block() -> void:
	# The main loop's exit saves game.cfg (the window's close, the menu's EXIT).
	var game: MainGame = MainGameScript.new()
	autofree(game)
	GameCfgStore.block().set_word("music_volume", 77)
	game.request_quit()
	assert_string_contains(_saved_cfg(), "music_volume         = 77\r\n")
