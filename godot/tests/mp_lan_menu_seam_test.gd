extends GutTest

# The mp.mnu LAN co-op menu seam: the companion (mp_menu_companion.gd) drives the JO
# multiplayer menu by control NAME — the same Command-by-name convention the shell's
# start/exit controls use, but for hosting/joining a LAN game. This pins the wiring
# (which controls do what, and the host config START_GAME reports). The live two-machine
# flow (real discovery + a second client spawning) is the manual smoke; this is the unit.

const MenuShell := preload("res://game/nova_menu_shell.gd")
const MissionRuntime := preload("res://game/world/mission_runtime.gd")


# A REAL LanSession (typed seam) whose discovery feed is driven by hand:
# publish() emits the native servers_changed signal without touching sockets.
class _LanSessionFeed extends LanSession:
	func publish(servers: Array) -> void:
		emit_signal("servers_changed", servers)




# A stand-in for the built mp.mnu host-settings screen: a plain Node (the companion only
# needs find_child + the optional widget_value_changed signal) with the named NovaMnu*
# controls as children, exactly as the menu builder would name them from the .mnu.
func _make_host_menu() -> MnuMenu:
	var menu := MnuMenu.new()
	menu.build_on_ready = false
	menu.name = "Menu"
	add_child_autofree(menu)
	for n in ["GAME_NAME", "MAX_PLAYERS"]:
		var e := MnuEdit.new()
		e.name = n
		menu.add_child(e)
	var mission_list := MnuList.new()
	mission_list.name = "MISSION_LIST"
	menu.add_child(mission_list)
	var selected := MnuTable.new()
	selected.name = "SELECTED_MISSIONS"
	selected.add_column(80, 0, false)  # Mission
	selected.add_column(60, 0, false)  # Type
	selected.add_column(40, 0, false)  # Switch
	menu.add_child(selected)
	for n in ["ADD_MISSIONS", "REMOVE_MISSIONS", "START_GAME"]:
		var b := Button.new()
		b.name = n
		menu.add_child(b)
	return menu


func _make_lan_menu() -> MnuMenu:
	var menu := MnuMenu.new()
	menu.build_on_ready = false
	menu.name = "Menu"
	add_child_autofree(menu)
	var server_list := MnuList.new()
	server_list.name = "LAN_GAME_LIST"
	menu.add_child(server_list)
	for control_name in ["LAN_SEARCH", "LAN_JOINGAME"]:
		var button := Button.new()
		button.name = control_name
		menu.add_child(button)
	return menu


func _press(menu: Node, name: String) -> void:
	menu.find_child(name, true, false).emit_signal("pressed")


func _start_host_config(mission_file: String) -> HostSessionConfig:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	var mission_list := menu.find_child("MISSION_LIST", true, false) as MnuList
	mission_list.set_items(PackedStringArray([mission_file]))
	mission_list.select(0)
	_press(menu, "ADD_MISSIONS")
	_press(menu, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	return get_signal_parameters(mp, "lan_host_start_requested")[0] as HostSessionConfig


func test_owned_screens_default() -> void:
	assert_true(MpMenuCompanion.OWNED_SCREENS.has("LAN_MULTI_PLAYER"))
	assert_true(MpMenuCompanion.OWNED_SCREENS.has("MULTI_PLAYER_HOST"))


func test_owns_menu_detects_mp_menu() -> void:
	var mp := MpMenuCompanion.new()
	var menu := _make_host_menu()
	assert_true(mp.owns_menu(menu), "a menu carrying SELECTED_MISSIONS is the JO mp menu")
	var plain := MnuMenu.new()
	plain.build_on_ready = false
	add_child_autofree(plain)
	assert_false(mp.owns_menu(plain), "a plain menu is left to the shell")


func test_add_and_remove_missions() -> void:
	var mp := MpMenuCompanion.new()
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	var mission_list := menu.find_child("MISSION_LIST", true, false) as MnuList
	mission_list.set_items(PackedStringArray(["alpha.bms", "bravo.bms"]))
	mission_list.select(0)
	_press(menu, "ADD_MISSIONS")
	var table := menu.find_child("SELECTED_MISSIONS", true, false) as MnuTable
	assert_eq(table.get_row_count(), 1, "ADD moved the highlighted mission into the rotation")
	assert_eq(table.get_cell_text(0, 0), "alpha.bms")
	_press(menu, "ADD_MISSIONS")
	assert_eq(table.get_row_count(), 1, "ADD de-dupes the same mission")
	table.select_row(0)
	_press(menu, "REMOVE_MISSIONS")
	assert_eq(table.get_row_count(), 0, "REMOVE dropped the selected row")


func test_start_game_emits_host_config() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	(menu.find_child("GAME_NAME", true, false) as LineEdit).text = "CoopNight"
	(menu.find_child("MAX_PLAYERS", true, false) as LineEdit).text = "6"
	var mission_list := menu.find_child("MISSION_LIST", true, false) as MnuList
	mission_list.set_items(PackedStringArray(["alpha.bms"]))
	mission_list.select(0)
	_press(menu, "ADD_MISSIONS")
	_press(menu, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.server_name, "CoopNight")
	assert_eq(config.max_players, 6)
	assert_eq(config.game_type, HostSessionConfig.GAME_TYPE_COOP,
		"the session uses the witnessed retail Co-op g_GameType, not an AS capture value")
	assert_eq(config.game_type_attr, "", "no GAME_TYPE spin in the stand-in menu")
	assert_eq(config.expansion, "",
		"a base/unmounted root advertises no expansion instead of captured jox01")
	assert_eq(config.mission, "alpha.bms")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_LAN)
	var session_options := config.to_session_options()
	assert_eq(String(session_options.get("channel", "")), HostSessionConfig.CHANNEL_LAN,
		"the FFI options retain the LAN cadence selector")
	assert_eq(int(session_options.get("lan_mode", 0)), 1,
		"a stock LAN request carries retail g_LanMode 1")
	# SERVERTYPE absent in the stand-in menu -> serve-and-play (dedicated=false). The real screen's
	# SERVERTYPE spinlist (HG_SERVEONLY value=1) flips this; the value-attr read is unit-tested in
	# mnu_widgets_test (test_spinlist_get_value_attr_returns_value_not_label).
	assert_eq(config.dedicated, false, "no SERVERTYPE control -> serve-and-play default")


