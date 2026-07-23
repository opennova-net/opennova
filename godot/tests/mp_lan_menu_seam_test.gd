extends GutTest

# The mp.mnu LAN co-op menu seam: the companion (mp_menu_host.gd) drives the JO
# multiplayer menu by control NAME — the same Command-by-name convention the shell's
# start/exit controls use, but for hosting/joining a LAN game. This pins the wiring
# (which controls do what, and the host config START_GAME reports). The live two-machine
# flow (real discovery + a second client spawning) is the manual smoke; this is the unit.

const MenuShell := preload("res://game/menu_shell.gd")


class _LanSessionStub extends RefCounted:
	signal servers_changed(servers: Array)

	func publish(servers: Array) -> void:
		servers_changed.emit(servers)


class _LanMenuStub extends Node:
	signal widget_value_changed(widget_name: String, kind: String, index: int, value: String)


# A stand-in for the built mp.mnu host-settings screen: a plain Node (the companion only
# needs find_child + the optional widget_value_changed signal) with the named NovaMnu*
# controls as children, exactly as the menu builder would name them from the .mnu.
func _make_host_menu() -> Node:
	var menu := Node.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	for n in ["GAME_NAME", "MAX_PLAYERS"]:
		var e := NovaMnuEdit.new()
		e.name = n
		menu.add_child(e)
	var mission_list := NovaMnuList.new()
	mission_list.name = "MISSION_LIST"
	menu.add_child(mission_list)
	var selected := NovaMnuTable.new()
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


func _make_lan_menu() -> Node:
	var menu := _LanMenuStub.new()
	menu.name = "Menu"
	add_child_autofree(menu)
	var server_list := NovaMnuList.new()
	server_list.name = "LAN_GAME_LIST"
	menu.add_child(server_list)
	for control_name in ["LAN_SEARCH", "LAN_JOINGAME"]:
		var button := Button.new()
		button.name = control_name
		menu.add_child(button)
	return menu


func _press(menu: Node, name: String) -> void:
	menu.find_child(name, true, false).emit_signal("pressed")


func test_owned_screens_default() -> void:
	assert_true(MpMenuHost.OWNED_SCREENS.has("LAN_MULTI_PLAYER"))
	assert_true(MpMenuHost.OWNED_SCREENS.has("MULTI_PLAYER_HOST"))


func test_owns_menu_detects_mp_menu() -> void:
	var mp := MpMenuHost.new()
	var menu := _make_host_menu()
	assert_true(mp.owns_menu(menu), "a menu carrying SELECTED_MISSIONS is the JO mp menu")
	var plain := Node.new()
	add_child_autofree(plain)
	assert_false(mp.owns_menu(plain), "a plain menu is left to the shell")


func test_add_and_remove_missions() -> void:
	var mp := MpMenuHost.new()
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	var mission_list := menu.find_child("MISSION_LIST", true, false) as NovaMnuList
	mission_list.set_items(PackedStringArray(["alpha.bms", "bravo.bms"]))
	mission_list.select(0)
	_press(menu, "ADD_MISSIONS")
	var table := menu.find_child("SELECTED_MISSIONS", true, false) as NovaMnuTable
	assert_eq(table.get_row_count(), 1, "ADD moved the highlighted mission into the rotation")
	assert_eq(table.get_cell_text(0, 0), "alpha.bms")
	_press(menu, "ADD_MISSIONS")
	assert_eq(table.get_row_count(), 1, "ADD de-dupes the same mission")
	table.select_row(0)
	_press(menu, "REMOVE_MISSIONS")
	assert_eq(table.get_row_count(), 0, "REMOVE dropped the selected row")


