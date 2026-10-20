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


func test_the_lan_switch_forces_the_lan_only_word() -> void:
	# `/LAN`, any case: every read of the block forces mpnovaworldhostlanonly
	# to 1 (docs/gamecfg/game-cfg-re.md, D-GAMECFG-5).
	_write_cfg("version = 29\r\nmpnovaworldhostlanonly = 0\r\n")
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir, "/lan"]))
	GameCfgStore.reset()
	assert_eq(GameCfgStore.block().get_word("mpnovaworldhostlanonly"), 1, "the start's read")
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()
	assert_eq(GameCfgStore.block().get_word("mpnovaworldhostlanonly"), 0, "no switch, the file's")


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


func test_a_host_request_takes_the_block_attribute_word() -> void:
	# A host request starts from the block's mpattrib: the host screen's
	# TEAM_CHOOSE switch shows the file's bit, and a launch-flag host, with no
	# screen, sends the file's word in its S2C 0x64 (D-NET-NEW-LP-4).
	_write_cfg("version = 29\r\nmpattrib = 14854\r\n")
	GameCfgStore.reset()
	var config := HostSessionConfig.new()
	assert_eq(config.mp_attributes, 14854, "the request carries the file's word, 0x3A06")
	assert_eq(GameCfgStore.block().host_dialog_values()["TEAM_CHOOSE"], "1",
			"the screen shows TEAM_CHOOSE on")
	_write_cfg("version = 29\r\nmpattrib = 14850\r\n")
	GameCfgStore.reset()
	assert_eq(HostSessionConfig.new().mp_attributes, 14850, "and the stock word when the file says so")


func test_the_host_screen_shows_the_block_and_writes_it_back() -> void:
	# The host screen opens on the block's words (its cap clamped to the
	# screen's 64 in the block too), its START reads the controls back into the
	# block, and the request, a launch-flag host's included, is the block's
	# (D-NET-335) [orig: UI_PopulateHostSettingsFromConfig @0x555FE0;
	# HostDialog_ReadSettings @0x556E36, Game_SaveConfig @0x556E72].
	_write_cfg("version = 29\r\ngame_name = \"Night Ops\"\r\nmpmaxplayers = 80\r\ntime_limit = 25\r\n")
	GameCfgStore.reset()
	var block := GameCfgStore.block()
	var shown: Dictionary = block.host_dialog_values()
	assert_eq(shown["GAME_NAME"], "Night Ops", "the screen shows the file's name")
	assert_eq(shown["TIME"], "25", "and its time limit")
	assert_eq(shown["MAX_PLAYERS"], "64", "and its cap at the screen's 64")
	assert_eq(block.get_word("mpmaxplayers"), 64, "the populate writes the clamp into the block")
	assert_eq(HostSessionConfig.new().server_name, "Night Ops", "a launch-flag host hosts under it")
	assert_true(block.apply_host_dialog_control("GAME_NAME", "Renamed"))
	assert_true(block.apply_host_dialog_control("TEAM_CHOOSE", "1"))
	assert_eq(HostSessionConfig.new().server_name, "Renamed", "START's read lands in the block")
	assert_eq(HostSessionConfig.new().mp_attributes & 4, 4, "the switch onto its mpattrib bit")
	assert_eq(GameCfgStore.save(), OK)
	assert_string_contains(_saved_cfg(), "Renamed", "and the save writes it")


func test_a_single_player_start_leaves_team_choose_in_the_block() -> void:
	# Single player's mission start stores 0x3A06 into the block after its save,
	# so a later host of the process advertises TeamChoose (D-NET-NEW-LP-4)
	# [orig: SinglePlayer_StartMission @0x561BB2 / @0x561BB7].
	_write_cfg("version = 29\r\nmpattrib = 14850\r\n")
	GameCfgStore.reset()
	GameCfgStore.block().single_player_mission_start()
	assert_eq(GameCfgStore.block().get_word("mpattrib"), 14854, "the block's word is 0x3A06")
	assert_eq(HostSessionConfig.new().mp_attributes, 14854, "and a later host's request carries it")


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


func test_the_terrain_texture_detail_reaches_the_world_as_read() -> void:
	# The shell hands the world game.cfg's terrain_texdetail as the block read
	# it, clamped into 0..3 (a -1 is 0), through its options listener; the world
	# copies it at its next mission start (engine renderer/device_texture.h).
	_write_cfg("version = 29\r\nterrain_texdetail = -1\r\n")
	var shell := await _boot_shell()
	if shell == null:
		return
	assert_eq(GameCfgStore.block().get_word("terrain_texdetail"), 0, "the read clamps -1 to 0")
	assert_eq(shell.get_world().get_terrain_texdetail(), 0, "the shell hands the world the word")


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