func test_start_game_defaults() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	_press(menu, "START_GAME")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.server_name, "COOPGAME", "blank name -> default")
	assert_eq(config.max_players, 4, "blank cap -> default 4")
	assert_eq(config.mission, "", "no missions selected -> empty")
	assert_eq(config.bind_port, HostSessionConfig.DEFAULT_LAN_PORT,
		"the witnessed retail LAN host port rides the record default")


func test_hosted_mission_game_type_reaches_native_session_config() -> void:
	# The host screen selects a mission; retail derives g_GameType from that
	# mission before the native session serializes S2C 0x08/0x7B. Exercise a
	# non-Coop value and the valid numeric-zero Deathmatch value so a fixed Coop
	# default or a zero-as-unset sentinel cannot satisfy this seam.
	var cases := [
		{
			"mission": "team_deathmatch.bms",
			"mode": MissionData.ATTRIB_TEAM_DEATHMATCH,
			"expected": 0x10000,
		},
		{
			"mission": "deathmatch.bms",
			"mode": MissionData.ATTRIB_DEATHMATCH,
			"expected": 0,
		},
	]
	for row in cases:
		var config := _start_host_config(String(row["mission"]))
		config.bind_port = 0 # OS-selected port keeps this focused test isolated.
		var mission := MissionData.new()
		assert_eq(mission.create_default(), OK)
		assert_true(mission.set_game_mode(int(row["mode"])))
		var sim := Simulation.new()
		var runtime := MissionRuntime.new()
		add_child_autofree(runtime)
		var container := Node3D.new()
		add_child_autofree(container)
		assert_gt(int(runtime.setup(mission, container, {
			"simulation": sim,
			"host_session": config,
			"mission_file": String(row["mission"]),
		})), 0)
		assert_eq(int(sim.get_host_session_config().get("gametype", -1)),
				int(row["expected"]),
				"%s reaches the native wire configuration" % String(row["mission"]))


func test_env_lan_mode_override_is_explicit_and_validated() -> void:
	assert_eq(NetSessionController.resolve_lan_mode_override("4", 1), 4,
		"the parity harness can match a retail game.cfg lanmode")
	assert_eq(NetSessionController.resolve_lan_mode_override(" 2 ", 1), 2,
		"surrounding whitespace follows the other NW_LAN_* overrides")
	for invalid in ["", "fast", "0", "5", "-1"]:
		assert_eq(NetSessionController.resolve_lan_mode_override(invalid, 1), 1,
			"invalid override '%s' preserves the configured default" % invalid)


func test_env_lan_max_players_override_is_explicit_and_validated() -> void:
	assert_eq(NetSessionController.resolve_lan_max_players_override("4", 32), 4,
		"the parity harness can match the retail hook's configured capacity")
	assert_eq(NetSessionController.resolve_lan_max_players_override(" 64 ", 32), 64,
		"the hook's witnessed listen-host upper bound remains valid")
	for invalid in ["", "many", "0", "65", "-1"]:
		assert_eq(NetSessionController.resolve_lan_max_players_override(invalid, 32), 32,
			"invalid override '%s' preserves the configured default" % invalid)


