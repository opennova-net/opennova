extends GutTest

# The mp.mnu LAN co-op menu seam: the companion (mp_menu_companion.gd) drives the JO
# multiplayer menu by control NAME — the same Command-by-name convention the shell's
# start/exit controls use, but for hosting/joining a LAN game — riding a MenuDriver
# over a real MnuDocument (the compiled-menu surface; the Control tree is gone).
# This pins the wiring (which controls do what, and the host config START_GAME
# reports). The live two-machine flow (real discovery + a second client spawning)
# is the manual smoke; this is the unit.



# A REAL LanSession (typed seam) whose discovery feed is driven by hand: the
# test emits the native servers_changed signal itself, no sockets touched.
static func _publish(session: LanSession, servers: Array) -> void:
	session.emit_signal("servers_changed", servers)


# One browse row with the counts the LAN list formats.
static func _lan_row(server_name: String, players: int, max_players: int) -> LanServerRow:
	var row := LanServerRow.make(server_name, "192.168.1.10", 32768)
	row.players = players
	row.max_players = max_players
	return row


# --- Driver harness (the compiled-menu seam) ----------------------------------

# A synthetic mp.mnu host-settings screen: the named controls as authored .mnu
# markup (edits + mission list + the 3-column rotation table + buttons). The
# with_spins variant adds the GAME_TYPE/SERVERTYPE spin lists with authored
# `value=` items (the semantic attr distinct from the display label).
func _host_screen_xml(with_spins := false) -> String:
	var body := MenuDriverFixture.wnd("edit", "GAME_NAME", 10)
	body += MenuDriverFixture.wnd("edit", "MAX_PLAYERS", 34)
	body += MenuDriverFixture.wnd("checkbox", "ALLOW_SPECTATORS", 330)
	body += MenuDriverFixture.wnd("edit", "SPECTATOR_PW", 354)
	body += MenuDriverFixture.wnd("list", "MISSION_LIST", 58)
	body += ('<WINDOW type="table" name="SELECTED_MISSIONS">'
			+ '<POSITION><LEFT>300</LEFT><TOP>58</TOP><RIGHT>520</RIGHT><BOTTOM>200</BOTTOM></POSITION>'
			+ '<COLUMN count="3">'
			+ '<HEADER column="0" width="80" justify="LEFT">MISSION</HEADER>'
			+ '<HEADER column="1" width="60" justify="LEFT">TYPE</HEADER>'
			+ '<HEADER column="2" width="40" justify="LEFT">SWITCH</HEADER>'
			+ '</COLUMN></WINDOW>')
	body += MenuDriverFixture.wnd("button", "ADD_MISSIONS", 210)
	body += MenuDriverFixture.wnd("button", "REMOVE_MISSIONS", 234)
	body += MenuDriverFixture.wnd("button", "START_GAME", 258)
	if with_spins:
		# Synthetic value attrs: the test pins pass-through of the authored
		# value=, not any specific retail number.
		body += MenuDriverFixture.wnd("spinlist", "GAME_TYPE", 282,
				'<ITEMS><ITEM value="3">HG_COOPERATIVE</ITEM><ITEM value="0">HG_DEATHMATCH</ITEM></ITEMS>')
		body += MenuDriverFixture.wnd("spinlist", "SERVERTYPE", 306,
				'<ITEMS><ITEM value="0">HG_SERVEPLAY</ITEM><ITEM value="1">HG_SERVEONLY</ITEM></ITEMS>')
	return MenuDriverFixture.screen_xml("MULTI_PLAYER_HOST", body)


func _lan_screen_xml() -> String:
	var body := MenuDriverFixture.wnd("list", "LAN_GAME_LIST", 10)
	body += MenuDriverFixture.wnd("button", "LAN_SEARCH", 200)
	body += MenuDriverFixture.wnd("button", "LAN_JOINGAME", 224)
	return MenuDriverFixture.screen_xml("LAN_MULTI_PLAYER", body)


func _make_host_driver(with_spins := false) -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _host_screen_xml(with_spins)), "jo_mp.mnu")


func _make_lan_driver() -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _lan_screen_xml()), "jo_mp.mnu")


