extends GutTest

# The LaunchFlags binding over the engine's one launch-flag parser
# (base/resource_index/boot_policy.h): every runtime launch flag reads through
# the args override tests substitute for the process command line, and an
# absent flag yields the caller's fallback or the field's sentinel.


func after_each() -> void:
	LaunchFlags.clear_args_override()


func test_product_defaults_to_opennova_with_an_explicit_compatibility_profile() -> void:
	LaunchFlags.set_args_override(PackedStringArray([]))
	assert_eq(LaunchFlags.product(), "acidgambit")
	assert_eq(LaunchFlags.game(), "jo")
	assert_true(LaunchFlags.product_supported())
	assert_false(LaunchFlags.no_update())
	assert_false(LaunchFlags.after_update())
	LaunchFlags.set_args_override(PackedStringArray(["--after-update"]))
	assert_true(LaunchFlags.after_update())
	LaunchFlags.set_args_override(PackedStringArray(["/game", "DFX2", "--no-update"]))
	assert_eq(LaunchFlags.product(), "dfx2")
	assert_eq(LaunchFlags.game(), "dfx2")
	assert_true(LaunchFlags.no_update())
	LaunchFlags.set_args_override(PackedStringArray(["/game", "acidgambit"]))
	assert_eq(LaunchFlags.product(), "acidgambit")
	assert_eq(LaunchFlags.game(), "jo")
	LaunchFlags.set_args_override(PackedStringArray(["/game", "unknown"]))
	assert_false(LaunchFlags.product_supported())
	LaunchFlags.set_args_override(PackedStringArray(["/game", "dfx"]))
	assert_false(LaunchFlags.product_supported())
	assert_eq(LaunchFlags.game(), "dfx", "format tools can still name other engine profiles")
	LaunchFlags.set_args_override(PackedStringArray(["/game"]))
	assert_false(LaunchFlags.product_supported())


# ADR 0067 d2: without /game the launch directory's UPDATE.WIZ names the product
# by its CMD (JO:CA's, the Steam DFX2's, OpenNova content's own); a wizard naming
# another game leaves none, and /game always wins.
func test_a_launch_without_game_takes_its_folders_wizard() -> void:
	var dir := TestFs.cache_dir(self, "launch_wizard")
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", dir]))
	assert_eq(LaunchFlags.launch_directory(), dir)
	assert_eq(LaunchFlags.product(), "acidgambit", "no wizard: OpenNova's own folder")
	for row in [["JOPTR.CMD", "jo", "jo"], ["DFX2.CMD", "dfx2", "dfx2"], ["ACIDGAMBIT.CMD", "acidgambit", "jo"]]:
		TestFs.write_text(self, dir.path_join("UPDATE.WIZ"), "[HEADER]\r\nCMD=%s\r\n" % row[0])
		assert_eq(LaunchFlags.product(), row[1], row[0])
		assert_eq(LaunchFlags.game(), row[2], row[0])
		assert_true(LaunchFlags.product_supported(), row[0])
	TestFs.write_text(self, dir.path_join("UPDATE.WIZ"), "[HEADER]\r\nCMD=DFBHD.CMD\r\n")
	assert_eq(LaunchFlags.product(), "")
	assert_false(LaunchFlags.product_supported())
	assert_false(LaunchFlags.game_given())
	assert_eq(LaunchFlags.game(), "jo", "a foreign folder still resolves an engine profile")
	LaunchFlags.set_args_override(PackedStringArray(["/game", "jo", "--resource-dir", dir]))
	assert_true(LaunchFlags.game_given())
	assert_eq(LaunchFlags.product(), "jo")
	assert_true(LaunchFlags.product_supported())
	TestFs.remove_dir_recursive(dir)


