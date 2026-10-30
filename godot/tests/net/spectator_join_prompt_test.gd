extends GutTest

# A discovered retail/OpenNova host whose ServerHello.P2 offers a choice (0x6038:
# passwords, spectators) stops before world loading or ClientAuth on the join
# screen's choice panels (pre.mnu PRE_GAME_MENU; engine inmatch/pre_game_menu.h). The
# flow's decisions are ctest inmatch_join_flow's; these drive the device: the browse's
# LanSession, the screen's panels, the sockets and the NovaWorld session.

const TMP_DIR := "res://.godot/spectator_join_prompt_test"

var _screen: PreGameMenuPresenter = null


func before_each() -> void:
	JoinScreenFixture.stage(self, TMP_DIR)


func after_each() -> void:
	LaunchFlags.clear_args_override()
	_screen = null


func after_all() -> void:
	JoinScreenFixture.unstage(TMP_DIR)


# The controller with no shell shows its choice on a join screen opened over the
# authored pre.mnu (NetSessionController.standalone_join_screen).
func _controller(panel_layer: Control) -> NetSessionController:
	var controller := NetSessionController.new()
	add_child_autofree(controller)
	controller.setup(null, null, null, panel_layer)
	_screen = JoinScreenFixture.open(self, TMP_DIR)
	controller.standalone_join_screen = _screen
	return controller


func _target(flags: int) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 32768
	target.server_name = "Spectator Host"
	target.server_flags = flags
	return target


# D-NET-521: a bare target (no flag word) enumerates first, and the row that
# enumeration finds becomes the join's discovered session: the join goes on with
# the browse's connection, straight to the 0x42 with the row's HK, SF and the
# browse's CI, so the only 0x41s are the browse's. A row with spectators prompts,
# so no world load starts here.
func test_preflight_row_becomes_the_discovered_session() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 32768
	assert_eq(target.entry_step(), JoinTarget.ENTRY_PREFLIGHT,
			"a target with no flag word enumerates first")
	assert_false(target.discovered_session)
	controller.join(target)
	# The preflight browse runs as the controller's SpectatorPreflight LanSession;
	# its row arrives through the browser's own signal.
	var probe := controller.get_node_or_null("SpectatorPreflight") as LanSession
	assert_not_null(probe, "the bare target starts the preflight browse")
	if probe == null:
		return
	assert_false(probe.is_windowed(), "the enumerate-and-join browse has no time limit")
	# The session answers from another address than the one dialed (a name, a
	# broadcast or a multi-homed host): the join goes there.
	var row := LanServerRow.make("Preflight Host", "192.0.2.9", 32768)
	row.server_flags = JoinTarget.FLAG_ALLOW_SPECTATORS
	row.host_key = 0x0FE0E118
	row.password_required = true
	row.client_index = 7
	probe.servers_changed.emit([row])
	assert_true(target.discovered_session, "the preflight row is the join's discovered session")
	assert_eq(target.host_ip, "192.0.2.9", "the join goes to the address the session answered from")
	assert_not_null(target.transport, "the join goes on with the browse's socket")
	assert_eq(target.session_host_key, 0x0FE0E118, "the row's HK rides the join")
	assert_true(target.session_password_required, "the row's SF rides the join")
	assert_eq(target.session_client_index, 7, "the join carries the browse's CI")
	assert_eq(target.server_flags, JoinTarget.FLAG_ALLOW_SPECTATORS)
	assert_true(controller.is_join_choice_pending(),
			"the row's spectator flag opens the join choice")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATE)


# D-NET-703: the enumerate-and-join never gives up on its own and never dials
# without a row: a silent endpoint keeps the browse running past the old 1 s
# fallback, with no prompt and no join, until a session answers or the player
# aborts.
func test_silent_endpoint_keeps_enumerating_without_a_fallback_dial() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.join_role = JoinTarget.ROLE_SPECTATOR
	target.role_explicit = true
	assert_eq(target.entry_step(), JoinTarget.ENTRY_PREFLIGHT,
			"an answered role on an undiscovered word enumerates too")
	controller.join(target)
	var probe := controller.get_node_or_null("SpectatorPreflight") as LanSession
	assert_not_null(probe, "the bare target starts the enumerate")
	if probe == null:
		return
	await get_tree().create_timer(1.5).timeout
	assert_true(is_instance_valid(probe) and probe.is_browsing(), "still enumerating after 1.5 s")
	assert_false(target.discovered_session, "no row-less join")
	assert_false(controller.is_join_choice_pending())


func test_discovered_spectator_host_prompts_before_joining() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(
			JoinTarget.FLAG_ALLOW_SPECTATORS
			| JoinTarget.FLAG_SPECTATOR_PASSWORD)

	controller.join(target)

	assert_true(controller.is_join_choice_pending(),
			"an enabled ServerHello.P2 opens the retail role decision")
	assert_false(target.role_explicit,
			"no join role is committed before the user chooses")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATE,
			"spectator slots open the SPECTATE panel")
	JoinScreenFixture.set_checked(_screen, "SPECTATE", true)
	JoinScreenFixture.press(_screen, "ACCEPT")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATOR_PASSWORD,
			"P2 bit 0x4000 asks a spectator for its password in the shared control")
	assert_true(_screen.is_window_shown("SPECTATOR_STATIC_WRAPPER"))
	assert_false(target.role_explicit, "nothing is committed before the last ACCEPT")


