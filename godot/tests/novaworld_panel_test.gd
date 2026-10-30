extends GutTest

# Guards the NovaWorld panel's host Map picker + the host_failed feedback inlet — the fix for the
# "stuck on Starting a NovaWorld host..." bug (the host request used to carry no map and the panel
# had no failure channel). The authored scene is instantiated with client startup disabled, and the
# NovaWorldClient remains unstarted on the authored scene — no gate/HTTP side effects in the unit.

const NovaWorldPanel := preload("res://game/novaworld_panel.gd")
const PANEL_SCENE := preload("res://game/novaworld_panel.tscn")


# A REAL ResourceRoot (ADR 0034 typed seam) over a per-test temp dir: the
# mission set is whatever .bms files the dir carries, including subdirectory
# entries (list_files returns paths; MissionCatalog reduces to basenames).
var _root_dirs: Array[String] = []
# The stored NovaWorld choice (user://novaworld_client.cfg), set aside for the
# tests that write it and put back after each.
var _had_choice := false
var _saved_choice := PackedByteArray()


func before_each() -> void:
	var path := NovaWorldSettings.CONFIG_PATH
	_had_choice = FileAccess.file_exists(path)
	_saved_choice = FileAccess.get_file_as_bytes(path) if _had_choice else PackedByteArray()


func after_each() -> void:
	LaunchFlags.clear_args_override()
	var path := NovaWorldSettings.CONFIG_PATH
	if _had_choice:
		var file := FileAccess.open(path, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_choice)
			file.close()
	elif FileAccess.file_exists(path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	for dir in _root_dirs:
		TestFs.remove_dir_recursive(dir)
	_root_dirs.clear()


func _real_root(missions: PackedStringArray, files := {}) -> ResourceRoot:
	var dir := OS.get_temp_dir().replace("\\", "/") + \
			"/opennova_nw_panel_%d_%d" % [Time.get_ticks_usec(), _root_dirs.size()]
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_root_dirs.append(dir)
	for mission in missions:
		var path := dir.path_join(mission)
		DirAccess.make_dir_recursive_absolute(path.get_base_dir())
		var file := FileAccess.open(path, FileAccess.WRITE)
		assert_not_null(file, "mission fixture %s opens for write" % mission)
		if file != null:
			file.store_string("bms")
			file.close()
	for file_name in files:
		var extra := FileAccess.open(dir.path_join(file_name), FileAccess.WRITE)
		assert_not_null(extra, "fixture %s opens for write" % file_name)
		if extra != null:
			extra.store_string(files[file_name])
			extra.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	return root


func _make_panel(missions: PackedStringArray, files := {}) -> NovaWorldPanel:
	var panel = PANEL_SCENE.instantiate()
	panel.start_client_on_ready = false
	panel.target_override = NovaWorldSettings.Target.OPENNOVA
	panel.resource_root = _real_root(missions, files)
	add_child_autofree(panel)
	return panel


## A browser row authored from its wire field names (the record's properties).
func _row(fields: Dictionary) -> NovaWorldServerRow:
	var row := NovaWorldServerRow.new()
	for key in fields:
		row.set(key, fields[key])
	return row


func _begin_configured_host(panel: NovaWorldPanel, mission := "alpha.bms") -> void:
	var config := HostSessionConfig.new()
	config.mission = mission
	config.missions = [mission]
	config.mission_launch_options = [0]
	panel.begin_configured_host(config)


func _make_panel_in_viewport(viewport_size: Vector2i) -> NovaWorldPanel:
	var viewport := SubViewport.new()
	viewport.size = viewport_size
	add_child_autofree(viewport)
	var panel := PANEL_SCENE.instantiate() as NovaWorldPanel
	panel.start_client_on_ready = false
	panel.target_override = NovaWorldSettings.Target.OPENNOVA
	panel.resource_root = _real_root(PackedStringArray())
	viewport.add_child(panel)
	return panel


# The OpenNova target dials our service's gate (ADR 0010 d2): a source run (this
# suite) reaches the local stack on loopback, never a live service.
func test_opennova_target_dials_loopback_in_a_source_run() -> void:
	var panel := _make_panel(PackedStringArray())
	assert_eq(panel.client_for_test().host, NovaWorldSettings.LOCAL_NOVAWORLD_HOST)


# An export dials its flavour's deployment: a release export prod's gate, a debug
# export (PR CI's zip) staging's, as their OPENNOVA.WIZ names that deployment's
# update service.
func test_opennova_host_is_the_export_flavours_deployment() -> void:
	assert_eq(NovaWorldSettings.opennova_host_for("", true, false), "nw.opennova.net",
			"a release export: prod's gate")
	assert_eq(NovaWorldSettings.opennova_host_for("", true, true), "nw.staging.opennova.net",
			"a debug export: staging's gate")
	assert_eq(NovaWorldSettings.opennova_host_for("", false, true), "127.0.0.1",
			"a source run: loopback")
	assert_eq(NovaWorldSettings.opennova_host_for("", false, false), "127.0.0.1",
			"a source run of a release editor binary: loopback too")
	assert_eq(NovaWorldSettings.opennova_host_for("nw.example.test", true, false),
			"nw.example.test", "the flag beats a release export's own")
	assert_eq(NovaWorldSettings.opennova_host_for("nw.example.test", true, true),
			"nw.example.test", "and a debug export's")
	assert_eq(NovaWorldSettings.opennova_host_for("127.0.0.1", false, true), "127.0.0.1")


func test_novaworld_host_flag_moves_the_opennova_target_alone() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--novaworld-host", "nw.staging.opennova.net"]))
	var panel := _make_panel(PackedStringArray())
	assert_eq(panel.client_for_test().host, "nw.staging.opennova.net")
	var real = PANEL_SCENE.instantiate()
	real.start_client_on_ready = false
	real.target_override = NovaWorldSettings.Target.REAL
	real.resource_root = _real_root(PackedStringArray())
	add_child_autofree(real)
	assert_eq(real.client_for_test().host, NovaWorldSettings.REAL_NOVAWORLD_HOST,
			"Original NovaWorld is NovaLogic's gate whatever the flag says")


# The stored choice: OpenNova's or the original NovaWorld; no file, no key, a
# value of another type or out of range is OpenNova's.
func test_the_stored_novaworld_choice_reads_back_and_defaults_to_opennova() -> void:
	var path := NovaWorldSettings.CONFIG_PATH
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.OPENNOVA, "no file")
	NovaWorldSettings.save_target(NovaWorldSettings.Target.REAL)
	assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.REAL)
	NovaWorldSettings.save_target(NovaWorldSettings.Target.OPENNOVA)
	assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.OPENNOVA)
	for bad: Variant in [7, -1, "1", 1.0, [1]]:
		ConfigStore.write(path, NovaWorldSettings.SECTION, "target", bad)
		assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.OPENNOVA,
				"a bad value (%s) is OpenNova's" % str(bad))


