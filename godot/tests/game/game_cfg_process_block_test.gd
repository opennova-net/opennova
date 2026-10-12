extends GutTest

## The game shell's process-lifetime game.cfg block (GameCfgStore over the
## native GameConfigBlock; docs/gamecfg/game-cfg-re.md, D-NET-335): a NovaWorld
## ServerCommand in the hosting wait (D-NET-383), the session create's mpreset
## exit (D-NET-385), the main-menu entry's and the Mods reload's mpreset exits
## (the NovaWorld session a return kept going with the exit), a join's
## expansion switch that reads nothing, and the shell's exit legs, the one that
## saves and the one that does not. The
## start's read and its exit run in a child process (cli_startup_test.gd). A
## shell booted in the tree takes a process_exit_handler, so its exit ends no
## GUT run.

const MainGameScript := preload("res://game/main_game.gd")

var _run_dir := ""
var _signals: Array = []
var _shell: MainGame = null
var _shell_dir := ""
var _exit_codes: Array[int] = []


func before_each() -> void:
	_run_dir = OS.get_cache_dir().path_join("opennova_game_cfg_block_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()
	_signals.clear()
	_exit_codes.clear()


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _shell_dir.is_empty():
		TestFs.remove_dir_recursive(_shell_dir)
		_shell_dir = ""
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)


# The packed shell in its main menu, running in this test's run directory
# (game.cfg and player.sav there), its process exits recorded.
func _boot_shell() -> MainGame:
	_shell = await WorldFixture.boot_shell(self, _run_dir)
	_shell_dir = WorldFixture.last_shell_dir()
	if _shell != null:
		_shell.process_exit_handler = func(code: int) -> void: _exit_codes.append(code)
	return _shell


func _await_exit(frame_limit := 60) -> void:
	for _frame in range(frame_limit):
		if not _exit_codes.is_empty():
			return
		await get_tree().process_frame


func _cfg_path() -> String:
	return _run_dir.path_join(GameCfgStore.FILE_NAME)


func _saved_cfg() -> String:
	return FileAccess.get_file_as_string(_cfg_path())


func _record(name: String, value: Variant) -> void:
	_signals.append([name, value])


func test_the_shell_words_are_the_cfg_words() -> void:
	# The words the shell keeps, by their game.cfg keys and without case, at
	# the defaults' values (docs/gamecfg/game-cfg-re.md, the key table).
	var block := GameCfgStore.block()
	var defaults := {
		"SFXLevel": 192,
		"DialogueLevel": 192,
		"music_volume": 192,
		"xhair_appearance": 0,
		"xhair_color": 0xFFFFFF,
		"xhair_spread": 1,
		"enable_keyboardtips": 1,
		"enable_gameplaytips": 1,
		"hud_color_index": 2,
		"hud_detail": 0,
		"enable_slotmachine": 0,
		"mpreset": 0,
		"MPRESET": 0,
	}
	for key: String in defaults:
		assert_eq(block.get_word(key), int(defaults[key]), "%s at its default" % key)
	block.set_word("sfxlevel", 7)
	assert_eq(block.get_word("SFXLevel"), 7, "a key matches without case")
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


func test_the_shell_quit_saves_the_block() -> void:
	# The main loop's exit saves game.cfg (the window's close, the menu's EXIT).
	var game: MainGame = MainGameScript.new()
	autofree(game)
	GameCfgStore.block().set_word("music_volume", 77)
	game.request_quit()
	assert_string_contains(_saved_cfg(), "music_volume         = 77\r\n")


func test_an_exit_at_the_return_to_menu_read() -> void:
	# Every main-menu entry reads game.cfg (the MainMenu scene's init), the
	# return from a mission's included: a file that sets mpreset ends the
	# process there with exit code 0 and nothing saved.
	var shell := await _boot_shell()
	if shell == null:
		return
	assert_true(await WorldFixture.start_shell_mission(self, shell), "the mission loads")
	var text := "version = 29\r\nmpreset = 1\r\n"
	var file := FileAccess.open(_cfg_path(), FileAccess.WRITE)
	file.store_string(text)
	file.close()
	assert_eq(shell.return_to_menu(), OK)
	await _await_exit()
	assert_eq(_exit_codes, [0] as Array[int], "the menu entry's read ends the process with 0")
	assert_true(shell.is_process_exit_pending())
	assert_eq(FileAccess.get_file_as_string(_cfg_path()), text, "the exit saves nothing")