func test_start_game_emits_host_config() -> void:
	var mp := MpMenuHost.new()
	watch_signals(mp)
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	(menu.find_child("GAME_NAME", true, false) as LineEdit).text = "CoopNight"
	(menu.find_child("MAX_PLAYERS", true, false) as LineEdit).text = "6"
	var mission_list := menu.find_child("MISSION_LIST", true, false) as NovaMnuList
	mission_list.set_items(PackedStringArray(["alpha.bms"]))
	mission_list.select(0)
	_press(menu, "ADD_MISSIONS")
	_press(menu, "START_GAME")
	assert_signal_emitted(mp, "lan_host_start_requested")
	var config: Dictionary = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.get("server_name"), "CoopNight")
	assert_eq(config.get("max_players"), 6)
	assert_eq(config.get("game_type"), "COOP", "the co-op-minimal bring-up reports Co-op")
	assert_eq(config.get("gametype"), 0x30020,
		"the session uses the witnessed retail Co-op g_GameType, not an AS capture value")
	assert_eq(config.get("expansion"), "",
		"a base/unmounted root advertises no expansion instead of captured jox01")
	assert_eq(config.get("mission"), "alpha.bms")
	assert_eq(config.get("channel"), "LAN")
	# SERVERTYPE absent in the stand-in menu -> serve-and-play (dedicated=false). The real screen's
	# SERVERTYPE spinlist (HG_SERVEONLY value=1) flips this; the value-attr read is unit-tested in
	# mnu_widgets_test (test_spinlist_get_value_attr_returns_value_not_label).
	assert_eq(config.get("dedicated"), false, "no SERVERTYPE control -> serve-and-play default")


func test_start_game_defaults() -> void:
	var mp := MpMenuHost.new()
	watch_signals(mp)
	var menu := _make_host_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "MULTI_PLAYER_HOST", null)
	_press(menu, "START_GAME")
	var config: Dictionary = get_signal_parameters(mp, "lan_host_start_requested")[0]
	assert_eq(config.get("server_name"), "COOPGAME", "blank name -> default")
	assert_eq(config.get("max_players"), 4, "blank cap -> default 4")
	assert_eq(config.get("mission"), "", "no missions selected -> empty")


func test_lan_join_emits_selected_server() -> void:
	var mp := MpMenuHost.new()
	watch_signals(mp)
	mp._servers = [{"name": "biggy", "host_ip": "192.168.1.10", "port": 32768}]
	# A single-click selection relays the row index through the menu's aggregate signal.
	mp._on_widget_value_changed("LAN_GAME_LIST", "list", 0, "biggy (1/4)")
	mp._on_lan_join()
	assert_signal_emitted(mp, "lan_join_requested")
	var server: Dictionary = get_signal_parameters(mp, "lan_join_requested")[0]
	assert_eq(server.get("host_ip"), "192.168.1.10")
	assert_eq(server.get("port"), 32768)


func test_refreshed_lan_rows_require_a_fresh_selection() -> void:
	var mp := MpMenuHost.new()
	watch_signals(mp)
	var menu := _make_lan_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var session := _LanSessionStub.new()
	mp.set_lan_session(session)
	session.publish([{"name": "old", "host_ip": "192.168.1.10", "port": 32768}])
	menu.emit_signal("widget_value_changed", "LAN_GAME_LIST", "list", 0, "old")

	session.publish([{"name": "replacement", "host_ip": "192.168.1.11", "port": 32769}])
	var server_list := menu.find_child("LAN_GAME_LIST", true, false) as NovaMnuList
	assert_eq(server_list.get_item_count(), 1,
		"a servers_changed payload replaces the prior full snapshot instead of appending")
	assert_eq(server_list.get_item_text(0), "replacement (0/0)")
	_press(menu, "LAN_JOINGAME")
	assert_signal_not_emitted(mp, "lan_join_requested",
		"refreshed rows invalidate the selection from the previous result set")


func test_swapping_lan_sessions_disconnects_the_previous_discovery_source() -> void:
	var mp := MpMenuHost.new()
	var menu := _make_lan_menu()
	mp.on_menu_built(menu, "jo_mp.mnu", "LAN_MULTI_PLAYER", null)
	var previous := _LanSessionStub.new()
	var current := _LanSessionStub.new()
	mp.set_lan_session(previous)
	mp.set_lan_session(current)
	current.publish([{"name": "current", "players": 1, "max_players": 4, "mission": "new.bms"}])
	previous.publish([{"name": "stale", "players": 4, "max_players": 4, "mission": "old.bms"}])

	var server_list := menu.find_child("LAN_GAME_LIST", true, false) as NovaMnuList
	assert_eq(server_list.get_item_count(), 1)
	assert_eq(server_list.get_item_text(0), "current (1/4)",
		"the current source wins and pre-auth rows do not invent a mission label")


func test_shell_accepts_companion() -> void:
	var host = MenuShell.new()
	add_child_autofree(host)
	host.add_companion(MpMenuHost.new())  # installs without a menu loaded, no crash
	assert_true(true, "companion installed on a menuless shell without error")