func test_auto_game_type_matches_retail_mission_mode_table() -> void:
	var modes := {
		0: 0x10020,
		MissionData.ATTRIB_DEATHMATCH: 0x00000,
		MissionData.ATTRIB_TEAM_DEATHMATCH: 0x10000,
		MissionData.ATTRIB_COOP: 0x30020,
		MissionData.ATTRIB_KING_OF_THE_HILL: 0x00001,
		MissionData.ATTRIB_TEAM_KING_OF_THE_HILL: 0x10001,
		MissionData.ATTRIB_SEARCH_AND_DESTROY: 0x90002,
		MissionData.ATTRIB_ATTACK_AND_DEFEND: 0x10002,
		MissionData.ATTRIB_CAPTURE_THE_FLAG: 0x10004,
		MissionData.ATTRIB_FLAGBALL: 0x10008,
		MissionData.ATTRIB_ADVANCE_AND_SECURE: 0x10010,
		MissionData.ATTRIB_CONQUER_AND_CONTROL: 0x50010,
	}
	for mode in modes:
		assert_eq(HostSessionConfig.game_type_for_mission_mode(int(mode)), int(modes[mode]),
			"mission mode 0x%08x uses retail's session game type" % int(mode))


func test_host_session_carries_retail_rule_defaults() -> void:
	var config := HostSessionConfig.new()
	assert_false(config.game_type_auto,
		"explicit callers stay pinned until a producer opts into mission derivation")
	var options := config.to_session_options()
	assert_eq(int(options.get("class_allow_mask", -1)), 0x03FF,
			"ordinary host requests carry retail's all-ten-classes default")
	assert_eq(int(options.get("respawn_time", -1)), 30)
	assert_eq(int(options.get("time_limit_minutes", -1)), 10)
	assert_eq(int(options.get("replay_enabled", -1)), 1)
	assert_eq(int(options.get("max_team_lives", -1)), 100)
	assert_eq(int(options.get("score_limit", -1)), 50)
	assert_eq(int(options.get("respawn_timeout", -1)), 5)
	assert_eq(int(options.get("start_delay", -1)), 0)
	assert_eq(int(options.get("destroy_buildings", -1)), 0)
	assert_eq(int(options.get("death_messages", -1)), 1)
	assert_eq(String(options.get("integrity_profile", "missing")), "",
			"ordinary hosts do not infer an integrity corpus from expansion")
	config.integrity_profile = "retail-revx02-024f56f2-2d087374"
	assert_eq(String(config.to_session_options().get("integrity_profile", "")),
			config.integrity_profile,
			"parity automation can bind both host and client to one witnessed corpus")
	config.class_allow_mask = 1 << 6
	assert_eq(int(config.to_session_options().get("class_allow_mask", -1)), 1 << 6,
			"the typed host request forwards a map-specific Soldier Class policy")


func test_lan_join_emits_selected_server() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	mp._servers = [{"name": "biggy", "host_ip": "192.168.1.10", "port": 32768}]
	# A single-click selection relays the row index through the menu's aggregate signal.
	mp._on_widget_value_changed("LAN_GAME_LIST", "list", 0, "biggy (1/4)")
	mp._on_lan_join()
	assert_signal_emitted(mp, "lan_join_requested")
	var target: JoinTarget = get_signal_parameters(mp, "lan_join_requested")[0]
	assert_eq(target.host_ip, "192.168.1.10")
	assert_eq(target.port, 32768)
	assert_eq(target.server_name, "biggy", "the browse-row name rides as a display hint")
	assert_eq(target.mission, "", "map identity is absent pre-auth; 0x7B supplies it")


func test_refreshed_lan_rows_require_a_fresh_selection() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var menu := _make_lan_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var session := _LanSessionFeed.new()
	autofree(session)
	mp.set_lan_session(session)
	session.publish([{"name": "old", "host_ip": "192.168.1.10", "port": 32768}])
	menu.emit_signal("widget_value_changed", "LAN_GAME_LIST", "list", 0, "old")

	session.publish([{"name": "replacement", "host_ip": "192.168.1.11", "port": 32769}])
	var server_list := menu.find_child("LAN_GAME_LIST", true, false) as MnuList
	assert_eq(server_list.get_item_count(), 1,
		"a servers_changed payload replaces the prior full snapshot instead of appending")
	assert_eq(server_list.get_item_text(0), "replacement (0/0)")
	_press(menu, "LAN_JOINGAME")
	assert_signal_not_emitted(mp, "lan_join_requested",
		"refreshed rows invalidate the selection from the previous result set")


func test_swapping_lan_sessions_disconnects_the_previous_discovery_source() -> void:
	var mp := MpMenuCompanion.new()
	var menu := _make_lan_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var previous := _LanSessionFeed.new()
	autofree(previous)
	var current := _LanSessionFeed.new()
	autofree(current)
	mp.set_lan_session(previous)
	mp.set_lan_session(current)
	current.publish([{"name": "current", "players": 1, "max_players": 4, "mission": "new.bms"}])
	previous.publish([{"name": "stale", "players": 4, "max_players": 4, "mission": "old.bms"}])

	var server_list := menu.find_child("LAN_GAME_LIST", true, false) as MnuList
	assert_eq(server_list.get_item_count(), 1)
	assert_eq(server_list.get_item_text(0), "current (1/4)",
		"the current source wins and pre-auth rows do not invent a mission label")


func test_shell_accepts_companion() -> void:
	var host = MenuShell.new()
	add_child_autofree(host)
	host.add_companion(MpMenuCompanion.new())  # installs without a menu loaded, no crash
	assert_true(true, "companion installed on a menuless shell without error")