func test_an_exit_at_the_mods_reload() -> void:
	# The Mods list's switch saves the block, then reads it through the menu's
	# init: a word a SetMPReset stored ends the process there with exit code 0.
	var shell := await _boot_shell()
	if shell == null:
		return
	GameCfgStore.block().set_word("mpreset", 1)
	shell.get_menu_shell().game_reloaded.emit()
	await _await_exit()
	assert_eq(_exit_codes, [0] as Array[int], "the reload's read ends the process with 0")
	assert_string_contains(_saved_cfg(), 'mpreset = "1"\r\n', "the reload saved before its read")


func test_every_main_menu_entry_reads_the_file() -> void:
	# The first entry reads over the mount; each later one (a return from a
	# mission, a cancelled join) reads the file again, and exits on mpreset.
	assert_true(GameCfgStore.enter_main_menu(null))
	var file := FileAccess.open(_cfg_path(), FileAccess.WRITE)
	file.store_string("version = 29\r\nmusic_volume = 33\r\n")
	file.close()
	assert_true(GameCfgStore.enter_main_menu(null))
	assert_eq(GameCfgStore.block().get_word("music_volume"), 33, "the re-entry read the file")
	file = FileAccess.open(_cfg_path(), FileAccess.WRITE)
	file.store_string("version = 29\r\nmpreset = 1\r\n")
	file.close()
	assert_false(GameCfgStore.enter_main_menu(null), "a set mpreset ends the process there")


func test_a_join_switch_leaves_the_next_entry_a_plain_read() -> void:
	# A join's switch to the host's expansion hands the block the new mount's
	# weapon table and reads nothing: the main-menu entry after the match reads
	# game.cfg over that mount with no save first, so a word changed in memory
	# since (a HUD colour cycled in the match) is the file's again.
	var install := _run_dir.path_join("install")
	assert_eq(DirAccess.make_dir_recursive_absolute(install.path_join("expansion/jox01")), OK)
	WorldFixture.write_pff(
		self, install.path_join("resource.pff"), [{"name": "basetag.txt", "bytes": "BASE"}]
	)
	WorldFixture.write_pff(
		self, install.path_join("expansion/jox01/jox01.pff"), [{"name": "exptag.txt", "bytes": "EXP"}]
	)
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(install, ""), OK)
	var world := WorldFixture.make_world(self)
	world.set_resource_root(root)
	var block := GameCfgStore.block()
	world.set_game_config(block)
	var text := "version = 29\r\nmusic_volume = 33\r\n"
	_write_cfg(text)
	assert_true(GameCfgStore.enter_main_menu(root), "the menu entry over the base game")
	assert_eq(block.get_word("music_volume"), 33)
	block.set_word("music_volume", 77)
	assert_eq(world.mount_join_expansion("jox01"), "", "the join switches to the host's expansion")
	assert_eq(root.get_expansion(), "jox01")
	assert_eq(block.get_word("music_volume"), 77, "the switch reads nothing")
	assert_eq(_saved_cfg(), text, "and saves nothing")
	assert_true(GameCfgStore.enter_main_menu(root), "the entry after the match")
	assert_eq(block.get_word("music_volume"), 33, "a plain read: the word is the file's again")
	assert_eq(_saved_cfg(), text, "with no save before it")
	root.clear()


func test_an_exit_at_the_return_ends_the_kept_novaworld_session() -> void:
	# A return that keeps the NovaWorld session (a match entered from the
	# NovaWorld menu) whose menu read ends the process: the session goes with
	# the exit, its goodbye burst alone, and no NovaWorld menu follows.
	var shell := await _boot_shell()
	if shell == null:
		return
	assert_true(await WorldFixture.start_shell_mission(self, shell), "the mission loads")
	var session := NovaWorldClient.new()
	shell.get_world().adopt_novaworld_client(session)
	_write_cfg("version = 29\r\nmpreset = 1\r\n")
	assert_eq(shell.return_to_menu(), OK)
	assert_eq(_exit_codes, [0] as Array[int], "the menu entry's read ends the process with 0")
	assert_false(is_instance_valid(session), "the kept session went with the exit")
	assert_null(shell.find_child("NovaWorldPanel", true, false), "no NovaWorld menu follows")


func _write_cfg(text: String) -> void:
	var file := FileAccess.open(_cfg_path(), FileAccess.WRITE)
	file.store_string(text)
	file.close()