# A launch naming a gate opens on OpenNova's whatever is stored, so a developer's
# launch at a stack never reaches NovaLogic's live gate by a stored pick; a join's
# relaunch returns to the stored target, the join's; else the stored choice.
func test_a_launch_opens_on_its_target() -> void:
	NovaWorldSettings.save_target(NovaWorldSettings.Target.REAL)
	assert_eq(NovaWorldSettings.launch_target("", false), NovaWorldSettings.Target.REAL)
	assert_eq(NovaWorldSettings.launch_target("127.0.0.1", false), NovaWorldSettings.Target.OPENNOVA)
	assert_eq(NovaWorldSettings.launch_target("127.0.0.1", true), NovaWorldSettings.Target.REAL)
	LaunchFlags.set_args_override(PackedStringArray(["--novaworld-host", "127.0.0.1"]))
	var panel = PANEL_SCENE.instantiate()
	panel.start_client_on_ready = false
	panel.resource_root = _real_root(PackedStringArray())
	add_child_autofree(panel)
	assert_eq(panel.client_for_test().host, "127.0.0.1", "the flag's gate, not NovaLogic's")
	var option := panel.get_node("Center/Shell/Margin/RootVBox/Header/TargetOption") as OptionButton
	assert_eq(option.get_selected_id(), NovaWorldSettings.Target.OPENNOVA, "the box shows it")
	assert_eq(NovaWorldSettings.load_target(), NovaWorldSettings.Target.REAL, "the choice is kept")


