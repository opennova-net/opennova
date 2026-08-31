extends GutTest

# Guards the NovaWorld panel's host Map picker + the host_failed feedback inlet — the fix for the
# "stuck on Starting a NovaWorld host..." bug (the host request used to carry no map and the panel
# had no failure channel). We drive _build_ui() directly (NOT via _ready / add_child) so the panel's
# NovaWorldClient is never created — no gate/HTTP side effects in the unit.

const NovaWorldPanel := preload("res://game/novaworld_panel.gd")


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
	var panel = NovaWorldPanel.new()
	autofree(panel)  # freed at teardown WITHOUT entering the tree, so _ready/_create_client never run
	panel.resource_root = _real_root(missions)
	panel.build_ui_for_target(NovaWorldSettings.Target.OPENNOVA)  # builds the UI + populates the Map picker off-tree
	return panel


func test_map_picker_populates_from_root() -> void:
	# The loose index is flat (memory: ResourceRoot indexes flat filenames);
	# list_files still returns path-bearing entries, so MissionCatalog's
	# basename reduction is what the picker text asserts below.
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	assert_eq(panel.mission_count(), 2, "Map picker lists the root's .bms missions")
	assert_eq(panel.mission_name_at(0), "alpha.bms", "items are basenames")
	assert_eq(panel.selected_mission(), "alpha.bms", "first mission selected by default")


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


func test_server_row_label_shows_address() -> void:
	var panel := _make_panel(PackedStringArray())
	var row := {"name": "Alpha", "players": 3, "max_players": 16, "game_type": "COOP",
			"password": "N", "locked": "N", "ip": "203.0.113.7"}
	assert_eq(panel.format_server_row(row), "Alpha  (3/16)  COOP  203.0.113.7",
			"the row label carries the server's address")
	row["locked"] = "Y"
	row["ip"] = "0.0.0.0"
	assert_eq(panel.format_server_row(row), "Alpha  (3/16)  COOP  [locked]",
			"an unreported address (0.0.0.0) is omitted, locked marker stays last")


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
