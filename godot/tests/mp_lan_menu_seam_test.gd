extends GutTest

# The mp.mnu LAN co-op menu seam's device half: the companion (mp_menu_companion.gd)
# over the engine's LAN controller (runtime/inmatch/lan_multiplayer_menu.h, whose
# rules the ctest lan_multiplayer_menu pins), riding a MenuDriver over a real
# MnuDocument as the shell hands it the document (MenuDriver.claim_screen). This
# pins what stays the device's: the host request the companion makes of the cfg
# block (HostSessionConfig and the native session it configures), the LanSession's
# signals and join row, the JoinTarget decode and the refusal's gameerr text. The
# live two-machine flow (real discovery + a second client spawning) is the manual
# smoke; this is the unit.

var _run_dir := ""


# The host screen shows the process's cfg block (GameCfgStore) and its START
# writes the block back and saves it (D-NET-335): every test runs over a fresh
# block in a working directory of its own.
func before_each() -> void:
	_run_dir = OS.get_cache_dir().path_join("opennova_mp_lan_menu_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()


func after_each() -> void:
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)


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
# markup (edits + mission list + the 3-column rotation table + buttons).
func _host_screen_xml() -> String:
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
	return MenuDriverFixture.screen_xml("MULTI_PLAYER_HOST", body)


func _lan_screen_xml() -> String:
	var body := MenuDriverFixture.wnd("list", "LAN_GAME_LIST", 10)
	body += MenuDriverFixture.wnd("button", "LAN_SEARCH", 200)
	body += MenuDriverFixture.wnd("button", "LAN_JOINGAME", 224)
	return MenuDriverFixture.screen_xml("LAN_MULTI_PLAYER", body)


func _make_host_driver() -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _host_screen_xml()), "jo_mp.mnu")


func _make_lan_driver() -> MenuDriver:
	return MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _lan_screen_xml()), "jo_mp.mnu")


# A control's press through the runtime's own activation (its WidgetActivated event,
# which reaches the controller holding the document).
func _press(driver: MenuDriver, name: String) -> void:
	driver.activate(driver.widget_id(name))


# The shell's hand-over of the document to the companion's controller.
func _hand_over(mp: MpMenuCompanion, driver: MenuDriver) -> void:
	assert_true(driver.claim_screen(mp.screen()), "the LAN controller holds the document")


# A synthetic host-listable catalog row (objective co-op unless overridden —
# stock co-op is host-invisible by the witnessed populate skip).
func _pool_row(file: String, game_type := 0x30020) -> MissionCatalogRow:
	return MissionCatalogRow.create(file, "", "", game_type, false, 1)


