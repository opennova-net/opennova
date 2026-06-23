class_name MpMenuHost
extends RefCounted

# Drives the multiplayer menu (mp.mnu) by control NAME for the LAN co-op path. It is a
# companion the game-agnostic NovaMenuHost (menu_shell.gd) delegates to: when the shell
# loads a menu the companion owns (the JO mp.mnu LAN browser + host-settings screens),
# the shell hands the whole menu over here instead of running its generic
# launch/mission wiring, so START_GAME on the host screen means "host a game" rather
# than the shell's "launch the first mission". Every other menu stays the shell's.
#
# This is our faithful adaptation of the original's per-screen/per-control command
# registration: the engine wires the LAN browser + host dialog by control name and a
# channel selector (state 1=NovaWorld, 2=LAN) [orig: UI_RegisterLANMultiplayerCallbacks
# @0x558d20; UI_HandleHostSessionStart @0x556d00 — LAN host = SetConnectionMode(3) +
# SetTransportMode(3)]. We are LAN-only here (channel = LAN).
#
# Scope this pass is CO-OP-MINIMAL: the host reads GAME_NAME, the selected missions, the
# player cap, and forces COOP; the rest of the host-settings controls render but are not
# read. LAN search/join call the discovery seam (NovaLanSession, Phase 3); until that is
# provided the browser list stays empty.

# The mp.mnu screens this companion owns. The shell skips its generic start/mission
# wiring on a menu containing these so START_GAME is not double-bound to a SP launch.
const OWNED_SCREENS := ["LAN_MULTI_PLAYER", "MULTI_PLAYER_HOST"]

# The player chose to join the highlighted discovered LAN server. The argument is the
# server row dict from the discovery session ({name, host_ip, port, ...}).
signal lan_join_requested(server: Dictionary)
# START_GAME on the host screen, with the co-op-minimal host config (see _read_host_config).
signal lan_host_start_requested(config: Dictionary)

var _menu: Node  # the NovaMnuMenu at runtime; typed Node so we depend only on its tree + signals
var _root: NovaResourceRoot
var _lan_session = null        # NovaLanSession (Phase 3); null -> no discovery yet
var _servers: Array = []       # last LAN browse result; rows for LAN_GAME_LIST
var _selected_server := -1


# True when this menu is the JO multiplayer menu (so the shell delegates to us). Keyed on
# control names unique to mp.mnu's LAN/host screens rather than a screen name, since the
# whole document (all screens) is built at once.
func owns_menu(menu: Node) -> bool:
	if menu == null:
		return false
	return menu.find_child("LAN_GAME_LIST", true, false) != null \
		or menu.find_child("SELECTED_MISSIONS", true, false) != null


# Provide the LAN discovery/advertise session (Phase 3). Optional in Phase 1.
func set_lan_session(session) -> void:
	_lan_session = session
	if _lan_session != null and _lan_session.has_signal("servers_changed") \
			and not _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.connect(_on_servers_changed)


# Called by NovaMenuHost after each open_menu (re)build of a menu we own. All of mp.mnu's
# screens are built as (hidden) children at once, so we wire every owned screen's controls
# here by name regardless of which screen is visible — matching how the shell wires.
func on_menu_built(menu: Node, _file: String, _screen: String, root: NovaResourceRoot) -> void:
	_menu = menu
	_root = root
	if menu == null:
		return
	# Single-click selection in the LAN list relays through the menu's aggregate signal.
	# Connected by name so the companion stays decoupled from the concrete menu class.
	if menu.has_signal("widget_value_changed") \
			and not menu.is_connected("widget_value_changed", _on_widget_value_changed):
		menu.connect("widget_value_changed", _on_widget_value_changed)
	_wire_lan_browser()
	_wire_host_settings()


# --- LAN browser screen (LAN_MULTI_PLAYER) ------------------------------------

func _wire_lan_browser() -> void:
	_connect_pressed("LAN_SEARCH", _on_lan_search)
	_connect_pressed("LAN_JOINGAME", _on_lan_join)
	var list := _find("LAN_GAME_LIST")
	if list is NovaMnuList:
		if not (list as NovaMnuList).item_activated.is_connected(_on_lan_list_activated):
			(list as NovaMnuList).item_activated.connect(_on_lan_list_activated)
		_refresh_lan_list()


func _on_lan_search() -> void:
	# Begin LAN session discovery. The discovery session is Phase 3; until it exists this
	# is a no-op (the list stays empty), which is the faithful "searching, none found" shape.
	if _lan_session != null and _lan_session.has_method("start_browsing"):
		_lan_session.start_browsing()


func _on_servers_changed(servers: Array) -> void:
	_servers = servers
	_refresh_lan_list()


func _refresh_lan_list() -> void:
	var list := _find("LAN_GAME_LIST")
	if not (list is NovaMnuList):
		return
	var rows := PackedStringArray()
	for s in _servers:
		rows.append(_format_server_row(s))
	(list as NovaMnuList).set_items(rows)