# The original NovaWorld: sign-in, its list and its joins, but no hosting from
# this menu, which the Host button and its press say.
func test_the_original_novaworld_hosts_nothing_and_says_so() -> void:
	var real = PANEL_SCENE.instantiate()
	real.start_client_on_ready = false
	real.target_override = NovaWorldSettings.Target.REAL
	real.resource_root = _real_root(PackedStringArray(["alpha.bms"]))
	add_child_autofree(real)
	var host_button := real.get_node(
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/Actions/HostButton") as Button
	assert_eq(host_button.tooltip_text, NovaWorldPanel.HOST_ON_OPENNOVA_ONLY)
	real.client_for_test().emit_signal("connected")
	real.client_for_test().emit_signal("login_succeeded", "ljim")
	assert_true(real.browser_visible(), "its list is browsed")
	assert_false(real.host_enabled(), "no hosting on NovaLogic's NovaWorld")
	real.press_host()
	assert_eq(real.message_text(), NovaWorldPanel.HOST_ON_OPENNOVA_ONLY)
	var ours := _make_panel(PackedStringArray())
	var ours_host := ours.get_node(
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/Actions/HostButton") as Button
	assert_eq(ours_host.tooltip_text, "", "OpenNova's hosts")


func test_server_browser_is_gated_until_login_succeeds() -> void:
	var panel := _make_panel(PackedStringArray())
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.CONNECTING)
	assert_false(panel.browser_visible(), "games are hidden while the service connects")
	panel.client_for_test().emit_signal("connected")
	assert_true(panel.login_visible(), "a verified session advances to Sign In")
	assert_false(panel.browser_visible(), "connecting is not enough to reveal games")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")
	assert_true(panel.browser_visible(), "only a successful account login reveals games")


func test_authored_scene_is_matchmaking_and_fits_800_by_600() -> void:
	var panel := _make_panel(PackedStringArray())
	var title := panel.get_node("Center/Shell/Margin/RootVBox/Header/Brand/Title") as Label
	var shell := panel.get_node("Center/Shell") as PanelContainer
	var backdrop := panel.get_node("Backdrop") as ColorRect
	assert_eq(title.text, "MATCHMAKING")
	assert_lte(shell.custom_minimum_size.x, 768.0)
	assert_lte(shell.custom_minimum_size.y, 568.0)
	assert_eq(backdrop.mouse_filter, Control.MOUSE_FILTER_STOP,
			"the transparent overlay blocks clicks into the authored menu")


func test_populated_browser_controls_stay_inside_the_matchmaking_shell() -> void:
	var retail_row := _row({
		"rid": 1,
		"name": "! Long Retail Community Server",
		"msg": "Welcome to community-matchmaking.example.invalid",
		"mission_name": "AS - Padang River Basin",
		"game_type": "AAS",
		"players": 0,
		"max_players": 64,
		"mod": "jox01",
		"ver1": "3",
		"exp": "jox01",
		"region": "Jungle",
		"time_of_day": "Dawn",
		"time_left": "31",
		"dedicated": "Y",
		"player_names": PackedStringArray(),
	})
	var retail_rows: Array[NovaWorldServerRow] = [retail_row]
	for viewport_size in [Vector2i(1406, 896), Vector2i(800, 600)]:
		var panel := _make_panel_in_viewport(viewport_size)
		panel.client_for_test().emit_signal("connected")
		panel.client_for_test().emit_signal("login_succeeded", "ljim")
		panel.set_rows_for_test(retail_rows)
		await wait_process_frames(2)

		var shell := panel.get_node("Center/Shell") as Control
		var shell_rect := shell.get_global_rect()
		var status := panel.get_node("Center/Shell/Margin/RootVBox/StatusLabel") as Control
		var population := panel.get_node(
				"Center/Shell/Margin/RootVBox/Pages/LobbyView/PopulationLabel") as Control
		assert_gte(population.get_global_rect().position.y,
				status.get_global_rect().end.y,
				"the populated browser starts below the header and status at %s" %
						viewport_size)

		var bounded_paths := PackedStringArray([
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/Toolbar",
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/QuickFilters",
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/BrowserStack/BrowserSplit/ServerTree",
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/BrowserStack/BrowserSplit/DetailsPanel",
			"Center/Shell/Margin/RootVBox/Pages/LobbyView/Actions",
		])
		for path in bounded_paths:
			var control := panel.get_node(path) as Control
			var rect := control.get_global_rect()
			assert_gte(rect.position.y, shell_rect.position.y,
					"%s cannot overflow above the shell at %s" %
							[path, viewport_size])
			assert_lte(rect.end.y, shell_rect.end.y,
					"%s cannot overflow below the shell at %s" %
							[path, viewport_size])


func test_login_failure_uses_novaworld_message_screen() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal(
			"login_failed", "The account name or password is incorrect.")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The account name or password is incorrect.")
	assert_eq(panel.status_text(), "Sign-in failed.", "the signing-in status cannot linger")
	assert_false(panel.browser_visible(), "an authentication error cannot leak the browser")


func test_server_list_failure_is_terminal_and_retryable() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")
	panel.client_for_test().emit_signal("server_list_failed", "The request timed out.")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The request timed out.")
	assert_eq(panel.status_text(), "Could not load games.")
	assert_false(panel.browser_visible(), "a failed list never leaves a loading browser exposed")


func test_connection_and_join_failures_reach_the_message_screen() -> void:
	var connection_panel := _make_panel(PackedStringArray())
	connection_panel.client_for_test().emit_signal("error_occurred", "DNS lookup failed.")
	assert_eq(connection_panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_string_contains(connection_panel.message_text(), "matchmaking service")
	assert_string_contains(connection_panel.message_text(), "DNS lookup failed.")

	var join_panel := _make_panel(PackedStringArray())
	join_panel.client_for_test().emit_signal("connected")
	join_panel.client_for_test().emit_signal("login_succeeded", "ljim")
	join_panel.client_for_test().emit_signal("join_failed", "The host is no longer available.")
	assert_eq(join_panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(join_panel.message_text(), "The host is no longer available.")


# A dropped session leaves the panel clean: the rows, the sign-in, the join and
# host requests in flight and the actions they armed all go, and no late answer
# can act on them.
func test_disconnect_resets_the_connection_state() -> void:
	var panel := _panel_with_requests_in_flight()
	panel.client_for_test().emit_signal("disconnected", "The session timed out.")
	_assert_connection_state_reset(panel, "a disconnect")


func test_connection_error_resets_the_connection_state() -> void:
	var panel := _panel_with_requests_in_flight()
	panel.client_for_test().emit_signal("error_occurred", "DNS lookup failed.")
	_assert_connection_state_reset(panel, "an error")


# A populated, signed-in panel with a join and a host request in flight. The
# unstarted session answers both legs at once, so those answers are held off for
# the presses and wired back after them.
func _panel_with_requests_in_flight() -> NovaWorldPanel:
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	var client := panel.client_for_test()
	client.emit_signal("connected")
	client.emit_signal("login_succeeded", "ljim")
	panel.set_rows_for_test(_browser_rows())
	panel.set_pings_for_test({1: 33})
	var held := {}
	for signal_name: String in ["join_failed", "host_failed"]:
		held[signal_name] = client.get_signal_connection_list(signal_name)
		for connection: Dictionary in held[signal_name]:
			client.disconnect(signal_name, connection["callable"])
	panel.press_join()
	_begin_configured_host(panel)
	for signal_name: String in held:
		for connection: Dictionary in held[signal_name]:
			client.connect(signal_name, connection["callable"])
	assert_eq(panel.visible_rows().size(), 3, "the browser is populated")
	assert_eq(panel.joining_rid(), 2, "a join to the selected row is in flight")
	assert_true(panel.host_enabled(), "Host is live on the signed-in session")
	assert_true(panel.host_pending(), "the host request waits on the service")
	return panel


func _assert_connection_state_reset(panel: NovaWorldPanel, cause: String) -> void:
	var client := panel.client_for_test()
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.visible_rows().size(), 0, "%s clears the rows" % cause)
	assert_eq(panel.server_item_count(), 0, "%s empties the table" % cause)
	assert_eq(panel.joining_rid(), 0, "%s drops the join in flight" % cause)
	assert_false(panel.host_enabled(), "%s disables Host" % cause)
	assert_false(panel.join_enabled(), "%s disables Join" % cause)
	assert_false(panel.login_enabled(), "%s disables Login" % cause)
	assert_false(panel.host_pending(), "%s drops the host request in flight" % cause)
	assert_eq(_population_label(panel).text, "", "%s clears the population line" % cause)
	var message := panel.message_text()
	watch_signals(panel)
	panel.press_join()
	assert_eq(panel.joining_rid(), 0, "a Join press after %s submits no row" % cause)
	# Late answers to the dropped requests find nothing to act on.
	client.emit_signal("joined_game", "203.0.113.7", 3000, "APPID", PackedByteArray())
	assert_signal_not_emitted(panel, "join_in_match_requested",
			"a join resolving after %s enters no match" % cause)
	client.emit_signal("hosting_started")
	assert_signal_not_emitted(panel, "host_requested",
			"a hosting granted after %s starts no mission" % cause)
	client.emit_signal("host_failed", "NWEC01")
	assert_eq(panel.message_text(), message, "a host failure after %s reports nothing" % cause)
	client.emit_signal("join_failed", "NWEC09")
	assert_eq(panel.message_text(), message, "a join failure after %s reports nothing" % cause)
	client.emit_signal("login_failed", "The account name or password is incorrect.")
	assert_eq(panel.message_text(), message, "a sign-in failure after %s reports nothing" % cause)
	assert_false(panel.login_enabled(), "a sign-in failure after %s re-arms no Login" % cause)
	client.emit_signal("login_succeeded", "ljim")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE,
			"the report of %s stays up" % cause)
	assert_eq(panel.message_text(), message)
	assert_false(panel.browser_visible(), "a late sign-in cannot reopen the lobby")
	assert_false(panel.host_enabled())
	# A late ping pass reads nothing into the signed-out panel.
	panel.set_rows_for_test(_browser_rows())
	panel.set_pings_for_test({1: 33})
	client.emit_signal("server_pings_updated")
	assert_eq(_ping_cell(panel, "Bravo"), "33",
			"a ping pass after %s leaves the panel's pings alone" % cause)


func _population_label(panel: NovaWorldPanel) -> Label:
	return panel.get_node("Center/Shell/Margin/RootVBox/Pages/LobbyView/PopulationLabel") as Label


func _ping_cell(panel: NovaWorldPanel, server: String) -> String:
	for i in panel.visible_rows().size():
		if panel.visible_cell(i, NovaWorldServerBrowser.COLUMN_NAME) == server:
			return panel.visible_cell(i, NovaWorldServerBrowser.COLUMN_PING)
	return ""


# A session dropped at Sign In takes Login with it, and the press path too
# (Enter in the password field submits without the button).
func test_disconnect_at_sign_in_disarms_login() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	assert_true(panel.login_enabled(), "a verified session arms Login")
	panel.client_for_test().emit_signal("disconnected", "The session timed out.")
	assert_false(panel.login_enabled(), "the disconnect disarms Login")
	var form := "Center/Shell/Margin/RootVBox/Pages/LoginView/LoginCard/LoginMargin/LoginForm/"
	(panel.get_node(form + "UsernameEdit") as LineEdit).text = "ljim"
	var password := panel.get_node(form + "PasswordEdit") as LineEdit
	password.text = "secret"
	var status := panel.status_text()
	password.text_submitted.emit(password.text)
	assert_eq(panel.status_text(), status, "Enter after the disconnect starts no sign-in")


# Enter on the sign-in page runs the page's VK_RETURN hotkey: in either field, which lets the
# keyboard go first, or in neither, one Enter presses the submit while it is enabled. (The
# unstarted session fails the sign-in at once, so a press shows the message screen.)
# [orig: CEditWnd_HandleKeyEvent @0x6623A0 case 0xD - g_UIFocusWnd = 0 @0x66249F, the VK_RETURN
#  scan called @0x6624EF; UI_DispatchKeyboardEventToChildren @0x63AD10 - the WM_KEYDOWN focus
#  test @0x63ADDA; CWnd_IsVisibleInHierarchy @0x646290]
func test_enter_on_sign_in_runs_the_submit_hotkey() -> void:
	var form := "Center/Shell/Margin/RootVBox/Pages/LoginView/LoginCard/LoginMargin/LoginForm/"
	var pressed := "The matchmaking service is still connecting."
	var enter := InputEventKey.new()
	enter.keycode = KEY_ENTER
	enter.pressed = true
	for field_name in ["UsernameEdit", "PasswordEdit"]:
		var panel := _make_panel(PackedStringArray())
		panel.client_for_test().emit_signal("connected")
		(panel.get_node(form + "UsernameEdit") as LineEdit).text = "ljim"
		var field := panel.get_node(form + field_name) as LineEdit
		field.grab_focus()
		assert_true(field.has_focus(), "%s holds the keyboard" % field_name)
		field.text_submitted.emit(field.text)
		assert_false(field.has_focus(), "Enter in %s lets the keyboard go" % field_name)
		assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE,
				"one Enter in %s presses the submit" % field_name)
		assert_eq(panel.message_text(), pressed)

	var bare := _make_panel(PackedStringArray())
	bare.client_for_test().emit_signal("connected")
	(bare.get_node(form + "UsernameEdit") as LineEdit).text = "ljim"
	assert_null(bare.get_viewport().gui_get_focus_owner(), "the page opens with no focus")
	bare.get_viewport().push_input(enter)
	assert_eq(bare.current_screen(), NovaWorldPanel.Screen.MESSAGE,
			"Enter with no field holding the keyboard presses the submit")
	assert_eq(bare.message_text(), pressed)

	# A submit that is not enabled (DFX2's LOGIN gate closed, a sign-in in flight) is skipped:
	# the field still lets the keyboard go.
	var closed := _make_panel(PackedStringArray())
	closed.client_for_test().emit_signal("connected")
	(closed.get_node(form + "LoginButton") as Button).disabled = true
	var user := closed.get_node(form + "UsernameEdit") as LineEdit
	user.text = "ljim"
	user.grab_focus()
	var status := closed.status_text()
	user.text_submitted.emit(user.text)
	closed.get_viewport().push_input(enter)
	assert_false(user.has_focus(), "Enter lets the keyboard go with the submit disabled")
	assert_eq(closed.current_screen(), NovaWorldPanel.Screen.LOGIN, "and presses nothing")
	assert_eq(closed.status_text(), status)


