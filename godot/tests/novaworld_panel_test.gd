extends GutTest

# Guards the NovaWorld panel's host Map picker + the host_failed feedback inlet — the fix for the
# "stuck on Starting a NovaWorld host..." bug (the host request used to carry no map and the panel
# had no failure channel). We drive _build_ui() directly (NOT via _ready / add_child) so the panel's
# NovaWorldClient is never created — no gate/HTTP side effects in the unit.

const NovaWorldPanel := preload("res://game/novaworld_panel.gd")


# A stand-in resource root: the panel only calls has_method("list_files") + list_files(".bms").
class StubRoot:
	extends RefCounted
	var _files: PackedStringArray
	func _init(files: PackedStringArray) -> void:
		_files = files
	func list_files(_ext: String) -> PackedStringArray:
		return _files


func _make_panel(missions: PackedStringArray) -> NovaWorldPanel:
	var panel = NovaWorldPanel.new()
	autofree(panel)  # freed at teardown WITHOUT entering the tree, so _ready/_create_client never run
	panel._target = NovaWorldSettings.Target.OPENNOVA
	panel.resource_root = StubRoot.new(missions)
	panel._build_ui()  # builds the UI + populates the Map picker off-tree
	return panel


func test_map_picker_populates_from_root() -> void:
	var panel := _make_panel(PackedStringArray(["coop/alpha.bms", "coop/bravo.bms"]))
	assert_eq(panel._mission_option.item_count, 2, "Map picker lists the root's .bms missions")
	assert_eq(panel._mission_option.get_item_text(0), "alpha.bms", "items are basenames")
	assert_eq(panel._selected_mission(), "alpha.bms", "first mission selected by default")


func test_host_pressed_emits_selected_mission() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms", "bravo.bms"]))
	panel._mission_option.select(1)
	watch_signals(panel)
	panel._on_host_pressed()
	assert_signal_emitted(panel, "host_requested")
	var config: HostSessionConfig = get_signal_parameters(panel, "host_requested")[0]
	assert_eq(config.mission, "bravo.bms", "the picked map rides the host request")
	assert_eq(config.channel, HostSessionConfig.CHANNEL_NOVAWORLD,
		"panel hosts on the NovaWorld channel")


func test_host_pressed_reports_when_no_missions() -> void:
	var panel := _make_panel(PackedStringArray())
	watch_signals(panel)
	panel._on_host_pressed()
	assert_signal_not_emitted(panel, "host_requested", "no missions -> no host request emitted")
	assert_string_contains(panel._status_label.text, "No missions", "the empty case is reported, not hung")


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


func test_host_failed_reports_and_reenables() -> void:
	var panel := _make_panel(PackedStringArray(["alpha.bms"]))
	panel._host_button.disabled = true
	panel.host_failed("No mission available to host.")
	assert_eq(panel._status_label.text, "No mission available to host.", "the failure reason is shown")
	assert_false(panel._host_button.disabled, "Host is re-enabled on the OpenNova target")
