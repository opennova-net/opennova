extends GutTest

# A discovered retail/OpenNova host carrying ServerHello.P2 spectator flags
# must stop before world loading or ClientAuth and ask for the join role.


func after_each() -> void:
	LaunchFlags.clear_args_override()


func _controller(panel_layer: Control) -> NetSessionController:
	var controller := NetSessionController.new()
	add_child_autofree(controller)
	controller.setup(null, null, null, panel_layer)
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
	assert_eq(target.entry_step(false), JoinTarget.ENTRY_PREFLIGHT,
			"a target with no flag word enumerates first")
	assert_false(target.discovered_session)
	controller.join_lan_server(target)
	# The preflight browse runs as the controller's SpectatorPreflight LanSession;
	# its row arrives through the browser's own signal.
	var probe := controller.get_node_or_null("SpectatorPreflight") as LanSession
	assert_not_null(probe, "the bare target starts the preflight browse")
	if probe == null:
		return
	var row := LanServerRow.make("Preflight Host", "127.0.0.1", 32768)
	row.server_flags = JoinTarget.FLAG_ALLOW_SPECTATORS
	row.host_key = 0x0FE0E118
	row.password_required = true
	row.client_index = 7
	probe.servers_changed.emit([row])
	assert_true(target.discovered_session, "the preflight row is the join's discovered session")
	assert_eq(target.session_host_key, 0x0FE0E118, "the row's HK rides the join")
	assert_true(target.session_password_required, "the row's SF rides the join")
	assert_eq(target.session_client_index, 7, "the join carries the browse's CI")
	assert_eq(target.server_flags, JoinTarget.FLAG_ALLOW_SPECTATORS)
	assert_not_null(layer.get_node_or_null("JoinRolePrompt"),
			"the row's spectator flag asks the role question")


func test_discovered_spectator_host_prompts_before_joining() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(
			JoinTarget.FLAG_ALLOW_SPECTATORS
			| JoinTarget.FLAG_SPECTATOR_PASSWORD)

	controller.join_lan_server(target)

	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt,
			"an enabled ServerHello.P2 opens the retail role decision")
	assert_false(target.role_explicit,
			"no join role is committed before the user chooses")
	if prompt == null:
		return
	var player := prompt.find_child("JoinAsPlayer", true, false) as Button
	var spectator := prompt.find_child("JoinAsSpectator", true, false) as Button
	var password := prompt.find_child("SpectatorPassword", true, false) as LineEdit
	assert_not_null(player)
	assert_not_null(spectator)
	assert_not_null(password,
			"P2 bit 0x4000 reveals the spectator-password field")
	if password != null:
		assert_true(password.secret)
		assert_eq(password.max_length, 17,
				"the prompt preserves retail's 17-character password limit")


func test_unprotected_spectator_host_omits_password_field_and_cancel_closes() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)

	controller.join_lan_server(target)

	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt == null:
		return
	assert_null(prompt.find_child("SpectatorPassword", true, false))
	var cancel := prompt.find_child("CancelJoin", true, false) as Button
	assert_not_null(cancel)
	if cancel != null:
		cancel.pressed.emit()
	assert_false(target.role_explicit)


func test_server_password_prompt_is_independent_of_spectator_access() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD)
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt == null:
		return
	var password := prompt.find_child("ServerPassword", true, false) as LineEdit
	assert_not_null(password)
	if password != null:
		assert_true(password.secret)
	assert_null(prompt.find_child("SpectatorPassword", true, false))
	var spectator := prompt.find_child("JoinAsSpectator", true, false) as Button
	assert_false(spectator.visible)
	assert_false(target.role_explicit)


func test_both_passwords_have_distinct_controls() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD
			| JoinTarget.FLAG_ALLOW_SPECTATORS | JoinTarget.FLAG_SPECTATOR_PASSWORD)
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt != null:
		assert_not_null(prompt.find_child("ServerPassword", true, false))
		assert_not_null(prompt.find_child("SpectatorPassword", true, false))


# A TeamChoose host with protected sides prompts for the side password and
# offers no team control: retail's PRE.MNU holds GAME_PASSWORD, the SPECTATE /
# PLAY radios and TEAM_PASSWORD, and every join sends the automatic TR=-1, the
# side coming from the side password (D-NET-590, D-NET-167; the wire is pinned
# by ctest npruntime_client_runtime).
func test_team_choose_host_prompts_for_the_side_password_with_no_team_control() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_TEAM_CHOICE | JoinTarget.FLAG_BLUE_PASSWORD
			| JoinTarget.FLAG_RED_PASSWORD | JoinTarget.FLAG_SERVER_PASSWORD)
	target.join_password = "prefilled"
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt, "protected sides prompt even without spectator access")
	if prompt == null:
		return
	assert_null(prompt.find_child("TeamChoice", true, false),
			"the prompt offers no team control")
	assert_false("team_request" in target, "a join target carries no side request")
	var password := prompt.find_child("JoinPassword", true, false) as LineEdit
	assert_not_null(password)
	assert_not_null(prompt.find_child("ServerPassword", true, false))
	if password != null:
		assert_true(password.secret)
		assert_eq(password.text, "prefilled")
		assert_eq(password.max_length, 63)
		password.text = "unsubmitted"
	var cancel := prompt.find_child("CancelJoin", true, false) as Button
	cancel.pressed.emit()
	assert_eq(target.join_password, "prefilled", "cancel does not commit credentials")
	assert_false(target.role_explicit)


# TeamChoose alone (a plain team game; a stock host advertises 0x904) offers no
# join choice: retail tests the discovered word against 0x6038 (passwords and
# spectators) and joins at once, so the entry dials without a prompt and with
# no credentials (the engine's join_entry_step, ctest join_entry).
func test_team_choice_alone_dials_without_a_prompt() -> void:
	var target := _target(JoinTarget.FLAG_TEAM_CHOICE)
	assert_eq(target.entry_step(false), JoinTarget.ENTRY_DIAL,
			"a plain team game opens no join choice")
	assert_eq(_target(0x904).entry_step(true), JoinTarget.ENTRY_DIAL,
			"a stock LAN team game (P2 0x904) dials")
	assert_eq(target.join_password, "", "no credential is invented")


# The NovaWorld lobby session survives the panel at the in-match handoff
# (D-NET-220): the controller holds the panel's client until the joiner load
# takes it, and an abandoned join (the role prompt cancelled) leaves the play
# and frees it.
func test_abandoned_novaworld_join_frees_the_held_session() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	controller.open_novaworld_panel()
	var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
	assert_not_null(panel, "the NovaWorld panel opens")
	if panel == null:
		return
	var client := panel.client_for_test()
	panel.join_in_match_requested.emit(_target(JoinTarget.FLAG_ALLOW_SPECTATORS))
	assert_eq(client.get_parent(), controller,
			"the panel's session moves to the controller instead of dying with the panel")
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt, "the spectator host still asks for the join role")
	if prompt == null:
		return
	(prompt.find_child("CancelJoin", true, false) as Button).pressed.emit()
	await wait_frames(2)
	assert_false(is_instance_valid(client), "an abandoned join frees the held session")


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
		var prompt := layer.get_node_or_null("JoinRolePrompt")
		if prompt != null:
			(prompt.find_child("CancelJoin", true, false) as Button).pressed.emit()
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