func _format_server_row(s: Dictionary) -> String:
	var name := String(s.get("name", "?"))
	var cur := int(s.get("players", 0))
	var max_p := int(s.get("max_players", 0))
	var mission := String(s.get("mission", ""))
	return "%s (%d/%d) - %s" % [name, cur, max_p, mission]


func _on_lan_list_activated(index: int) -> void:
	_selected_server = index
	_on_lan_join()


func _on_lan_join() -> void:
	if _selected_server < 0 or _selected_server >= _servers.size():
		return
	lan_join_requested.emit(_servers[_selected_server])


# --- Host-settings screen (MULTI_PLAYER_HOST) ---------------------------------

func _wire_host_settings() -> void:
	var mission_list := _find("MISSION_LIST")
	if mission_list is NovaMnuList:
		_seed_mission_list(mission_list as NovaMnuList)
	_connect_pressed("ADD_MISSIONS", _on_add_missions)
	_connect_pressed("REMOVE_MISSIONS", _on_remove_missions)
	_connect_pressed("START_GAME", _on_host_start)


# Fill MISSION_LIST with the resource dir's missions (the available pool). The selected
# rotation is the SELECTED_MISSIONS table, maintained by ADD/REMOVE.
func _seed_mission_list(list: NovaMnuList) -> void:
	var names := PackedStringArray()
	if _root != null:
		for m in _root.list_files(".bms"):
			names.append(String(m).get_file())
	list.set_items(names)


func _on_add_missions() -> void:
	var mission_list := _find("MISSION_LIST")
	var table := _find("SELECTED_MISSIONS")
	if not (mission_list is NovaMnuList) or not (table is NovaMnuTable):
		return
	for idx in (mission_list as NovaMnuList).get_selected_items():
		var name := (mission_list as NovaMnuList).get_item_text(idx)
		if not _table_has_mission(table as NovaMnuTable, name):
			# cols: Mission / Type / Switch (the Switch bitmap value, 0 = off).
			(table as NovaMnuTable).add_row_values(PackedStringArray([name, "COOP", "0"]))


func _on_remove_missions() -> void:
	var table := _find("SELECTED_MISSIONS")
	if not (table is NovaMnuTable):
		return
	# Remove high index first so lower indices stay valid as rows shift down.
	var rows := Array((table as NovaMnuTable).get_selected_rows())
	rows.sort()
	rows.reverse()
	for r in rows:
		(table as NovaMnuTable).remove_row(int(r))


func _on_host_start() -> void:
	lan_host_start_requested.emit(_read_host_config())


# Read the co-op-minimal host config off the built tree by control name. Unread controls
# (rules tab, weapon restrictions, server type/location) still render; co-op forces COOP.
func _read_host_config() -> Dictionary:
	var server_name := _edit_text("GAME_NAME", "")
	if server_name.is_empty():
		server_name = "COOPGAME"
	var max_text := _edit_text("MAX_PLAYERS", "")
	var max_players := clampi(int(max_text) if max_text.is_valid_int() else 4, 1, 99)
	var missions := _selected_missions()
	return {
		"server_name": server_name,
		"missions": missions,
		"mission": String(missions[0]) if missions.size() > 0 else "",
		"max_players": max_players,
		"game_type": "COOP",                       # co-op-minimal: forced
		"game_type_raw": _spin_value("GAME_TYPE", ""),  # the spinlist choice, for later
		"channel": "LAN",
		"net_transport": "lan",
	}


func _selected_missions() -> Array:
	var table := _find("SELECTED_MISSIONS")
	var out: Array = []
	if table is NovaMnuTable:
		for r in range((table as NovaMnuTable).get_row_count()):
			out.append((table as NovaMnuTable).get_cell_text(r, 0))
	return out


# --- Aggregate signal + helpers -----------------------------------------------

func _on_widget_value_changed(widget_name: String, kind: String, index: int, _value: String) -> void:
	if widget_name == "LAN_GAME_LIST" and kind == "list":
		_selected_server = index


func _table_has_mission(table: NovaMnuTable, name: String) -> bool:
	for r in range(table.get_row_count()):
		if table.get_cell_text(r, 0) == name:
			return true
	return false


func _find(name: String) -> Node:
	return _menu.find_child(name, true, false) if _menu != null else null


func _connect_pressed(name: String, handler: Callable) -> void:
	var node := _find(name)
	if node is BaseButton and not (node as BaseButton).pressed.is_connected(handler):
		(node as BaseButton).pressed.connect(handler)


func _edit_text(name: String, default_value: String) -> String:
	var node := _find(name)
	if node is LineEdit:  # NovaMnuEdit extends LineEdit
		return (node as LineEdit).text
	return default_value


func _spin_value(name: String, default_value: String) -> String:
	var node := _find(name)
	if node != null and node.has_method("get_value"):  # NovaMnuSpinList
		return String(node.get_value())
	return default_value