# The sign-in page's REMEMBER_LOGIN checkbox ("Remember my login information.", the GLOBAL_VAR
# checkbox of JO's and DFX2's sign-in templates): the page's activation shows the state it sets
# from game.cfg's rememberlogin; a click flips it and stores the new state as the browser state's
# property (which the next nw_cdata.coo save reads) and as rememberlogin, writing no file (the
# shell's game.cfg save points write the word). The checkbox takes clicks only. (engine:
# LobbyHttpFlow::store_remember_login, fill_remembered_login.)
func test_remember_login_checkbox_stores_rememberlogin() -> void:
	var run_dir := OS.get_cache_dir().path_join("opennova_nw_panel_rl_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	_root_dirs.append(run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	GameCfgStore.reset()
	var block := GameCfgStore.block()
	block.set_word("rememberlogin", 0)
	var panel := _make_panel(PackedStringArray())
	var client := panel.client_for_test()
	client.emit_signal("connected")
	var form := "Center/Shell/Margin/RootVBox/Pages/LoginView/LoginCard/LoginMargin/LoginForm/"
	var check := panel.get_node(form + "RememberLoginCheck") as CheckBox
	assert_eq(check.text, "Remember my login information.")
	assert_eq(check.focus_mode, Control.FOCUS_NONE, "the checkbox takes no keyboard")

	# The sign-in page's activation with rememberlogin 0: unchecked.
	check.set_pressed_no_signal(true)
	client.reshow_sign_in()
	client.emit_signal("web_lobby_opened", false, "")
	assert_false(check.button_pressed, "the activation shows rememberlogin 0")
	assert_false(client.is_remember_login_checked())

	# A click checks it: the property and rememberlogin take it; no file is written.
	check.button_pressed = true
	assert_true(client.is_remember_login_checked(), "the click sets the property")
	assert_eq(block.get_word("rememberlogin"), 1, "and stores rememberlogin")
	assert_false(FileAccess.file_exists(run_dir.path_join(GameCfgStore.FILE_NAME)),
		"the click saves nothing")
	# The next click clears both.
	check.button_pressed = false
	assert_false(client.is_remember_login_checked())
	assert_eq(block.get_word("rememberlogin"), 0)

	# The next activation shows rememberlogin as it stands (1 here).
	block.set_word("rememberlogin", 1)
	client.reshow_sign_in()
	client.emit_signal("web_lobby_opened", false, "")
	assert_true(check.button_pressed, "the activation shows rememberlogin 1")
	assert_true(client.is_remember_login_checked())
	assert_eq(block.get_word("rememberlogin"), 1, "the activation stores nothing")
	GameCfgStore.reset()


# A live Join goes with the session that armed it.
func test_error_in_the_lobby_disarms_join() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")
	panel.set_rows_for_test(_browser_rows())
	assert_true(panel.join_enabled(), "the selected row's Join is live")
	panel.client_for_test().emit_signal("error_occurred", "DNS lookup failed.")
	assert_false(panel.join_enabled(), "the error disarms Join")


func test_empty_and_filtered_states_are_not_fake_server_rows() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal("login_succeeded", "ljim")
	panel.set_rows_for_test([])
	assert_eq(panel.server_item_count(), 0, "empty state adds no selectable server")
	panel.set_rows_for_test(_browser_rows())
	assert_eq(panel.server_item_count(), 3)
	assert_string_contains(panel.details_text(), "Server: alpha",
			"the first sorted server is selected when no prior rid exists")


# Host goes out on the panel's own session (ConnectOrHost's hosting leg): the
# mission only starts once the service granted the hosting.
func test_host_request_waits_for_the_service() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	watch_signals(panel)
	_begin_configured_host(panel)
	assert_signal_not_emitted(panel, "host_requested",
		"no mission starts before the service's ServerHostResult")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE,
		"a session that is not set up fails the hosting leg")
	assert_string_contains(panel.message_text(), "NWEC01")


