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


func after_each() -> void:
	for dir in _root_dirs:
		TestFs.remove_dir_recursive(dir)
	_root_dirs.clear()


func _real_root(missions: PackedStringArray) -> ResourceRoot:
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
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	return root


func _make_panel(missions: PackedStringArray) -> NovaWorldPanel:
	var panel = PANEL_SCENE.instantiate()
	panel.start_client_on_ready = false
	panel.target_override = NovaWorldSettings.Target.OPENNOVA
	panel.resource_root = _real_root(missions)
	add_child_autofree(panel)
	return panel


## A browser row authored from its wire field names (the record's properties).
func _row(fields: Dictionary) -> NovaWorldServerRow:
	var row := NovaWorldServerRow.new()
	for key in fields:
		row.set(key, fields[key])
	return row


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


func test_map_picker_populates_from_root() -> void:
	# The loose index is flat (memory: ResourceRoot indexes flat filenames);
	# list_files still returns path-bearing entries, so MissionCatalog's
	# basename reduction is what the picker text asserts below.
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	assert_eq(panel.mission_count(), 2, "Map picker lists the root's .bms missions")
	assert_eq(panel.mission_name_at(0), "alpha.bms", "items are basenames")
	assert_eq(panel.selected_mission(), "alpha.bms", "first mission selected by default")


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
	panel.select_mission(1)
	watch_signals(panel)
	panel.press_host()
	assert_signal_not_emitted(panel, "host_requested",
		"no mission starts before the service's ServerHostResult")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE,
		"a session that is not set up fails the hosting leg")
	assert_string_contains(panel.message_text(), "NWEC01")


func test_hosting_started_hands_the_request_up() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	panel.select_mission(1)
	var client := panel.client_for_test()
	# The unstarted session fails the leg at once; detach that answer so the
	# service's grant can be played in its place.
	for connection in client.get_signal_connection_list("host_failed"):
		client.disconnect("host_failed", connection["callable"])
	watch_signals(panel)
	panel.press_host()
	client.emit_signal("hosting_started")
	assert_signal_emitted(panel, "host_requested")
	var config: HostSessionConfig = get_signal_parameters(panel, "host_requested")[0]
	assert_eq(config.mission, "bravo.bms", "the picked map rides the host request")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_NOVAWORLD,
		"panel hosts on the NovaWorld channel")
	assert_eq(config.to_session_options().channel,
		HostSessionConfig.CHANNEL_NOVAWORLD,
		"the FFI options retain the NovaWorld period-12 selector")


# The NovaWorld menu's re-entry after a match: the panel takes the session
# back, leaves its hosting and play, and lands in the lobby when signed in.
func test_adopted_session_lands_in_the_lobby_or_sign_in() -> void:
	var panel := _make_panel(PackedStringArray())
	var session := NovaWorldClient.new()
	panel.adopt_client(session)
	assert_eq(panel.client_for_test(), session, "the panel holds the returned session")
	assert_eq(session.get_parent(), panel)
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.LOGIN,
		"an unauthenticated session lands on the sign-in screen")


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


func test_host_pressed_reports_when_no_missions() -> void:
	var panel := _make_panel(PackedStringArray())
	watch_signals(panel)
	panel.press_host()
	assert_signal_not_emitted(panel, "host_requested", "no missions -> no host request emitted")
	assert_string_contains(panel.status_text(), "No missions", "the empty case is reported, not hung")


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
			"ver1": "1.7.5.7", "dedicated": "Y", "password": "Y",
			"player_names": PackedStringArray(["ljim", "walker"])})
	var text := "\n".join(NovaWorldServerBrowser.details_lines(row))
	assert_string_contains(text, "Message: Friday night co-op")
	assert_string_contains(text, "Players: 4/24")
	assert_string_contains(text, "Password protected")
	assert_false(text.contains("Region:"), "empty fields are skipped")
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


func test_expansion_advisory_warns_once_then_defers_to_the_join() -> void:
	# The temp-dir root has no expansions installed, so a row advertising one
	# takes the policy's FAIL decision: the FIRST Join press warns instead of
	# joining, the SECOND proceeds (the in-match 0x7B reconcile stays the
	# authoritative gate, D-NET-178).
	var panel := _make_panel(PackedStringArray())
	var row := _row({"name": "EscalationHost", "exp": "JOE", "rid": 42})
	assert_true(panel.expansion_advisory_blocks_first_press(row, 42),
			"the first press on a not-installed expansion row is blocked")
	assert_string_contains(panel.status_text(), "expansion 'JOE'",
			"the warning names the host's expansion")
	assert_string_contains(panel.status_text(), "Press Join again",
			"the warning explains the second-press override")
	assert_false(panel.expansion_advisory_blocks_first_press(row, 42),
			"the second press on the same row proceeds")
	assert_true(panel.expansion_advisory_blocks_first_press(row, 42),
			"the override is one-shot — a later press warns again")


# On NovaWorld the account handle IS the callsign: the host rosters the player
# under the service identity, and a stock client's ClientAuth NA is the handle
# (wire-witnessed live: stock na="ljim" = the 0x7B/0x46 roster name). A local
# callsign would leave the joiner's name-match waiting forever (pre-spawn
# free-cam). Signed out, the local callsign stands.
func test_nw_join_callsign_is_the_signed_in_handle() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.player_name = "Player"
	assert_eq(panel.join_callsign(), "Player",
			"signed out, the local callsign stands")
	panel.set_signed_in_handle("ljim")
	assert_eq(panel.join_callsign(), "ljim",
			"signed in, the NW handle is the join callsign")
	panel.set_signed_in_handle("  ")
	assert_eq(panel.join_callsign(), "ljim",
			"a blank handle cannot clobber the retained one")


func test_expansion_advisory_ignores_rows_without_an_expansion() -> void:
	var panel := _make_panel(PackedStringArray())
	assert_false(panel.expansion_advisory_blocks_first_press(
			_row({"name": "BaseGameHost", "exp": "", "rid": 7}), 7),
			"a base-game row (empty exp) never warns")
	assert_false(panel.expansion_advisory_blocks_first_press(
			_row({"name": "NoExpField", "rid": 8}), 8),
			"a row with no exp field never warns (stale/absent GSB data)")


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