func _start_host_config(mission_file: String) -> HostSessionConfig:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver()
	_hand_over(mp, driver)
	mp.seed_host_pool([_pool_row(mission_file)])
	var mission_list := driver.widget_id("MISSION_LIST")
	driver.select_row(mission_list, 0)
	_press(driver, "ADD_MISSIONS")
	_press(driver, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	return get_signal_parameters(mp, "lan_host_start_requested")[0] as HostSessionConfig


func test_start_game_emits_host_config() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_host_driver()
	_hand_over(mp, driver)
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
	# screen's SERVERTYPE spinlist (HG_SERVEONLY value=1) flips this; its value read into the
	# block is pinned by the ctest lan_multiplayer_menu (host_filter).
	assert_true(config.serve_and_play, "no SERVERTYPE control -> serve-and-play default")


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
	assert_eq(options.class_availability, PackedInt32Array([1, 1, 1, 1, 1, 1, 1, 1, 1, 1]),
			"ordinary host requests carry retail's all-ALWAYS class words (Config_SetDefaults)")
	assert_eq(options.round_time_limit_minutes, 30)
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
	config.class_availability = PackedInt32Array([0, 0, 0, 0, 0, 0, 1, 0, 0, 0])
	assert_eq(config.to_session_options().class_availability,
			PackedInt32Array([0, 0, 0, 0, 0, 0, 1, 0, 0, 0]),
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
	_hand_over(mp, driver)
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
	assert_eq(options.round_time_limit_minutes, 45, "TIME feeds the round-time global")
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


# The hosted session's create clamps the cap into 1..65 before the apply, so
# a blank MAX_PLAYERS publishes 1 (2 with the dedicated slot) and a dedicated
# 65+ publishes 66, as opennova-serve's host_session_settings does
# [orig: CNapiGameSession_BuildAndCreateSession @0x569554 (is_in_session),
# @0x56955D..0x56955F, @0x56956A..0x56956C; then the apply
# Game_ApplySessionSettingsToGlobals @0x551b26..0x551b48] (D-NET-335).
func test_host_session_clamps_the_cap_in_session() -> void:
	for case in [[0, true, 1, 1], [0, false, 1, 2], [-3, true, 1, 1], [99, false, 65, 66],
			[66, true, 65, 65], [64, false, 64, 65]]:
		var options := HostSessionOptions.new()
		options.max_players = case[0]
		options.serve_and_play = case[1]
		var sim := Simulation.new()
		sim.configure_host_session(options)
		var live := sim.get_host_session_config()
		assert_eq(live.max_players, case[2], "cap %d (%s) clamps to %d"
				% [case[0], "listen" if case[1] else "dedicated", case[2]])
		assert_eq(live.player_slot_limit, case[3], "cap %d (%s) publishes %d"
				% [case[0], "listen" if case[1] else "dedicated", case[3]])


func test_lan_join_emits_selected_server() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_lan_driver()
	_hand_over(mp, driver)
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
	biggy.client_index = 0x2A
	session.servers_changed.emit([biggy])
	var lan_list := driver.widget_id("LAN_GAME_LIST")
	# A single click selects the row (the runtime's list event reaches the controller).
	driver.select_row(lan_list, 0)
	# The hand-published row has no session record behind it, so a session would refuse
	# its join (test_a_row_with_no_listed_session_refuses_the_join): the session-less
	# companion joins the row as listed, which pins the row's decode.
	mp.set_lan_session(null)
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
	# The join goes on with the browse's connection, its 0x42 carrying the browse's
	# CI (D-NET-521).
	assert_eq(target.session_client_index, 0x2A, "the browse's CI rides the join target")
	assert_eq(JoinTarget.new().session_client_index, 0, "a bare dial knows no browse CI")
	assert_false(JoinTarget.new().discovered_session, "a bare dial sends the 0x41 first")


# A join from a row takes the session's listed session at the row's index; a row whose
# index names none (here a session whose browse holds no record) is refused before
# anything is sent: no join request, the gameerr CVUNABLETOCREATE text instead (engine:
# net/npwire/lan_discovery.h join_session; ctest lan_discovery). The press hands the frame
# to the join machine: the live search's pump stops, and its socket stays open for the
# refusal's state 9, which tears it down (engine: LanDiscoveryBrowser::stop carries the
# witness; spectator_join_prompt_test pins the teardown).
func test_a_row_with_no_listed_session_refuses_the_join() -> void:
	var mp := MpMenuCompanion.new()
	watch_signals(mp)
	var driver := _make_lan_driver()
	_hand_over(mp, driver)
	var session := LanSession.new()
	autofree(session)
	mp.set_lan_session(session)
	assert_eq(session.start_browsing("127.0.0.1", 32787, 32787), OK, "a live search")
	_publish(session, [LanServerRow.make("gone", "192.168.1.10", 32768)])
	var lan_list := driver.widget_id("LAN_GAME_LIST")
	driver.select_row(lan_list, 0)
	assert_null(session.get_join_row(0), "the browse holds no listed session")
	_press(driver, "LAN_JOINGAME")
	assert_signal_not_emitted(mp, "lan_join_requested", "the refused join dials nothing")
	assert_signal_emitted(mp, "lan_join_refused")
	if get_signal_emit_count(mp, "lan_join_refused") > 0:
		var text: String = get_signal_parameters(mp, "lan_join_refused")[0]
		assert_eq(text, Strings.lookup(Strings.TABLE_GAMEERR, "MP Errors", "CVUNABLETOCREATE"),
				"the refusal reads gameerr's CVUNABLETOCREATE")
	assert_false(session.is_browsing(), "the press stops the LAN screen's search")
	var pump: UdpPump = session.take_transport()
	assert_not_null(pump, "the search's socket stays for the join machine's state 9")
	if pump != null:
		assert_true(pump.is_open(), "nothing closed it at the press")
		pump.close()


func test_swapping_lan_sessions_disconnects_the_previous_discovery_source() -> void:
	var mp := MpMenuCompanion.new()
	var driver := _make_lan_driver()
	_hand_over(mp, driver)
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