# The NovaWorld page's launch saves the player profile, then game.cfg, before
# its host leg, whatever the hosting answers (here the unstarted session's
# NWEC01).
func test_host_press_saves_the_profile_first() -> void:
	var run_dir := OS.get_cache_dir().path_join("opennova_nw_panel_run_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	_root_dirs.append(run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	_begin_configured_host(panel)
	assert_true(FileAccess.file_exists(run_dir.path_join("player.sav")),
		"the launch saved player.sav in the working directory")
	assert_true(FileAccess.file_exists(run_dir.path_join(GameCfgStore.FILE_NAME)),
		"and game.cfg beside it")
	assert_string_contains(panel.message_text(), "NWEC01", "before the host leg failed")


func test_hosting_started_hands_the_request_up() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	var client := panel.client_for_test()
	# The unstarted session fails the leg at once; detach that answer so the
	# service's grant can be played in its place.
	for connection in client.get_signal_connection_list("host_failed"):
		client.disconnect("host_failed", connection["callable"])
	watch_signals(panel)
	_begin_configured_host(panel, "bravo.bms")
	client.emit_signal("hosting_started")
	assert_signal_emitted(panel, "host_requested")
	var config: HostSessionConfig = get_signal_parameters(panel, "host_requested")[0]
	assert_eq(config.mission, "bravo.bms", "the picked map rides the host request")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_NOVAWORLD,
		"panel hosts on the NovaWorld channel")
	assert_eq(config.to_session_options().channel,
		HostSessionConfig.CHANNEL_NOVAWORLD,
		"the FFI options retain the NovaWorld period-12 selector")