# Simulate a control press: the driver emits widget_activated(id, NAME) on the
# click/hotkey edge and companions route by NAME — the seam contract.
func _press(driver: MenuDriver, name: String) -> void:
	driver.widget_activated.emit(driver.widget_id(name), name)


# A synthetic host-listable catalog row (objective co-op unless overridden —
# stock co-op is host-invisible by the witnessed populate skip).
func _pool_row(file: String, game_type := 0x30020) -> MissionCatalogRow:
	return MissionCatalogRow.create(file, "", "", game_type, false)


func _start_host_config(mission_file: String) -> HostSessionConfig:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	mp.seed_host_pool([_pool_row(mission_file)])
	var mission_list := driver.widget_id("MISSION_LIST")
	driver.select_row(mission_list, 0)
	_press(driver, "ADD_MISSIONS")
	_press(driver, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	return get_signal_parameters(mp, "lan_host_start_requested")[0] as HostSessionConfig


func test_owns_menu_detects_mp_menu() -> void:
	var mp := MpMenuCompanion.new()
	assert_true(mp.owns_menu(_make_host_driver()),
			"a menu carrying SELECTED_MISSIONS is the JO mp menu")
	var plain := MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, MenuDriverFixture.screen_xml("PLAIN",
			MenuDriverFixture.wnd("button", "OK", 10))), "plain.mnu")
	assert_false(mp.owns_menu(plain), "a plain menu is left to the shell")


func test_add_and_remove_missions() -> void:
	var mp := MpMenuCompanion.new()
	var driver := _make_host_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	mp.seed_host_pool([_pool_row("alpha.bms"), _pool_row("bravo.bms")])
	var mission_list := driver.widget_id("MISSION_LIST")
	assert_eq(driver.item_count(mission_list), 2, "the pool seeds the list")
	driver.select_row(mission_list, 0)
	_press(driver, "ADD_MISSIONS")
	var table := driver.widget_id("SELECTED_MISSIONS")
	assert_eq(driver.table_row_count(table), 1, "ADD moved the highlighted mission into the rotation")
	assert_eq(driver.table_cell_text(table, 0, 0), "alpha.bms")
	assert_eq(driver.table_cell_text(table, 0, 1), "COOP",
			"the Type cell carries the GateTypeAbbrev key (no gametext here)")
	assert_eq(driver.table_cell_text(table, 0, 2), "0",
			"objective co-op defaults its rotation Switch off")
	# The added mission leaves the available list [orig: the +4412 hide]; the
	# reseed also clears the selection, so a second ADD picks nothing.
	assert_eq(driver.item_count(mission_list), 1, "the added mission left the list")
	_press(driver, "ADD_MISSIONS")
	assert_eq(driver.table_row_count(table), 1, "ADD de-dupes the same mission")
	driver.table_select_row(table, 0)
	_press(driver, "REMOVE_MISSIONS")
	assert_eq(driver.table_row_count(table), 0, "REMOVE dropped the selected row")
	assert_eq(driver.item_count(mission_list), 2,
			"REMOVE restored the mission to the available list")


func test_start_game_emits_host_config() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	driver.set_widget_text(driver.widget_id("GAME_NAME"), "CoopNight")
	driver.set_widget_text(driver.widget_id("MAX_PLAYERS"), "6")
	driver.set_widget_checked(driver.widget_id("ALLOW_SPECTATORS"), true)
	driver.set_widget_text(driver.widget_id("SPECTATOR_PW"), "watch")
	mp.seed_host_pool([_pool_row("alpha.bms")])
	var mission_list := driver.widget_id("MISSION_LIST")
	driver.select_row(mission_list, 0)
	_press(driver, "ADD_MISSIONS")
	_press(driver, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.server_name, "CoopNight")
	assert_eq(config.max_players, 6)
	assert_eq(config.spectator_slots, -1,
			"checked ALLOW_SPECTATORS retains retail's shared-capacity sentinel")
	assert_eq(config.spectator_password, "watch")
	assert_true(config.game_type_auto,
		"the selected mission's mode owns g_GameType, not a recorded default")
	assert_eq(config.game_type, 0,
		"the record keeps the host block's zeroed mp_gametype (Config_SetDefaults)")
	assert_eq(config.game_type_attr, "", "no GAME_TYPE spin in the stand-in menu")
	assert_eq(config.expansion, "",
		"a base/unmounted root advertises no expansion instead of captured jox01")
	assert_eq(config.mission, "alpha.bms")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_LAN)
	var session_options := config.to_session_options()
	assert_eq(session_options.channel, HostSessionConfig.CHANNEL_LAN,
		"the session request retains the LAN cadence selector")
	assert_eq(session_options.lan_mode, 1,
		"a stock LAN request carries retail g_LanMode 1")
	assert_eq(session_options.spectator_slots, -1)
	assert_eq(session_options.spectator_password, "watch")
	# SERVERTYPE absent in this stand-in menu -> serve-and-play (dedicated=false). The real
	# screen's SERVERTYPE spinlist (HG_SERVEONLY value=1) flips this; the value-attr read is
	# pinned in test_servertype_value_attr_selects_dedicated_not_the_label below.
	assert_true(config.serve_and_play, "no SERVERTYPE control -> serve-and-play default")


