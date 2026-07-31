class_name MpMenuCompanion
extends MenuCompanion

# Drives the multiplayer menu (mp.mnu) by control NAME for the LAN co-op path. It is a
# companion the game-agnostic NovaMenuShell (nova_menu_shell.gd) delegates to: when the shell
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
# read. LAN search/join call the production NovaLanSession discovery seam.

# The mp.mnu screens this companion owns. The shell skips its generic start/mission
# wiring on a menu containing these so START_GAME is not double-bound to a SP launch.
const OWNED_SCREENS := ["LAN_MULTI_PLAYER", "MULTI_PLAYER_HOST"]

# The player chose to join the highlighted discovered LAN server. The payload is the
# typed dial target decoded from the discovery row (JoinTarget.from_lan_row).
signal lan_join_requested(target: JoinTarget)
# START_GAME on the host screen, with the co-op-minimal host request (see _read_host_config).
signal lan_host_start_requested(config: HostSessionConfig)

var _lan_session = null        # NovaLanSession; injected by MainGame
var _servers: Array = []       # last LAN browse result; rows for LAN_GAME_LIST
var _selected_server := -1
var _browse_error := ""        # last LAN search failure, shown in the empty list


# True when this menu is the JO multiplayer menu (so the shell delegates to us). Keyed on
# control names unique to mp.mnu's LAN/host screens rather than a screen name, since the
# whole document (all screens) is built at once.
func owns_menu(menu: Node) -> bool:
	if menu == null:
		return false
	return menu.find_child("LAN_GAME_LIST", true, false) != null \
		or menu.find_child("SELECTED_MISSIONS", true, false) != null


# Provide the LAN discovery session. Kept injectable for menu and socket seam tests.
func set_lan_session(session) -> void:
	if _lan_session != null and _lan_session.has_signal("servers_changed") \
			and _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.disconnect(_on_servers_changed)
	if _lan_session != null and _lan_session.has_signal("error_occurred") \
			and _lan_session.error_occurred.is_connected(_on_lan_browse_error):
		_lan_session.error_occurred.disconnect(_on_lan_browse_error)
	_lan_session = session
	if _lan_session != null and _lan_session.has_signal("servers_changed") \
			and not _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.connect(_on_servers_changed)
	if _lan_session != null and _lan_session.has_signal("error_occurred") \
			and not _lan_session.error_occurred.is_connected(_on_lan_browse_error):
		_lan_session.error_occurred.connect(_on_lan_browse_error)


# All of mp.mnu's screens are built as (hidden) children at once, so we wire every owned
# screen's controls by name regardless of which screen is visible — matching how the
# shell wires.
func _wire(_file: String, _screen: String) -> void:
	# Single-click selection in the LAN list relays through the menu's aggregate signal.
	# Connected by name so the companion stays decoupled from the concrete menu class.
	if _menu.has_signal("widget_value_changed") \
			and not _menu.is_connected("widget_value_changed", _on_widget_value_changed):
		_menu.connect("widget_value_changed", _on_widget_value_changed)
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
	# Begin LAN session discovery. A missing binding remains a safe no-op so the retail
	# menu can still render in parser-only/test builds.
	_browse_error = ""
	if _lan_session != null and _lan_session.has_method("start_browsing"):
		# Returns a Godot Error; the session also emits error_occurred with the
		# specific reason, which lands in _browse_error first.
		if int(_lan_session.start_browsing()) != OK and _browse_error.is_empty():
			_on_lan_browse_error("could not start the search")
			return
	_refresh_lan_list()


# A failed bind or an all-sends-failed probe burst was previously console-only,
# leaving the player staring at a silently empty list.
func _on_lan_browse_error(message: String) -> void:
	_browse_error = String(message)
	_refresh_lan_list()


func _on_servers_changed(servers: Array) -> void:
	_servers = servers
	_selected_server = -1
	if not servers.is_empty():
		_browse_error = ""
	_refresh_lan_list()