# The multiplayer menu's MULTI_PLAYER_HOST as the shipped one authors its spins: value= rows (an
# <ITEM> with none holds 0), GAME_LOCATION's two-letter rows behind an empty one.
func _host_dialog_menu() -> String:
	var body := MenuDriverFixture.wnd("edit", "GAME_NAME", 10)
	body += MenuDriverFixture.wnd("spinlist", "GAME_LOCATION", 34,
			'<ITEMS><ITEM value="33"></ITEM><ITEM value="8">DE</ITEM><ITEM value="32">US</ITEM></ITEMS>')
	body += MenuDriverFixture.wnd("spinlist", "SERVER_PUNKBUSTER", 58,
			'<ITEMS><ITEM>NO</ITEM><ITEM value="1">YES</ITEM></ITEMS>')
	body += MenuDriverFixture.wnd("spinlist", "REPLAY", 82,
			'<ITEMS><ITEM>NEXT</ITEM><ITEM value="1">REPLAY</ITEM></ITEMS>')
	body += MenuDriverFixture.wnd("edit", "MAX_PLAYERS", 106)
	return MenuDriverFixture.screen_xml("MULTI_PLAYER_HOST", body)


# NovaWorld starts with the same rendered configuration as the LAN host screen.
func test_host_request_uses_the_rendered_host_dialog() -> void:
	var run_dir := OS.get_cache_dir().path_join("opennova_nw_panel_cfg_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(run_dir), OK)
	_root_dirs.append(run_dir)
	var cfg_path := run_dir.path_join(GameCfgStore.FILE_NAME)
	var file := FileAccess.open(cfg_path, FileAccess.WRITE)
	assert_not_null(file, "game.cfg opens for write")
	if file != null:
		file.store_string("version = 29\r\ngame_name = \"Night Ops\"\r\nmpmaxplayers = 80\r\n" +
				"sv_punkbuster = 1\r\nreplay = 2\r\ncountry = \"us\"\r\n")
		file.close()
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", run_dir]))
	GameCfgStore.reset()
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	var driver := MenuDriverFixture.driver_over(self, MenuDriverFixture.doc_from_xml(self, _host_dialog_menu()), "mp.mnu", "MULTI_PLAYER_HOST")
	GameCfgStore.block().populate_host_screen(driver)
	GameCfgStore.block().read_host_screen(driver)
	var client := panel.client_for_test()
	for connection in client.get_signal_connection_list("host_failed"):
		client.disconnect("host_failed", connection["callable"])
	watch_signals(panel)
	_begin_configured_host(panel)
	client.emit_signal("hosting_started")
	assert_signal_emitted(panel, "host_requested")
	var config: HostSessionConfig = get_signal_parameters(panel, "host_requested")[0]
	assert_eq(config.server_name, "Night Ops", "the request hosts under game.cfg's name")
	assert_eq(config.max_players, 64, "under the cap the screen's populate clamped")
	assert_eq(config.server_punkbuster, 0, "with PunkBuster stored 0 (no pb\\pbcl.dll)")
	assert_eq(config.country, "US", "and the country the location row's own name")
	assert_eq(GameCfgStore.block().host_dialog_value("GAME_NAME"), "Night Ops",
		"the block keeps the file's name")
	assert_eq(GameCfgStore.block().host_dialog_value("REPLAY"), "0",
		"and takes row 0's value for a word no REPLAY row holds")
	var saved := FileAccess.get_file_as_string(cfg_path)
	assert_string_contains(saved, 'country                 = "US"',
		"the launch's save writes the block's country, not the file's")
	assert_string_contains(saved, "replay                  = 0",
		"and its replay word")
	GameCfgStore.reset()


# The NovaWorld menu's re-entry after a match: the panel takes the session
# back, leaves its hosting and play, and lands in the lobby when signed in.
func test_adopted_session_lands_in_the_lobby_or_sign_in() -> void:
	var panel := _make_panel(PackedStringArray())
	var session := NovaWorldClient.new()
	panel.adopt_client(session)
	assert_eq(panel.client_for_test(), session, "the panel holds the returned session")
	assert_eq(session.get_parent(), panel)
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.LOGIN,
		"an unverified session lands on the sign-in screen")


# The re-entry's web lobby open (the engine's LobbyHttpFlow::enter): its page
# lands the panel on the lobby for a login the service remembers, else on the
# sign-in screen.
func test_web_lobby_open_lands_on_its_page() -> void:
	var panel := _make_panel(PackedStringArray())
	var session := NovaWorldClient.new()
	panel.adopt_client(session)
	session.emit_signal("web_lobby_opened", true, "ljim")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.LOBBY,
		"the lobby page lands on the lobby")
	assert_true(panel.browser_visible(), "the games show behind the remembered login")
	session.emit_signal("web_lobby_opened", false, "")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.LOGIN,
		"the sign-in page lands on sign-in")
	assert_false(panel.browser_visible(), "and hides the games")


# A match that ended with an error text shows it first; Back re-enters
# NovaWorld only when the match was entered from it.
func test_post_mission_error_shows_the_text() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.show_post_mission_error("The host closed the session.", true)
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The host closed the session.")
	assert_null(panel.client_for_test(), "the reset session is gone until Back reconnects")
	panel.show_post_mission_error("", false)
	assert_string_contains(panel.message_text(), "CVUNKNOWN",
		"an empty disconnect text falls back to the unknown-error text")


func test_host_pressed_opens_shared_setup_before_starting() -> void:
	var panel := _make_panel(PackedStringArray())
	watch_signals(panel)
	panel.press_host()
	assert_signal_emitted(panel, "host_setup_requested")
	assert_signal_not_emitted(panel, "host_requested", "the shared host dialog supplies the rotation before starting")