func test_unprotected_spectator_host_omits_password_field_and_cancel_closes() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)

	controller.join(target)

	assert_true(controller.is_join_choice_pending())
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATE)
	assert_false(_screen.is_window_shown("TEAM_PASSWORD_WRAPPER"),
			"no password control while the host asks none")
	JoinScreenFixture.press(_screen, "BACK")
	assert_false(controller.is_join_choice_pending(), "BACK abandons the join")
	assert_false(target.role_explicit)


# The join machine's state 9 ahead of a dial (the choice's BACK here) runs FullDestroy(-1):
# off NovaWorld its transport teardown closes the socket the join would have gone on with
# (with its NET TEARING DOWN SOCKET line while the connect log is on); a NovaWorld join keeps
# its transport (the menu's connect type 1) (engine: inmatch::tear_down_session_transport
# carries the witness; ctest npruntime_connect_log_lines).
func test_the_choice_back_tears_down_a_lan_joins_socket() -> void:
	for novaworld: bool in [false, true]:
		var layer := Control.new()
		add_child_autofree(layer)
		var controller := _controller(layer)
		var pump := UdpPump.new()
		assert_eq(pump.bind_client(), OK, "the browse's socket the join goes on with")
		var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)
		target.transport = pump
		if novaworld:
			target.network_type = JoinTarget.NETWORK_NOVAWORLD
		controller.join(target)
		assert_true(controller.is_join_choice_pending())
		JoinScreenFixture.press(_screen, "BACK")
		assert_false(controller.is_join_choice_pending(), "BACK abandons the join")
		assert_eq(pump.is_open(), novaworld,
				"a NovaWorld join keeps its transport" if novaworld
				else "a LAN join's socket is torn down")
		pump.close()


# A refused row's Cancel is the same state 9: the LAN screen's socket, which the refused join
# never took, is torn down.
func test_a_refused_rows_cancel_tears_down_the_lan_socket() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var pump := UdpPump.new()
	assert_eq(pump.bind_client(), OK, "the LAN screen's search socket")
	# With no shell the refusal's screen is skipped: its Cancel runs at once.
	controller.refuse("CVUNABLETOCREATE")
	assert_false(pump.is_open(), "the refusal's state 9 tears the LAN socket down")
	pump.close()


func test_server_password_prompt_is_independent_of_spectator_access() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD)
	controller.join(target)
	assert_true(controller.is_join_choice_pending())
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_GAME_PASSWORD,
			"a server password opens GAME_PASSWORD")
	assert_false(_screen.is_window_shown("SPECTATE_WRAPPER"),
			"no spectate choice off a host that takes no spectators")
	assert_false(target.role_explicit)


# The server password and the spectator password take their turns: GAME_PASSWORD,
# then SPECTATE, then the shared TEAM_PASSWORD control with the spectator static.
func test_the_passwords_take_their_panels_in_turn() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD
			| JoinTarget.FLAG_ALLOW_SPECTATORS | JoinTarget.FLAG_SPECTATOR_PASSWORD)
	controller.join(target)
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_GAME_PASSWORD)
	JoinScreenFixture.press(_screen, "ACCEPT")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATE)
	JoinScreenFixture.set_checked(_screen, "SPECTATE", true)
	JoinScreenFixture.press(_screen, "ACCEPT")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_SPECTATOR_PASSWORD)


# A TeamChoose host with protected sides asks for the side password and offers
# no team control: retail's PRE.MNU holds GAME_PASSWORD, the SPECTATE radio and
# TEAM_PASSWORD, and every join sends the automatic TR=-1, the side coming from
# the side password (D-NET-590, D-NET-167; the wire is pinned by ctest
# npruntime_client_runtime). BACK commits nothing.
func test_team_choose_host_prompts_for_the_side_password_with_no_team_control() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_TEAM_CHOICE | JoinTarget.FLAG_BLUE_PASSWORD
			| JoinTarget.FLAG_RED_PASSWORD | JoinTarget.FLAG_SERVER_PASSWORD)
	target.join_password = "prefilled"
	controller.join(target)
	assert_true(controller.is_join_choice_pending(),
			"protected sides open the choice even without spectator access")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_GAME_PASSWORD)
	assert_false("team_request" in target, "a join target carries no side request")
	JoinScreenFixture.press(_screen, "ACCEPT")
	assert_eq(_screen.panel(), JoinScreenStatus.PANEL_TEAM_PASSWORD,
			"the side password follows the server password")
	assert_true(_screen.is_window_shown("TEAM_STATIC_WRAPPER"))
	JoinScreenFixture.set_text(_screen, "TEAM_PASSWORD", "unsubmitted")
	JoinScreenFixture.press(_screen, "BACK")
	assert_eq(target.join_password, "prefilled", "BACK does not commit credentials")
	assert_false(target.role_explicit)