func test_start_game_defaults() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver()
	# The retail config defaults copy the Menu/UNTITLED gametext into the
	# session name before the host screen is populated from the config.
	var previous_gametext: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	var gametext := RtxtStringFile.new()
	gametext.add_entry("UNTITLED", "!Untitled", gametext.add_section("Menu"), Vector2i.ZERO)
	Strings.register_table(Strings.TABLE_GAMETEXT, gametext)
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	Strings.register_table(Strings.TABLE_GAMETEXT, previous_gametext)
	assert_eq(driver.get_widget_text(driver.widget_id("GAME_NAME")), "!Untitled",
			"the populate seeds GAME_NAME from the Menu/UNTITLED gametext")
	assert_eq(driver.get_widget_text(driver.widget_id("MAX_PLAYERS")),
			str(HostSessionOptions.new().max_players),
			"the populate seeds MAX_PLAYERS from the engine's host-screen cap")
	assert_eq(HostSessionOptions.new().max_players, 64,
			"the cfg table's mpmaxplayers default")
	assert_false(driver.is_widget_checked(driver.widget_id("ALLOW_SPECTATORS")),
			"the populate seeds ALLOW_SPECTATORS off (spectator_slots 0)")
	# An empty rotation never starts [orig: the START_GAME interactive gate on
	# the selected table @0x557f09 / the init disable @0x5589f2].
	_press(driver, "START_GAME")
	assert_signal_not_emitted(mp, "lan_host_start_requested",
			"an empty rotation gates START_GAME off")
	assert_true(driver.is_widget_disabled(driver.widget_id("START_GAME")),
			"the button is non-interactive while the rotation is empty")
	mp.seed_host_pool([_pool_row("alpha.bms")])
	driver.select_row(driver.widget_id("MISSION_LIST"), 0)
	_press(driver, "ADD_MISSIONS")
	assert_false(driver.is_widget_disabled(driver.widget_id("START_GAME")),
			"a filled rotation arms START_GAME")
	_press(driver, "START_GAME")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.server_name, "!Untitled", "an untouched host screen hosts under the seeded name")
	assert_eq(config.max_players, 64,
			"an untouched host screen hosts with the seeded cap")
	assert_eq(config.spectator_slots, 0,
			"unchecked ALLOW_SPECTATORS disables spectator admission")
	assert_eq(config.spectator_password, "")
	assert_eq(config.mission, "alpha.bms", "the rotation head is the mission")
	assert_eq(config.bind_port, HostSessionConfig.DEFAULT_LAN_PORT,
		"the witnessed retail LAN host port rides the record default")
	# A user-cleared edit is still read verbatim: no shell fallback substitutes.
	driver.set_widget_text(driver.widget_id("GAME_NAME"), "")
	driver.set_widget_text(driver.widget_id("MAX_PLAYERS"), "")
	_press(driver, "START_GAME")
	var cleared: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(cleared.server_name, "", "a cleared name field is read verbatim")
	assert_eq(cleared.max_players, 0, "a cleared cap edit reads zero before the live slot rule")