# The browser table model is the engine's (net/novaworld/server_browser.h,
# pinned by ctest server_browser); these cases prove the binding plumbing over
# NovaWorldServerRow: cells, ping states, filter, sort, details.
func test_row_cells_cover_the_table_columns() -> void:
	var row := _row({"name": "Alpha", "mission_name": "ASH_G11A", "players": 3,
			"max_players": 16, "game_type": "COOP", "exp": "jox01",
			"password": "N", "locked": "N"})
	var cells := NovaWorldServerBrowser.row_cells(row, 42)
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_NAME], "Alpha")
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_MISSION], "ASH_G11A")
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_TYPE], "COOP")
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_PLAYERS], "3/16")
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_PING], "42")
	assert_eq(cells[NovaWorldServerBrowser.COLUMN_ACCESS], "Open")
	row.password = "Y"
	assert_eq(NovaWorldServerBrowser.row_cells(row, NovaWorldServerBrowser.PING_PENDING)[
			NovaWorldServerBrowser.COLUMN_ACCESS], "Password",
			"a passworded server is labeled plainly")


# The ping cell's three states: pending, the sweep's negative codes, milliseconds.
func test_ping_text_states() -> void:
	assert_eq(NovaWorldServerBrowser.ping_text(NovaWorldServerBrowser.PING_PENDING), "...",
			"in flight shows pending")
	assert_eq(NovaWorldServerBrowser.ping_text(NovaWorldServerBrowser.PING_FAILED), "N/A",
			"the sweep's failed code")
	assert_eq(NovaWorldServerBrowser.ping_text(NovaWorldServerBrowser.PING_NEVER_ATTEMPTED), "N/A",
			"the never-attempted code")
	assert_eq(NovaWorldServerBrowser.ping_text(87), "87", "a round-trip shows milliseconds")


func _browser_rows() -> Array[NovaWorldServerRow]:
	return [
		_row({"rid": 1, "name": "Bravo", "mission_name": "G11", "players": 16,
				"max_players": 16, "game_type": "COOP", "password": "N", "locked": "N"}),
		_row({"rid": 2, "name": "alpha", "mission_name": "Ash", "players": 0,
				"max_players": 32, "game_type": "TDM", "password": "Y", "locked": "N"}),
		_row({"rid": 3, "name": "Charlie", "mission_name": "Delta", "players": 4,
				"max_players": 24, "game_type": "COOP", "password": "N", "locked": "N",
				"mod": "escalation"}),
	]


func test_filter_rows_quick_filters_and_search() -> void:
	var rows := _browser_rows()
	assert_eq(NovaWorldServerBrowser.filter_rows(rows, "", "", false, false, false).size(), 3,
			"no filters keeps every row")
	assert_eq(NovaWorldServerBrowser.filter_rows(rows, "", "", true, false, false).size(), 2,
			"a 16/16 server hides behind Not full")
	assert_eq(NovaWorldServerBrowser.filter_rows(rows, "", "coop", true, false, false).size(), 1,
			"the type filter and Not full compose (case-insensitive type)")
	var by_text := NovaWorldServerBrowser.filter_rows(rows, "escal", "", false, false, false)
	assert_eq(by_text.size(), 1, "the search matches the mod field too")
	assert_eq(by_text[0].name, "Charlie")


func test_sort_rows_text_numeric_and_ping() -> void:
	var rows := _browser_rows()
	var by_name := NovaWorldServerBrowser.sort_rows(rows, NovaWorldServerBrowser.COLUMN_NAME, true, {})
	assert_eq(by_name[0].name, "alpha", "name sorts case-insensitively ascending")
	# Ping: rid 3 fastest, rid 1 slower, rid 2 unmeasured -> always last.
	var pings := {3: 20, 1: 95}
	var by_ping := NovaWorldServerBrowser.sort_rows(rows, NovaWorldServerBrowser.COLUMN_PING, false, pings)
	assert_eq(by_ping[0].rid, 1, "descending puts the slowest measured ping first")
	assert_eq(by_ping[2].rid, 2, "an unmeasured ping sorts last descending too")


func test_details_lines_and_roster_seams() -> void:
	var row := _row({"name": "Bravo", "msg": "Friday night co-op", "mission_name": "G11",
			"game_type": "COOP", "players": 4, "max_players": 24, "mod": "escalation",
			"ver1": "3", "dedicated": "Y", "password": "Y",
			"player_names": PackedStringArray(["ljim", "walker"])})
	var text := "\n".join(NovaWorldServerBrowser.details_lines(row))
	assert_string_contains(text, "Message: Friday night co-op")
	assert_string_contains(text, "Players: 4/24")
	assert_string_contains(text, "Password protected")
	assert_false(text.contains("Region:"), "empty fields are skipped")
	assert_false(text.contains("Version"), "VER1, the protocol's NovaWorld value, gets no line")
	assert_eq(row.player_names, PackedStringArray(["ljim", "walker"]),
			"the roster rides the record")


# The table view seams end to end: rows in, filter + sort + ping through the
# real controls, cells out.
func test_browser_view_filters_sorts_and_pings() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.set_rows_for_test(_browser_rows())
	assert_eq(panel.visible_rows().size(), 3, "every row shows unfiltered")
	assert_eq(panel.visible_cell(0, NovaWorldServerBrowser.COLUMN_NAME), "alpha",
			"the default sort is name ascending")
	panel.click_column_for_test(NovaWorldServerBrowser.COLUMN_NAME)
	assert_eq(panel.visible_cell(0, NovaWorldServerBrowser.COLUMN_NAME), "Charlie",
			"clicking the sorted column flips the direction")
	panel.apply_filter_for_test("", true, false, false)
	assert_eq(panel.visible_rows().size(), 2, "Not full hides the 16/16 row")
	panel.apply_filter_for_test("", false, false, false)
	panel.set_pings_for_test({1: 33})
	assert_eq(panel.visible_cell(panel.visible_rows().size() - 1,
			NovaWorldServerBrowser.COLUMN_PING), "...",
			"rows without a result still read as in flight")
	var bravo_row := -1
	for i in panel.visible_rows().size():
		if panel.visible_cell(i, NovaWorldServerBrowser.COLUMN_NAME) == "Bravo":
			bravo_row = i
	assert_eq(panel.visible_cell(bravo_row, NovaWorldServerBrowser.COLUMN_PING), "33",
			"the installed ping lands in the row's cell")


