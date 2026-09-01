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


func test_login_failure_uses_novaworld_message_screen() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.client_for_test().emit_signal("connected")
	panel.client_for_test().emit_signal(
			"login_failed", "The account name or password is incorrect.")
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The account name or password is incorrect.")
	assert_false(panel.browser_visible(), "an authentication error cannot leak the browser")


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


func test_host_pressed_emits_selected_mission() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	panel.select_mission(1)
	watch_signals(panel)
	panel.press_host()
	assert_signal_emitted(panel, "host_requested")
	var config: HostSessionConfig = get_signal_parameters(panel, "host_requested")[0]
	assert_eq(config.mission, "bravo.bms", "the picked map rides the host request")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_NOVAWORLD,
		"panel hosts on the NovaWorld channel")
	assert_eq(String(config.to_session_options().get("channel", "")),
		HostSessionConfig.CHANNEL_NOVAWORLD,
		"the FFI options retain the NovaWorld period-12 selector")


func test_host_pressed_reports_when_no_missions() -> void:
	var panel := _make_panel(PackedStringArray())
	watch_signals(panel)
	panel.press_host()
	assert_signal_not_emitted(panel, "host_requested", "no missions -> no host request emitted")
	assert_string_contains(panel.status_text(), "No missions", "the empty case is reported, not hung")


# The browser table's row cells (Server, Mission, Mode, Players, Ping, Access).
func test_row_cells_cover_the_table_columns() -> void:
	var row := {"name": "Alpha", "mission_name": "ASH_G11A", "players": 3,
			"max_players": 16, "game_type": "COOP", "exp": "jox01",
			"password": "N", "locked": "N"}
	var cells := NovaWorldPanel.row_cells(row, 42)
	assert_eq(cells[NovaWorldPanel.Column.NAME], "Alpha")
	assert_eq(cells[NovaWorldPanel.Column.MISSION], "ASH_G11A")
	assert_eq(cells[NovaWorldPanel.Column.TYPE], "COOP")
	assert_eq(cells[NovaWorldPanel.Column.PLAYERS], "3/16")
	assert_eq(cells[NovaWorldPanel.Column.PING], "42")
	assert_eq(cells[NovaWorldPanel.Column.ACCESS], "Open")
	row["password"] = "Y"
	assert_eq(NovaWorldPanel.row_cells(row, null)[NovaWorldPanel.Column.ACCESS], "Password",
			"a passworded server is labeled plainly")


# The ping cell's three states (engine/net/novaworld/ping_sweep.h's fold):
# absent = in flight, negative codes = unreachable, else milliseconds.
func test_ping_text_states() -> void:
	assert_eq(NovaWorldPanel.ping_text(null), "...", "in flight shows pending")
	assert_eq(NovaWorldPanel.ping_text(-2), "N/A", "the sweep's failed code")
	assert_eq(NovaWorldPanel.ping_text(-3), "N/A", "the never-attempted code")
	assert_eq(NovaWorldPanel.ping_text(87), "87", "a round-trip shows milliseconds")


func _browser_rows() -> Array:
	return [
		{"rid": 1, "name": "Bravo", "mission_name": "G11", "players": 16,
				"max_players": 16, "game_type": "COOP", "password": "N", "locked": "N"},
		{"rid": 2, "name": "alpha", "mission_name": "Ash", "players": 0,
				"max_players": 32, "game_type": "TDM", "password": "Y", "locked": "N"},
		{"rid": 3, "name": "Charlie", "mission_name": "Delta", "players": 4,
				"max_players": 24, "game_type": "COOP", "password": "N", "locked": "N",
				"mod": "escalation"},
	]


func test_filter_rows_quick_filters_and_search() -> void:
	var rows := _browser_rows()
	assert_eq(NovaWorldPanel.filter_rows(rows, {}).size(), 3, "no filters keeps every row")
	var not_full := NovaWorldPanel.filter_rows(rows, {"hide_full": true})
	assert_eq(not_full.size(), 2, "a 16/16 server hides behind Not full")
	var has_players := NovaWorldPanel.filter_rows(rows, {"hide_empty": true})
	assert_eq(has_players.size(), 2, "an empty server hides behind Has players")
	var unlocked := NovaWorldPanel.filter_rows(rows, {"hide_locked": true})
	assert_eq(unlocked.size(), 2, "a passworded server hides behind No password")
	var by_type := NovaWorldPanel.filter_rows(rows, {"game_type": "coop"})
	assert_eq(by_type.size(), 2, "the type filter matches case-insensitively")
	var by_text := NovaWorldPanel.filter_rows(rows, {"text": "escal"})
	assert_eq(by_text.size(), 1, "the search matches the mod field too")
	assert_eq(String((by_text[0] as Dictionary).get("name", "")), "Charlie")