# The SERVERTYPE/GAME_TYPE spin semantics ride the authored item `value=` attr, NOT
# the localized display label [orig: the host dialog server-type read,
# UI_HandleHostSessionStart @0x556d00 — HG_SERVEPLAY value=0 (serve-and-play) vs
# HG_SERVEONLY value=1 (dedicated)]. This replaces the deleted mnu_widgets_test.gd
# coverage (test_spinlist_get_value_attr_returns_value_not_label) after the
# Control-tree cutover: driver.spin_value_attr is the compiled surface's read of
# the same authored item value.
func test_servertype_value_attr_selects_dedicated_not_the_label() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver(true)
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	var servertype := driver.widget_id("SERVERTYPE")
	assert_eq(driver.spin_value_attr(servertype), "0",
		"the authored HG_SERVEPLAY row reads its value attr 0")
	assert_ne(driver.spin_value_attr(servertype), driver.item_text(servertype, 0),
		"the value attr is the semantic value, not the display text")
	driver.select_row(servertype, 1)  # emits "spinlist" — the user flip
	assert_eq(driver.spin_value_attr(servertype), "1",
		"HG_SERVEONLY reads its authored value attr 1")
	# The rotation must hold a mission before START arms; the pool row's
	# category (TKOTH -> 3) matches this menu's selected GAME_TYPE value "3".
	mp.seed_host_pool([_pool_row("alpha.bms", 0x10001)])
	driver.select_row(driver.widget_id("MISSION_LIST"), 0)
	_press(driver, "ADD_MISSIONS")
	_press(driver, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_false(config.serve_and_play, "SERVERTYPE value 1 hosts dedicated (no local player)")
	assert_eq(config.game_type_attr, "3",
		"the GAME_TYPE spin relays its selected row's authored value attr")


# The witnessed host pool rules (D-MNU-17): stock co-op (the pure-SP family)
# never lists [orig: the populate skip @0x558a70]; the GAME_TYPE spin filters
# by the mapped category with an absent spin reading ALL
# [orig: HostDialog_FilterMissionListByGameType @0x556fe0].
func test_host_pool_filter_and_sp_exclusion() -> void:
	var mp := MpMenuCompanion.new()
	var driver := _make_host_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	mp.seed_host_pool([
		_pool_row("training.bms", 0x10020),
		_pool_row("coop.bms", 0x30020),
		_pool_row("tkoth.bms", 0x10001),
	])
	var mission_list := driver.widget_id("MISSION_LIST")
	assert_eq(driver.item_count(mission_list), 2,
			"stock co-op is host-invisible; no GAME_TYPE spin reads ALL")

	var spun := MpMenuCompanion.new()
	var spin_driver := _make_host_driver(true)
	spun.on_menu_built(spin_driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	spun.seed_host_pool([
		_pool_row("coop.bms", 0x30020),
		_pool_row("tkoth.bms", 0x10001),
	])
	var spun_list := spin_driver.widget_id("MISSION_LIST")
	# This menu's GAME_TYPE row 0 authors value "3" — the TKOTH category.
	assert_eq(spin_driver.item_count(spun_list), 1,
			"the selected category filters the pool")
	assert_eq(spin_driver.item_text(spun_list, 0), "tkoth.bms")


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
		var runtime := MissionRoot.new()
		add_child_autofree(runtime)
		var container := Node3D.new()
		add_child_autofree(container)
		var options := MissionSetupOptions.new()
		options.simulation = sim
		options.host_session = config.to_session_options()
		options.mission_file = String(row["mission"])
		assert_gt(int(runtime.setup(mission, container, options)), 0)
		assert_eq(sim.get_host_session_config().game_type,
				int(row["expected"]),
				"%s reaches the native wire configuration" % String(row["mission"]))


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
		assert_eq(NetProtocol.game_type_for_mission_mode(int(mode)), int(modes[mode]),
			"mission mode 0x%08x uses retail's session game type" % int(mode))


func test_host_session_carries_retail_rule_defaults() -> void:
	var config := HostSessionConfig.new()
	assert_false(config.game_type_auto,
		"explicit callers stay pinned until a producer opts into mission derivation")
	var options := config.to_session_options()
	assert_eq(options.class_allow_mask, 0x03FF,
			"ordinary host requests carry retail's all-ten-classes default")
	assert_eq(options.respawn_time, 30)
	assert_eq(options.time_limit_minutes, 10)
	assert_eq(options.replay_enabled, 1)
	assert_eq(options.max_team_lives, 100)
	assert_eq(options.score_limit, 50)
	assert_eq(options.max_score, 5,
			"FlagBall never boots into retail's immediate team-1 zero-limit outcome")
	assert_eq(options.koth_delta, 5)
	assert_eq(options.flag_return_ticks, 210)
	assert_eq(options.flag_reset_seconds, 420,
			"the flag carry limit carries the engine's retail default")
	assert_eq(options.armory_reuse_time, 30,
			"the armory-reuse cooldown carries the engine's retail default")
	assert_eq(options.capture_duration_seconds, 15)
	assert_eq(options.capture_speed_setting, 1)
	assert_eq(options.spawn_wave_time_base, 0)
	assert_eq(options.spawn_wave_time_zone, 10)
	assert_eq(options.default_spawn_requires_no_team_zone, 0)
	assert_eq(options.num_teams, 2)
	assert_eq(options.respawn_timeout, 5)
	assert_eq(options.start_delay, 0)
	assert_eq(options.destroy_buildings, 0)
	assert_eq(options.death_messages, 1)
	assert_eq(options.integrity_profile, "",
			"ordinary hosts do not infer an integrity corpus from expansion")
	config.integrity_profile = "retail-revx02-024f56f2-2d087374"
	assert_eq(config.to_session_options().integrity_profile,
			config.integrity_profile,
			"parity automation can bind both host and client to one witnessed corpus")
	config.class_allow_mask = 1 << 6
	assert_eq(config.to_session_options().class_allow_mask, 1 << 6,
			"the typed host request forwards a map-specific Soldier Class policy")


func test_host_rule_controls_reach_native_session_configuration() -> void:
	var body := ""
	var edits := {"SERVER_PASSWORD": "secret", "SERVER_MESSAGE": "Squad night",
			"DELAY": "7", "RESPAWN": "8", "TIME": "45", "MAX_KOTH": "0",
			"KILL_LIMIT": "500", "MAX_SCORE": "500", "TAKEOVER_TIME": "20",
			"MAX_PLAYERS": "99"}
	for control in edits:
		body += MenuDriverFixture.wnd("edit", control, 10)
	for control in ["TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TEAM_CHOOSE",
			"CLAYMORE_PREF", "TRACERS", "LFP_TAKEOVER", "SERVERTYPE"]:
		body += MenuDriverFixture.wnd("spinlist", control, 40,
				'<ITEMS><ITEM value="0">ZERO</ITEM><ITEM value="1">ONE</ITEM></ITEMS>')
	body += MenuDriverFixture.wnd("spinlist", "GAME_LOCATION", 70,
			'<ITEMS><ITEM value="32">US</ITEM></ITEMS>')
	var screen := _host_screen_xml().replace(MenuDriverFixture.wnd("edit", "MAX_PLAYERS", 34), "")
	screen = screen.replace("</WINDOW></SCREEN>", body + "</WINDOW></SCREEN>")
	var driver := MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, screen), "jo_mp.mnu")
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	mp.on_menu_built(driver, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	# The populate seeds the rule controls from the record defaults: the
	# inverted flag spins show a CLEAR bit as 1, the sentinels map back to
	# their dialog numbers, SERVERTYPE reads serve-and-play as 0.
	assert_eq(driver.get_widget_text(driver.widget_id("TIME")), "30")
	assert_eq(driver.get_widget_text(driver.widget_id("RESPAWN")), "5")
	assert_eq(driver.get_widget_text(driver.widget_id("MAX_KOTH")), "10")
	assert_eq(driver.get_widget_text(driver.widget_id("KILL_LIMIT")), "50")
	assert_eq(driver.get_widget_text(driver.widget_id("TAKEOVER_TIME")), "15")
	assert_eq(driver.spin_value_attr(driver.widget_id("FRIENDLY_TAG")), "1",
			"friendly tags default on (mp_attributes bit 0x400 clear)")
	assert_eq(driver.spin_value_attr(driver.widget_id("TEAM_FF")), "0",
			"friendly fire defaults off (mp_attributes bit 0x200 set)")
	assert_eq(driver.spin_value_attr(driver.widget_id("LFP_TAKEOVER")), "1")
	assert_eq(driver.spin_value_attr(driver.widget_id("SERVERTYPE")), "0")
	for control in edits:
		driver.set_widget_text(driver.widget_id(control), edits[control])
	# The user turns friendly fire, tags, the warning and tracers off ...
	for control in ["TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"]:
		driver.select_row(driver.widget_id(control), 0)
	# ... and team choice, claymore preference, the takeover speed and the
	# dedicated server type on.
	for control in ["TEAM_CHOOSE", "CLAYMORE_PREF", "LFP_TAKEOVER", "SERVERTYPE"]:
		driver.select_row(driver.widget_id(control), 1)
	mp.seed_host_pool([_pool_row("alpha.bms")])
	driver.select_row(driver.widget_id("MISSION_LIST"), 0)
	_press(driver, "ADD_MISSIONS")
	_press(driver, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	var config: HostSessionConfig = get_signal_parameters(mp, "lan_host_start_requested")[0]
	var options := config.to_session_options()
	assert_ne(options, config, "the emitted session request is an independent copy")
	assert_eq(options.server_password, "secret")
	assert_eq(options.country, "US", "location stores the item name rather than its numeric value")
	assert_eq(options.custom_text, "Squad night")
	assert_eq(options.start_delay, 7)
	assert_eq(options.respawn_timeout, 8)
	assert_eq(options.respawn_time, 45, "TIME feeds the round-time global")
	assert_eq(options.time_limit_minutes, 0x2222222)
	assert_eq(options.score_limit, 65000)
	assert_eq(options.max_score, 65000)
	assert_eq(options.capture_duration_seconds, 20)
	assert_eq(options.capture_speed_setting, 1)
	assert_eq(options.mp_attributes & 0x860D, 0x860D)
	assert_eq(options.max_players, 64, "the dialog clamps at 64")
	assert_false(options.serve_and_play)
	var sim := Simulation.new()
	sim.configure_host_session(options)
	var live := sim.get_host_session_config()
	assert_eq(live.server_password, "secret", "the password reaches host admission")
	assert_eq(live.max_players, 64, "readback retains the requested player cap")
	assert_eq(live.player_slot_limit, 65, "dedicated hosting adds the reserved slot")
	sim.configure_host_session(live)
	assert_eq(sim.get_host_session_config().player_slot_limit, 65,
			"applying readback does not add a second reserved slot")
	assert_eq(live.score_limit, 65000)
	assert_eq(live.mp_attributes, options.mp_attributes)


func test_lan_join_emits_selected_server() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_lan_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	# The browse result arrives the way the live session delivers it: the
	# injected LanSession's servers_changed signal.
	var session := LanSession.new()
	autofree(session)
	mp.set_lan_session(session)
	var biggy := LanServerRow.make("biggy", "192.168.1.10", 32768)
	biggy.server_flags = JoinTarget.FLAG_ALLOW_SPECTATORS | JoinTarget.FLAG_SPECTATOR_PASSWORD
	biggy.expansion = "jox01"
	biggy.host_key = 0x0FE0E112
	biggy.password_required = true
	session.servers_changed.emit([biggy])
	var lan_list := driver.widget_id("LAN_GAME_LIST")
	driver.set_widget_items(lan_list, PackedStringArray(["biggy (1/4)"]))
	# A single-click selection relays the row index through the driver's aggregate signal.
	driver.select_row(lan_list, 0)
	_press(driver, "LAN_JOINGAME")
	assert_signal_emitted(mp, "lan_join_requested")
	var target: JoinTarget = get_signal_parameters(mp, "lan_join_requested")[0]
	assert_eq(target.host_ip, "192.168.1.10")
	assert_eq(target.port, 32768)
	assert_eq(target.server_name, "biggy", "the browse-row name rides as a display hint")
	assert_eq(target.mission, "", "map identity is absent pre-auth; 0x7B supplies it")
	assert_true(target.allows_spectators(),
			"the selected row carries ServerHello.P2 into the typed join target")
	assert_true(target.spectator_password_required())
	# The row's 0x81 SUS2 names the host's expansion, which the join switches to
	# before it dials (retail switches before it connects).
	assert_eq(target.expansion, "jox01", "the row's expansion rides the join target")
	assert_true(target.expansion_known, "a discovered row's expansion is known")
	assert_false(JoinTarget.new().expansion_known,
			"a bare target (--lan-join) knows no expansion and keeps the mount")
	# The row's record already holds the 0x81: the join goes straight to the 0x42
	# with its HK and SF.
	assert_true(target.discovered_session, "a LAN row's join connects from its session record")
	assert_eq(target.session_host_key, 0x0FE0E112, "the row's HK rides the join target")
	assert_true(target.session_password_required, "the row's SF rides the join target")
	assert_false(JoinTarget.new().discovered_session, "a bare dial sends the 0x41 first")


func test_refreshed_lan_rows_require_a_fresh_selection() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_lan_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var session := LanSession.new()
	autofree(session)
	mp.set_lan_session(session)
	_publish(session, [LanServerRow.make("old", "192.168.1.10", 32768)])
	var lan_list := driver.widget_id("LAN_GAME_LIST")
	driver.select_row(lan_list, 0)  # single click on the old row

	_publish(session, [LanServerRow.make("replacement", "192.168.1.11", 32769)])
	assert_eq(driver.item_count(lan_list), 1,
		"a servers_changed payload replaces the prior full snapshot instead of appending")
	assert_eq(driver.item_text(lan_list, 0), "replacement (0/0)")
	_press(driver, "LAN_JOINGAME")
	assert_signal_not_emitted(mp, "lan_join_requested",
		"refreshed rows invalidate the selection from the previous result set")


# A host discovered while the player has a row highlighted is appended below it;
# the highlighted row stays selected and JOIN still joins it (the retail LAN list
# only appends during a browse window). Before, every new row cleared the
# selection and JOIN did nothing.
func test_appended_lan_row_keeps_the_selection() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_lan_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var session := LanSession.new()
	autofree(session)
	mp.set_lan_session(session)
	var alpha := LanServerRow.make("alpha", "192.168.1.10", 32768)
	var bravo := LanServerRow.make("bravo", "192.168.1.11", 32768)
	var charlie := LanServerRow.make("charlie", "192.168.1.12", 32768)
	_publish(session, [alpha, bravo])
	var lan_list := driver.widget_id("LAN_GAME_LIST")
	driver.select_row(lan_list, 1)  # single click on bravo
	_publish(session, [alpha, bravo, charlie])
	assert_eq(driver.item_count(lan_list), 3, "the new host is appended")
	assert_eq(driver.selected_row(lan_list), 1, "the highlighted row stays selected")
	_press(driver, "LAN_JOINGAME")
	assert_signal_emitted(mp, "lan_join_requested",
		"JOIN still joins the row the player selected after a host was appended")
	if get_signal_emit_count(mp, "lan_join_requested") > 0:
		var target: JoinTarget = get_signal_parameters(mp, "lan_join_requested")[0]
		assert_eq(target.host_ip, "192.168.1.11", "the selected row, not the new one")


func test_swapping_lan_sessions_disconnects_the_previous_discovery_source() -> void:
	var mp := MpMenuCompanion.new()
	var driver := _make_lan_driver()
	mp.on_menu_built(driver, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var previous := LanSession.new()
	autofree(previous)
	var current := LanSession.new()
	autofree(current)
	mp.set_lan_session(previous)
	mp.set_lan_session(current)
	# (pre-auth rows carry no map identity: LanServerRow has no mission field)
	_publish(current, [_lan_row("current", 1, 4)])
	_publish(previous, [_lan_row("stale", 4, 4)])

	var lan_list := driver.widget_id("LAN_GAME_LIST")
	assert_eq(driver.item_count(lan_list), 1)
	assert_eq(driver.item_text(lan_list, 0), "current (1/4)",
		"the current source wins and pre-auth rows do not invent a mission label")


func test_shell_accepts_companion() -> void:
	var shell := MenuShell.new()
	add_child_autofree(shell)
	shell.add_companion(MpMenuCompanion.new())  # installs without a menu loaded, no crash
	assert_null(shell.get_driver(), "no driver is assembled until setup() builds the surface")