func _refresh_lan_list() -> void:
	var list := _find("LAN_GAME_LIST")
	if not (list is NovaMnuList):
		return
	var rows := PackedStringArray()
	for s in _servers:
		rows.append(_format_server_row(s))
	# Surface a search failure in the list itself (the row is inert: the join
	# guard checks against _servers, which stays empty).
	if rows.is_empty() and not _browse_error.is_empty():
		rows.append("Search failed - %s" % _browse_error)
	(list as NovaMnuList).set_items(rows)


func _format_server_row(s: Dictionary) -> String:
	var name := String(s.get("name", "?"))
	var cur := int(s.get("players", 0))
	var max_p := int(s.get("max_players", 0))
	# Retail LAN enumeration has not joined the session yet, so map identity is
	# deliberately absent here; it arrives in the normal post-auth 0x7B stream.
	# The row format is the witnessed retail pair: with an advertised expansion
	# variant "%s - %s (%ld/%ld)", else "%s (%ld/%ld)".
	# [orig: UI_ProcessLANSessionStateMachine @ 0x558de0 sprintf @0x559493/@0x5594b9]
	var expansion := String(s.get("expansion", "")).strip_edges()
	if expansion.is_empty():
		return "%s (%d/%d)" % [name, cur, max_p]
	return "%s - %s (%d/%d)" % [name, expansion, cur, max_p]


func _on_lan_list_activated(index: int) -> void:
	_selected_server = index
	_on_lan_join()


func _on_lan_join() -> void:
	if _selected_server < 0 or _selected_server >= _servers.size():
		return
	lan_join_requested.emit(JoinTarget.from_lan_row(_servers[_selected_server]))


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
	list.set_items(MissionCatalog.mission_names(_root))


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


# Read the co-op-minimal host request off the built tree by control name. Unread controls
# (rules tab, weapon restrictions, server location) still render; co-op forces COOP — the
# record's game_type default is the witnessed retail Co-op g_GameType
# (HostSessionConfig.GAME_TYPE_COOP), as is the LAN bind port default.
func _read_host_config() -> HostSessionConfig:
	var config := HostSessionConfig.new()
	var server_name := _edit_text("GAME_NAME", "")
	if not server_name.is_empty():
		config.server_name = server_name
	var max_text := _edit_text("MAX_PLAYERS", "")
	config.max_players = clampi(int(max_text) if max_text.is_valid_int() else 4, 1, 99)
	config.missions = _selected_missions()
	if config.missions.size() > 0:
		config.mission = config.missions[0]
	config.game_type_attr = _spin_attr("GAME_TYPE", "")
	# Retail's game-name field is the expansion currently mounted by the game,
	# and is empty for the base game. Never substitute a captured expansion id.
	config.expansion = _root.get_expansion() if _root != null else ""
	config.dedicated = _is_dedicated()  # serve-and-play (false, default) vs dedicated (no local player)
	return config


# Serve-and-play (default) vs dedicated: a DEDICATED host runs the listen server but spawns NO local
# player. mp.mnu's host screen carries this as the SERVERTYPE spinlist [orig: host dialog server-type
# read, UI_HandleHostSessionStart @0x556d00]: HG_SERVEPLAY value="0" (serve-and-play) vs HG_SERVEONLY
# value="1" (dedicated). We read the item's value attr (not its localized label). Absent/0 -> serve-and-play.
func _is_dedicated() -> bool:
	return _spin_attr("SERVERTYPE", "0") == "1"


func _selected_missions() -> Array[String]:
	var table := _find("SELECTED_MISSIONS")
	var out: Array[String] = []
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


# Returns the selected spin-list item's `value=` attribute (the semantic value the
# original reads), not its localized display label. Used to map SERVERTYPE/GAME_TYPE to behavior.
func _spin_attr(name: String, default_value: String) -> String:
	var node := _find(name)
	if node != null and node.has_method("get_value_attr"):  # NovaMnuSpinList
		return String(node.get_value_attr())
	return default_value