func test_sort_rows_text_numeric_and_ping() -> void:
	var rows := _browser_rows()
	var by_name := NovaWorldPanel.sort_rows(rows, NovaWorldPanel.Column.NAME, true, {})
	assert_eq(String((by_name[0] as Dictionary).get("name", "")), "alpha",
			"name sorts case-insensitively ascending")
	var by_players := NovaWorldPanel.sort_rows(
			rows, NovaWorldPanel.Column.PLAYERS, false, {})
	assert_eq(int((by_players[0] as Dictionary).get("players", -1)), 16,
			"players sorts numerically descending")
	# Ping: rid 3 fastest, rid 1 slower, rid 2 unmeasured -> always last.
	var pings := {3: 20, 1: 95}
	var by_ping := NovaWorldPanel.sort_rows(rows, NovaWorldPanel.Column.PING, true, pings)
	assert_eq(int((by_ping[0] as Dictionary).get("rid", 0)), 3)
	assert_eq(int((by_ping[2] as Dictionary).get("rid", 0)), 2,
			"an unmeasured ping sorts last ascending")
	var by_ping_desc := NovaWorldPanel.sort_rows(
			rows, NovaWorldPanel.Column.PING, false, pings)
	assert_eq(int((by_ping_desc[0] as Dictionary).get("rid", 0)), 1,
			"descending flips the measured order")
	assert_eq(int((by_ping_desc[2] as Dictionary).get("rid", 0)), 2,
			"an unmeasured ping sorts last descending too")


func test_details_lines_and_roster_seams() -> void:
	var row := {"name": "Bravo", "msg": "Friday night co-op", "mission_name": "G11",
			"game_type": "COOP", "players": 4, "max_players": 24, "mod": "escalation",
			"ver1": "1.7.5.7", "dedicated": "Y", "password": "Y",
			"player_names": PackedStringArray(["ljim", "walker"])}
	var lines := NovaWorldPanel.server_details_lines(row)
	var text := "\n".join(lines)
	assert_string_contains(text, "Message: Friday night co-op")
	assert_string_contains(text, "Players: 4/24")
	assert_string_contains(text, "Mod: escalation")
	assert_string_contains(text, "Version: 1.7.5.7")
	assert_string_contains(text, "Dedicated server")
	assert_string_contains(text, "Password protected")
	assert_false(text.contains("Region:"), "empty fields are skipped")


# The table view seams end to end: rows in, filter + sort + ping through the
# real controls, cells out.
func test_browser_view_filters_sorts_and_pings() -> void:
	var panel := _make_panel(PackedStringArray())
	panel.set_rows_for_test(_browser_rows())
	assert_eq(panel.visible_rows().size(), 3, "every row shows unfiltered")
	assert_eq(panel.visible_cell(0, NovaWorldPanel.Column.NAME), "alpha",
			"the default sort is name ascending")
	panel.click_column_for_test(NovaWorldPanel.Column.NAME)
	assert_eq(panel.visible_cell(0, NovaWorldPanel.Column.NAME), "Charlie",
			"clicking the sorted column flips the direction")
	panel.apply_filter_for_test("", true, false, false)
	assert_eq(panel.visible_rows().size(), 2, "Not full hides the 16/16 row")
	panel.apply_filter_for_test("", false, false, false)
	panel.set_pings_for_test({1: 33})
	assert_eq(panel.visible_cell(panel.visible_rows().size() - 1,
			NovaWorldPanel.Column.PING), "...",
			"rows without a result still read as in flight")
	var bravo_row := -1
	for i in panel.visible_rows().size():
		if panel.visible_cell(i, NovaWorldPanel.Column.NAME) == "Bravo":
			bravo_row = i
	assert_eq(panel.visible_cell(bravo_row, NovaWorldPanel.Column.PING), "33",
			"the installed ping lands in the row's cell")


func test_server_row_tooltip_lists_details() -> void:
	var panel := _make_panel(PackedStringArray())
	var row := {"mission_name": "ASH_G11A", "region": "Jungle", "country": "US",
			"ip": "203.0.113.7"}
	assert_eq(panel.server_row_tooltip(row),
			"Mission: ASH_G11A\nRegion: Jungle\nCountry: US\nAddress: 203.0.113.7",
			"the tooltip lists mission, locale, and address")
	row["exp"] = "JOE"
	assert_string_contains(panel.server_row_tooltip(row), "Expansion: JOE",
			"an advertised expansion shows in the row details")


func test_expansion_advisory_warns_once_then_defers_to_the_join() -> void:
	# The temp-dir root has no expansions installed, so a row advertising one
	# takes the policy's FAIL decision: the FIRST Join press warns instead of
	# joining, the SECOND proceeds (the in-match 0x7B reconcile stays the
	# authoritative gate, D-NET-178).
	var panel := _make_panel(PackedStringArray())
	var row := {"name": "EscalationHost", "exp": "JOE", "rid": 42}
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
			{"name": "BaseGameHost", "exp": "", "rid": 7}, 7),
			"a base-game row (empty exp) never warns")
	assert_false(panel.expansion_advisory_blocks_first_press(
			{"name": "NoExpField", "rid": 8}, 8),
			"a row with no exp field never warns (stale/absent GSB data)")


func test_host_failed_reports_and_reenables() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	assert_false(panel.host_enabled(), "Host starts disabled until the gate connects")
	panel.host_failed("No mission available to host.")
	assert_eq(panel.status_text(), "No mission available to host.", "the failure reason is shown")
	assert_true(panel.host_enabled(), "Host is re-enabled on the OpenNova target")