# ADR 0074: a wizard naming another CMD with GAME is a player's game, played as
# that profile in its folder (named by its login there, and by /game), under its
# own name and command file.
func test_a_player_games_folder_plays_its_game() -> void:
	var dir := TestFs.cache_dir(self, "launch_player_game")
	TestFs.write_text(self, dir.path_join("UPDATE.WIZ"),
			"[HEADER]\r\nCMD=MYGAME.CMD\r\nPRODUCT_LOGIN=MYGAME\r\nGAME=dfx2\r\n")
	for args in [["--resource-dir", dir], ["/game", "MyGame", "--resource-dir", dir]]:
		LaunchFlags.set_args_override(PackedStringArray(args))
		assert_eq(LaunchFlags.product(), "mygame")
		assert_true(LaunchFlags.product_supported())
		assert_eq(LaunchFlags.game(), "dfx2")
		assert_eq(LaunchFlags.product_command_file(), "MYGAME.CMD")
		assert_eq(LaunchFlags.product_name(), "MYGAME", "no command file yet: its login")
	TestFs.write_text(self, dir.path_join("MYGAME.CMD"),
			"[HEADER]\r\nPRODUCT=My Game\r\nVERSION=1\r\n\r\n[COMMAND]\r\n")
	assert_eq(LaunchFlags.product_name(), "My Game", "installed: its release's product name")
	LaunchFlags.set_args_override(PackedStringArray(["/game", "jo", "--resource-dir", dir]))
	assert_eq(LaunchFlags.product_command_file(), "JOPTR.CMD")
	assert_eq(LaunchFlags.product_name(), "Joint Operations")
	LaunchFlags.set_args_override(PackedStringArray(["/game", "mygame"]))
	assert_false(LaunchFlags.product_supported(), "its login names no game in another folder")
	TestFs.write_text(self, dir.path_join("UPDATE.WIZ"), "[HEADER]\r\nCMD=DFBHD.CMD\r\n")
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", dir]))
	assert_eq(LaunchFlags.product_command_file(), "DFBHD.CMD",
			"a foreign folder's, what its wizard names")
	TestFs.remove_dir_recursive(dir)


func test_runtime_flags_read_through_the_override() -> void:
	LaunchFlags.set_args_override(PackedStringArray([
		"--mission", "00TRa.bms", "--lan-host", "ASH_I5A.BMS",
		"--lan-join", "192.168.10.120:32770", "--lan-port", "32768",
		"--lan-gametype", "0x10020", "--lan-mode", "3", "--lan-max-players", "16",
		"--spectator", "--spectator-password", "watch me",
		"--callsign", "Ranger",
		"--capture-pcap", "C:/cap/s.pcapng", "--mcp-port", "8975",
		"--resource-dir", "C:/Games/JO", "/exp", "jox01", "/d", "--loose-root",
	]))
	assert_true(LaunchFlags.has_args_override())
	assert_eq(LaunchFlags.mission(), "00TRa.bms")
	assert_eq(LaunchFlags.lan_host(), "ASH_I5A.BMS")
	assert_eq(LaunchFlags.lan_join_ip(), "192.168.10.120")
	assert_eq(LaunchFlags.lan_join_port(32768), 32770)
	assert_false(LaunchFlags.lan_join_browse())
	assert_eq(LaunchFlags.lan_port(1), 32768)
	assert_eq(LaunchFlags.lan_gametype(), 0x10020)
	assert_eq(LaunchFlags.lan_mode(1), 3)
	assert_eq(LaunchFlags.lan_max_players(4), 16)
	assert_true(LaunchFlags.spectator())
	assert_eq(LaunchFlags.spectator_password(), "watch me")
	assert_eq(LaunchFlags.callsign(), "Ranger")
	assert_eq(LaunchFlags.capture_pcap(), "C:/cap/s.pcapng")
	assert_eq(LaunchFlags.mcp_port(), 8975)
	assert_eq(LaunchFlags.resource_dir(), "C:/Games/JO")
	assert_true(LaunchFlags.resource_dir_given())
	assert_eq(LaunchFlags.expansion("revx02"), "jox01")
	assert_true(LaunchFlags.loose_override_enabled())
	assert_true(LaunchFlags.loose_root_allowed())


func test_absent_flags_fall_back() -> void:
	LaunchFlags.set_args_override(PackedStringArray([]))
	assert_eq(LaunchFlags.mission(), "")
	assert_eq(LaunchFlags.lan_host(), "")
	assert_eq(LaunchFlags.lan_join_ip(), "")
	assert_eq(LaunchFlags.lan_join_port(32768), 32768)
	assert_false(LaunchFlags.lan_join_browse())
	assert_eq(LaunchFlags.lan_port(32768), 32768)
	assert_eq(LaunchFlags.lan_gametype(), -1)
	assert_eq(LaunchFlags.lan_mode(1), 1)
	assert_eq(LaunchFlags.lan_max_players(4), 4)
	assert_false(LaunchFlags.spectator())
	assert_eq(LaunchFlags.spectator_password(), "")
	assert_eq(LaunchFlags.callsign(), "")
	assert_eq(LaunchFlags.capture_pcap(), "")
	assert_eq(LaunchFlags.mcp_port(), 0)
	assert_eq(LaunchFlags.resource_dir(), "")
	assert_false(LaunchFlags.resource_dir_given())
	assert_false(LaunchFlags.loose_override_enabled())