func test_server_row_tooltip_lists_details() -> void:
	var row := _row({"mission_name": "ASH_G11A", "region": "Jungle", "country": "US",
			"ip": "203.0.113.7"})
	assert_eq(NovaWorldServerBrowser.row_tooltip(row),
			"Mission: ASH_G11A\nRegion: Jungle\nCountry: US\nAddress: 203.0.113.7",
			"the tooltip lists mission, locale, and address")
	row.exp = "JOE"
	assert_string_contains(NovaWorldServerBrowser.row_tooltip(row), "Expansion: JOE",
			"an advertised expansion shows in the row details")


func test_a_row_on_a_missing_expansion_is_greyed_and_unjoinable() -> void:
	# The temp-dir root has no expansions installed, so a row advertising one
	# takes the join reconcile's FAIL decision (D-NET-178): the browser greys it
	# out, says why, and keeps Join disabled while it is selected.
	var panel := _make_panel(PackedStringArray())
	var rows: Array[NovaWorldServerRow] = [
		_row({"name": "BaseGameHost", "rid": 7, "max_players": 16}),
		_row({"name": "EscalationHost", "exp": "JOE", "rid": 42, "max_players": 16}),
	]
	panel.set_rows_for_test(rows)
	assert_string_contains(panel.join_block_reason(rows[1]), "expansion 'JOE'",
			"the reason names the host's expansion")
	assert_false(panel.visible_row_dimmed(0), "a base-game row draws normally")
	assert_true(panel.visible_row_dimmed(1), "the missing-expansion row draws greyed out")
	assert_true(panel.join_enabled(), "the auto-selected base-game row can be joined")
	panel.select_visible_row_for_test(1)
	assert_false(panel.join_enabled(), "Join stays disabled on the unjoinable row")
	assert_string_contains(panel.details_text(), "Cannot join: Requires expansion 'JOE'",
			"the details pane leads with the reason")
	panel.select_visible_row_for_test(0)
	assert_true(panel.join_enabled(), "selecting a joinable row re-enables Join")


func test_a_server_that_refused_this_install_is_greyed_with_its_reason() -> void:
	var panel := _make_panel(PackedStringArray())
	var rows: Array[NovaWorldServerRow] = [
		_row({"name": "AlphaHost", "rid": 7, "max_players": 16}),
		_row({"name": "BravoHost", "rid": 42, "max_players": 16}),
	]
	panel.set_rows_for_test(rows)
	assert_false(panel.visible_row_dimmed(1), "before the refusal the row draws normally")
	panel.mark_refused(42, "Your game is incompatible with this server. (NCC007)")
	assert_eq(panel.join_block_reason(rows[1]),
			"Your game is incompatible with this server. (NCC007)",
			"the reason is the refusal's own text")
	assert_true(panel.visible_row_dimmed(1), "the refused server draws greyed out")
	panel.select_visible_row_for_test(1)
	assert_false(panel.join_enabled(), "Join stays disabled on it")
	assert_string_contains(panel.details_text(), "Cannot join: Your game is incompatible")
	assert_eq(panel.join_block_reason(rows[0]), "", "other servers stay joinable")


func test_rows_without_an_expansion_are_joinable() -> void:
	var panel := _make_panel(PackedStringArray())
	assert_eq(panel.join_block_reason(_row({"name": "BaseGameHost", "exp": "", "rid": 7})), "",
			"a base-game row (empty exp) is joinable")
	assert_eq(panel.join_block_reason(_row({"name": "NoExpField", "rid": 8})), "",
			"a row with no exp field is joinable (stale/absent GSB data)")


func test_host_failed_reports_and_reenables() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	assert_false(panel.host_enabled(), "Host starts disabled until the gate connects")
	panel.host_failed("No mission available to host.")
	assert_eq(panel.status_text(), "No mission available to host.", "the failure reason is shown")
	assert_true(panel.host_enabled(), "Host is re-enabled on the OpenNova target")


func test_join_rejection_resolves_menutxt_and_retains_diagnostic_code() -> void:
	var previous := Strings.get_table(Strings.TABLE_MENUTXT)
	var table := RtxtStringFile.new()
	table.add_section("NovaWorld")
	table.add_entry("NWEC09", "This game's identity does not match the server.", 0, Vector2i())
	Strings.register_table(Strings.TABLE_MENUTXT, table)
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal("login_succeeded", "synthetic")
	panel.client_for_test().emit_signal("join_failed", "NWEC09")
	assert_eq(panel.message_text(), "This game's identity does not match the server.\n\n(NWEC09)")
	Strings.register_table(Strings.TABLE_MENUTXT, previous)


# A connect give-up the client reports as an NWEC tag (D-NET-571's NWEC42, the
# state-1 give-up) shows its menutxt text, as the retail NovaWorld error dialog
# shows the connect's NWEC texts; a text that already names its code (the stock
# "(Code NWECnn)" ending) is shown as it is, with no second tag; a prose message
# keeps its lead.
func test_connection_error_tag_resolves_menutxt() -> void:
	var previous := Strings.get_table(Strings.TABLE_MENUTXT)
	var table := RtxtStringFile.new()
	table.add_section("NovaWorld")
	table.add_entry("NWEC42", "Unable to connect to NovaWorld. (Code NWEC42)", 0, Vector2i())
	Strings.register_table(Strings.TABLE_MENUTXT, table)
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("error_occurred", "NWEC42")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "Unable to connect to NovaWorld. (Code NWEC42)")
	var prose_panel := _make_panel(PackedStringArray())
	prose_panel.client_for_test().emit_signal("error_occurred", "DNS lookup failed.")
	assert_string_contains(prose_panel.message_text(), "matchmaking service")
	Strings.register_table(Strings.TABLE_MENUTXT, previous)