# The NovaWorld lobby session survives the panel at the in-match handoff
# (D-NET-220): the controller holds the panel's client until the joiner load
# takes it. A join abandoned before its dial, at the choice's BACK or the
# enumerate's abort, is the join machine's state 9: its destroy keeps a
# NovaWorld join's transport, and the main menu it returns to re-enters the
# NovaWorld menu, so the session goes back to the panel as a normal exit's does,
# never stopped: its socket stays open and writes no NET TEARING DOWN SOCKET
# line (engine: inmatch/host_config.h write_transport_socket_teardown_line
# carries the witness). The harness has no join screen to press ABORT_CANCEL
# on: an enumerate that cannot start (a target with no port) runs the same abort.
func test_an_abandoned_novaworld_join_reenters_the_novaworld_menu_with_its_session() -> void:
	var run_dir := OS.get_cache_dir().path_join("opennova_nw_abandon_run_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	for at_enumerate: bool in [false, true]:
		var cause := "the enumerate's abort" if at_enumerate else "the choice's BACK"
		var layer := Control.new()
		add_child_autofree(layer)
		var controller := _controller(layer)
		controller.open_novaworld_panel(false)
		var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
		assert_not_null(panel, "the NovaWorld panel opens")
		if panel == null:
			break
		var session := NovaWorldClient.new()
		panel.adopt_client(session)
		session.host = "127.0.0.1"
		session.gate_port = 1
		session.start()
		var pump: UdpPump = session.get_game_pump()
		assert_true(pump != null and pump.is_open(), "the session socket is open")
		var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)
		if at_enumerate:
			target = JoinTarget.new()
			target.host_ip = "127.0.0.1"
			target.port = 0
		target.network_type = JoinTarget.NETWORK_NOVAWORLD
		panel.join_in_match_requested.emit(target)
		if not at_enumerate:
			assert_eq(session.get_parent(), controller,
					"the panel's session moves to the controller instead of dying with the panel")
			assert_true(controller.is_join_choice_pending(),
					"the spectator host still asks for the join role")
			JoinScreenFixture.press(_screen, "BACK")
		assert_false(controller.is_join_choice_pending(), "%s abandons the join" % cause)
		var reentered := session.get_parent() as NovaWorldPanel
		assert_not_null(reentered, "%s re-enters the NovaWorld menu with the session" % cause)
		if reentered != null:
			assert_ne(reentered, panel, "a NovaWorld menu opened again after %s" % cause)
			assert_eq(reentered.client_for_test(), session, "the panel holds the kept session")
		assert_eq(session.get_state(), NovaWorldClient.STATE_GATE_PROBING,
				"%s never stops the session" % cause)
		assert_true(pump != null and pump.is_open(),
				"its socket stays open after %s: no teardown, no line" % cause)
		session.stop()
	TestFs.remove_dir_recursive(run_dir)


# The NovaWorld page's join launch saves the player profile, then game.cfg,
# before it dials (here before the join-role question).
func test_novaworld_join_saves_the_profile_first() -> void:
	var run_dir := OS.get_cache_dir().path_join("opennova_nw_join_run_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	controller.open_novaworld_panel()
	var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
	assert_not_null(panel, "the NovaWorld panel opens")
	if panel != null:
		panel.join_in_match_requested.emit(_target(JoinTarget.FLAG_ALLOW_SPECTATORS))
		assert_true(FileAccess.file_exists(run_dir.path_join("player.sav")),
				"the join launch saved player.sav in the working directory")
		assert_true(FileAccess.file_exists(run_dir.path_join(GameCfgStore.FILE_NAME)),
				"and game.cfg beside it")
		if controller.is_join_choice_pending():
			JoinScreenFixture.press(_screen, "BACK")
		await wait_frames(2)
	TestFs.remove_dir_recursive(run_dir)


# The post-mission route's NovaWorld half: only a match entered from the
# NovaWorld menu returns there with its session (the panel's adopt_client leg,
# novaworld_panel_test); a stray session after any other match is left.
func test_normal_exit_outside_novaworld_leaves_a_stray_session() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var session := NovaWorldClient.new()
	controller.return_from_mission(session, false, "")
	assert_null(layer.get_node_or_null("NovaWorldPanel"),
			"no NovaWorld menu re-entry for a match not entered from it")
	await wait_frames(2)
	assert_false(is_instance_valid(session), "the stray session is stopped and freed")


func test_error_exit_shows_the_novaworld_error_dialog() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	controller.return_from_mission(null, true, "The host closed the session.")
	var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
	assert_not_null(panel, "an error exit shows the NovaWorld error dialog")
	if panel == null:
		return
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The host closed the session.")