# `--lan-join browse[:<port>]` names no address: the joiner searches and joins
# the row on the port (any row without one), so no dial address reads.
func test_lan_join_browse_names_no_address() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--lan-join", "browse:32786"]))
	assert_true(LaunchFlags.lan_join_browse())
	assert_eq(LaunchFlags.lan_join_ip(), "")
	assert_eq(LaunchFlags.lan_join_port(0), 32786)
	LaunchFlags.set_args_override(PackedStringArray(["--lan-join", "BROWSE"]))
	assert_true(LaunchFlags.lan_join_browse())
	assert_eq(LaunchFlags.lan_join_ip(), "")
	assert_eq(LaunchFlags.lan_join_port(0), 0)


func test_resource_dir_without_a_value_is_given_but_empty() -> void:
	# The shell's usage error, distinct from no flag (the executable directory).
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir"]))
	assert_eq(LaunchFlags.resource_dir(), "")
	assert_true(LaunchFlags.resource_dir_given())


func test_malformed_integers_read_the_sentinel() -> void:
	LaunchFlags.set_args_override(PackedStringArray([
		"--lan-port", "fast", "--lan-mode", "5", "--lan-max-players", "65",
		"--lan-gametype", "-3", "--mcp-port", "70000",
	]))
	assert_eq(LaunchFlags.lan_port(32768), 32768)
	assert_eq(LaunchFlags.lan_mode(1), 1)
	assert_eq(LaunchFlags.lan_max_players(4), 4)
	assert_eq(LaunchFlags.lan_gametype(), -1)
	assert_eq(LaunchFlags.mcp_port(), 0)


func test_clearing_the_override_returns_to_the_process_command_line() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--mission", "00TRa.bms"]))
	assert_eq(LaunchFlags.mission(), "00TRa.bms")
	LaunchFlags.clear_args_override()
	assert_false(LaunchFlags.has_args_override())
	# The GUT process itself carries none of the runtime launch flags.
	assert_eq(LaunchFlags.mission(), "")
	assert_eq(LaunchFlags.mcp_port(), 0)


# ADR 0046 S13 A8: the directory the game was started in, where it keeps the files it
# writes beside itself as retail keeps its saves in its working directory [orig:
# PlayerProfile_LoadAllFromDisk @ 0x54f4d0]. `--working-dir`, which a source run's
# launcher passes (Godot's --path moved the process to the project), names it, '/'-
# separated; without the flag it is the process's own working directory, here the
# project GUT runs in (--path godot).
func test_working_dir_is_the_flag_else_the_process_working_directory() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", "C:\\p\\.opennova\\run\\runtime\\1"]))
	assert_eq(LaunchFlags.working_dir(), "C:/p/.opennova/run/runtime/1")
	LaunchFlags.set_args_override(PackedStringArray(["--", "--Working-Dir", " /tmp/run/2 "]))
	assert_eq(LaunchFlags.working_dir(), "/tmp/run/2")
	LaunchFlags.set_args_override(PackedStringArray([]))
	var own := String(LaunchFlags.working_dir())
	assert_false(own.is_empty())
	assert_true(DirAccess.dir_exists_absolute(own), own)
	assert_eq(own.trim_suffix("/").to_lower(),
			ProjectSettings.globalize_path("res://").trim_suffix("/").to_lower(),
			"Godot's --path makes the project the process's working directory")


# /sstga (boot_policy.h screenshot_tga): the Screen Shot action's file takes .tga and the frame
# goes through the engine's run-length TGA writer (formats/tga/tga.h tga_write_screenshot, whose
# ctest screenshot_tga pins the packets); without it the file is the first free SS%0.5d.jpg.
func test_sstga_names_the_tga_and_writes_the_run_length_file() -> void:
	var dir := ProjectSettings.globalize_path("user://").path_join("no_screen_shots_here")
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", dir]))
	assert_false(LaunchFlags.screen_shot_tga())
	assert_eq(LaunchFlags.screen_shot_path(), dir.path_join("SS00000.jpg"))
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", dir, "/SSTGA"]))
	assert_true(LaunchFlags.screen_shot_tga())
	assert_eq(LaunchFlags.screen_shot_path(), dir.path_join("SS00000.tga"))
	# A 2 x 1 frame of one colour (R 0x11, G 0x22, B 0x33; the alpha unread): the header, then
	# one run of two.
	var frame := PackedByteArray([0x11, 0x22, 0x33, 255, 0x11, 0x22, 0x33, 0])
	assert_eq(LaunchFlags.screen_shot_tga_file(frame, 2, 1), PackedByteArray([
		0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 24, 0, 0x81, 0x33, 0x22, 0x11,
	]))
	assert_eq(LaunchFlags.screen_shot_tga_file(PackedByteArray([1, 2, 3]), 1, 1).size(), 0)
