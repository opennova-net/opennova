extends GutTest

# The LaunchFlags binding over the engine's one launch-flag parser
# (base/resource_index/boot_policy.h): every runtime launch flag reads through
# the args override tests substitute for the process command line, and an
# absent flag yields the caller's fallback or the field's sentinel.


func after_each() -> void:
	LaunchFlags.clear_args_override()


func test_runtime_flags_read_through_the_override() -> void:
	LaunchFlags.set_args_override(PackedStringArray([
		"--mission", "00TRa.bms", "--lan-host", "ASH_I5A.BMS",
		"--lan-join", "192.168.10.120:32770", "--lan-port", "32768",
		"--lan-gametype", "0x10020", "--lan-mode", "3", "--lan-max-players", "16",
		"--spectator", "--spectator-password", "watch me",
		"--callsign", "Ranger", "--integrity-profile", "retail",
		"--capture-pcap", "C:/cap/s.pcapng", "--mcp-port", "8975",
		"--resource-dir", "C:/Games/JO", "/exp", "jox01", "/d", "--loose-root",
	]))
	assert_true(LaunchFlags.has_args_override())
	assert_eq(LaunchFlags.mission(), "00TRa.bms")
	assert_eq(LaunchFlags.lan_host(), "ASH_I5A.BMS")
	assert_eq(LaunchFlags.lan_join_ip(), "192.168.10.120")
	assert_eq(LaunchFlags.lan_join_port(32768), 32770)
	assert_eq(LaunchFlags.lan_port(1), 32768)
	assert_eq(LaunchFlags.lan_gametype(), 0x10020)
	assert_eq(LaunchFlags.lan_mode(1), 3)
	assert_eq(LaunchFlags.lan_max_players(4), 16)
	assert_true(LaunchFlags.spectator())
	assert_eq(LaunchFlags.spectator_password(), "watch me")
	assert_eq(LaunchFlags.callsign(), "Ranger")
	assert_eq(LaunchFlags.integrity_profile(), "retail")
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
	assert_eq(LaunchFlags.lan_port(32768), 32768)
	assert_eq(LaunchFlags.lan_gametype(), -1)
	assert_eq(LaunchFlags.lan_mode(1), 1)
	assert_eq(LaunchFlags.lan_max_players(4), 4)
	assert_false(LaunchFlags.spectator())
	assert_eq(LaunchFlags.spectator_password(), "")
	assert_eq(LaunchFlags.callsign(), "")
	assert_eq(LaunchFlags.integrity_profile(), "")
	assert_eq(LaunchFlags.capture_pcap(), "")
	assert_eq(LaunchFlags.mcp_port(), 0)
	assert_eq(LaunchFlags.resource_dir(), "")
	assert_false(LaunchFlags.resource_dir_given())
	assert_false(LaunchFlags.loose_override_enabled())


func test_resource_dir_without_a_value_is_given_but_empty() -> void:
	# The shell's usage error, distinct from no flag (the bundled assets/ boot).
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
