class_name MpMenuCompanion
extends MenuCompanion

# Drives the multiplayer menu (mp.mnu) by control NAME for the LAN co-op path. It is a
# companion the game-agnostic MenuShell (nova_menu_shell.gd) delegates to: when the shell
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
# read. LAN search/join call the production LanSession discovery seam.

# The mp.mnu screens this companion owns. The shell skips its generic start/mission
# wiring on a menu containing these so START_GAME is not double-bound to a SP launch.
const OWNED_SCREENS := ["LAN_MULTI_PLAYER", "MULTI_PLAYER_HOST"]

# The player chose to join the highlighted discovered LAN server. The payload is the
# typed dial target decoded from the discovery row (JoinTarget.from_lan_row).
signal lan_join_requested(target: JoinTarget)
# START_GAME on the host screen, with the co-op-minimal host request (see _read_host_config).
signal lan_host_start_requested(config: HostSessionConfig)

var _lan_session: LanSession = null  # injected by MainGame
var _servers: Array = []       # last LAN browse result; rows for LAN_GAME_LIST
var _selected_server := -1
var _browse_error := ""        # last LAN search failure, shown in the empty list


# True when this menu is the JO multiplayer menu (so the shell delegates to us). Keyed on
# control names unique to mp.mnu's LAN/host screens rather than a screen name, since the
# whole document (all screens) is addressable at once.
func owns_menu(driver: MenuDriver) -> bool:
	if driver == null:
		return false
	return driver.has_widget("LAN_GAME_LIST") or driver.has_widget("SELECTED_MISSIONS")


# Provide the LAN discovery session. Kept injectable for menu and socket seam tests.
func set_lan_session(session: LanSession) -> void:
	if _lan_session != null \
			and _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.disconnect(_on_servers_changed)
	if _lan_session != null \
			and _lan_session.error_occurred.is_connected(_on_lan_browse_error):
		_lan_session.error_occurred.disconnect(_on_lan_browse_error)
	_lan_session = session
	if _lan_session != null \
			and not _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.connect(_on_servers_changed)
	if _lan_session != null \
			and not _lan_session.error_occurred.is_connected(_on_lan_browse_error):
		_lan_session.error_occurred.connect(_on_lan_browse_error)


# All of mp.mnu's screens are addressable at once, so we wire every owned
# screen's controls by name regardless of which screen is visible — matching how the
# shell wires.
func _wire(_file: String, _screen: String) -> void:
	# Single-click selection in the LAN list relays through the driver's aggregate signal.
	if not _driver.widget_value_changed.is_connected(_on_widget_value_changed):
		_driver.widget_value_changed.connect(_on_widget_value_changed)
	if not _driver.list_activated.is_connected(_on_list_activated):
		_driver.list_activated.connect(_on_list_activated)
	_wire_lan_browser()
	_wire_host_settings()


# --- LAN browser screen (LAN_MULTI_PLAYER) ------------------------------------

func _wire_lan_browser() -> void:
	_connect_pressed("LAN_SEARCH", _on_lan_search)
	_connect_pressed("LAN_JOINGAME", _on_lan_join)
	if _driver.has_widget("LAN_GAME_LIST"):
		_refresh_lan_list()


func _on_lan_search() -> void:
	# Begin LAN session discovery. A missing binding remains a safe no-op so the retail
	# menu can still render in parser-only/test builds.
	_browse_error = ""
	if _lan_session != null:
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
	var id := _id("LAN_GAME_LIST")
	if id < 0:
		return
	var rows := PackedStringArray()
	for s in _servers:
		rows.append(_format_server_row(s))
	# Surface a search failure in the list itself (the row is inert: the join
	# guard checks against _servers, which stays empty).
	if rows.is_empty() and not _browse_error.is_empty():
		rows.append("Search failed - %s" % _browse_error)
	_driver.set_widget_items(id, rows)


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


func _on_list_activated(id: int, row: int) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if _driver.widget_name_of(id).nocasecmp_to("LAN_GAME_LIST") == 0:
		_selected_server = row
		_on_lan_join()


func _on_lan_join() -> void:
	if _selected_server < 0 or _selected_server >= _servers.size():
		return
	lan_join_requested.emit(JoinTarget.from_lan_row(_servers[_selected_server]))


# --- Host-settings screen (MULTI_PLAYER_HOST) ---------------------------------
# The witnessed populate/filter/selection chain (D-MNU-17):
# [orig: init_host_settings_dialog @0x558960 (rows = title else filename,
#  stock-co-op excluded, GAME_TYPE ALL=255 selected);
#  filter_mission_list_by_game_type @0x556fe0 (show rows whose mapped category
#  matches the spin value or 255, minus the already-selected set);
#  HostDialog_AddRemoveSelectedMissions @0x557c10 (ADD: table row = name /
#  GateTypeAbbrev cell / rotation default, hide from the list; REMOVE:
#  restore); the START_GAME interactive gate on the selected table].
# The engine rules live in npwire game_type.h through the NetProtocol binding.

# The GAME_TYPE spin's ALL-types item value [orig: @0x558aee].
const HOST_FILTER_ALL := 255

# The host pool: catalog rows the host screen may list, parallel to nothing —
# each entry carries its catalog row; the LIST maps row -> pool index through
# _list_pool_rows (retail's row VALUE = mission index).
var _host_pool: Array = []            # Array[MissionCatalogRow]
var _list_pool_rows := PackedInt32Array()  # visible list row -> _host_pool index
var _selected_pool_rows := PackedInt32Array()  # table row -> _host_pool index


func _wire_host_settings() -> void:
	if _driver.has_widget("MISSION_LIST"):
		seed_host_pool(MissionCatalog.rows(_root))
	_connect_pressed("ADD_MISSIONS", _on_add_missions)
	_connect_pressed("REMOVE_MISSIONS", _on_remove_missions)
	_connect_pressed("START_GAME", _on_host_start)
	_sync_start_gate()


## The host screen's available-mission pool (MissionCatalogRow array). The wire
## path hands the resource-dir catalog; tests inject synthetic rows. Stock
## (non-objective) co-op — the pure-SP/training family — never lists
## [orig: the populate skip @0x558a70]; the selected rotation resets.
func seed_host_pool(rows: Array) -> void:
	_host_pool.clear()
	_selected_pool_rows = PackedInt32Array()
	for row in rows:
		if NetProtocol.game_type_host_list_visible(int(row.get_game_type())):
			_host_pool.append(row)
	var table := _id("SELECTED_MISSIONS")
	if table >= 0:
		_driver.table_clear_rows(table)
	var mission_list := _id("MISSION_LIST")
	if mission_list >= 0:
		_seed_mission_list(mission_list)
	_sync_start_gate()


# Rebuild MISSION_LIST from the pool: rows whose mapped game-type category
# matches the GAME_TYPE spin (or ALL), minus the already-selected set; the row
# text is the catalog display (title else filename, the loose "*" carried)
# [orig: the populate @0x558a48 + the filter walk @0x557072].
func _seed_mission_list(id: int) -> void:
	var filter_value := _host_filter_value()
	var rows := PackedStringArray()
	_list_pool_rows = PackedInt32Array()
	for i in range(_host_pool.size()):
		if _selected_pool_rows.has(i):
			continue
		var code := int(_host_pool[i].get_game_type())
		if filter_value != HOST_FILTER_ALL \
				and NetProtocol.game_type_host_filter_category(code) != filter_value:
			continue
		rows.append(String(_host_pool[i].display_text()))
		_list_pool_rows.append(i)
	_driver.set_widget_items(id, rows)
	# The witnessed populate leaves NO selection (the reimpl list preselects
	# row 0 — the same clear the SP seeding applies).
	_driver.select_row(id, -1, false)


func _host_filter_value() -> int:
	var value := _spin_attr("GAME_TYPE", str(HOST_FILTER_ALL))
	return int(value) if value.is_valid_int() else HOST_FILTER_ALL


func _on_add_missions() -> void:
	var mission_list := _id("MISSION_LIST")
	var table := _id("SELECTED_MISSIONS")
	if mission_list < 0 or table < 0:
		return
	# Resolve selections to pool indices first — the reseed below rebuilds the
	# row mapping.
	var picked := PackedInt32Array()
	for idx in _driver.selected_rows(mission_list):
		if idx >= 0 and idx < _list_pool_rows.size():
			picked.append(_list_pool_rows[idx])
	for pool_idx in picked:
		if _selected_pool_rows.has(pool_idx):
			continue
		var row: MissionCatalogRow = _host_pool[pool_idx]
		var code := int(row.get_game_type())
		# cols: Mission / Type (the localized GateTypeAbbrev entry) / Switch
		# (rotation; default on for team games without the objective bit)
		# [orig: the add branch @0x557e27..0x557ef1].
		var rotation := NetProtocol.game_type_host_rotation_default(code)
		_driver.table_add_row(table, PackedStringArray([
			String(row.display_text()),
			_abbreviation_text(code),
			"1" if rotation else "0",
		]))
		_selected_pool_rows.append(pool_idx)
	_seed_mission_list(mission_list)
	_sync_start_gate()


func _on_remove_missions() -> void:
	var table := _id("SELECTED_MISSIONS")
	if table < 0:
		return
	# Remove high index first so lower indices stay valid as rows shift down
	# [orig: the backward walk @0x557c84].
	var rows := Array(_driver.table_selected_rows(table))
	rows.sort()
	rows.reverse()
	for r in rows:
		var row := int(r)
		_driver.table_remove_row(table, row)
		if row >= 0 and row < _selected_pool_rows.size():
			_selected_pool_rows.remove_at(row)
	var mission_list := _id("MISSION_LIST")
	if mission_list >= 0:
		_seed_mission_list(mission_list)
	_sync_start_gate()


# The localized Type cell: gametext GateTypeAbbrev/<key>, the key itself as
# the parser-only fallback [orig: get_game_type_abbreviation @0x520fc0].
func _abbreviation_text(code: int) -> String:
	var key := String(NetProtocol.game_type_host_abbreviation_key(code))
	var t: RtxtStringFile = Strings.get_table("gametext")
	if t != null and t.has_string_in_section("GateTypeAbbrev", key):
		return t.get_string_in_section("GateTypeAbbrev", key)
	return key


# START_GAME is interactive only while the rotation has missions
# [orig: the @0x557f09 tail + the init disable @0x5589f2].
func _sync_start_gate() -> void:
	var start := _id("START_GAME")
	if start < 0:
		return
	_driver.set_widget_disabled(start, _selected_pool_rows.is_empty())


func _on_host_start() -> void:
	if _selected_pool_rows.is_empty():
		return
	lan_host_start_requested.emit(_read_host_config())


# Read the host request off the loaded document by control name. Unread controls
# (rules tab, weapon restrictions, server location) still render. MissionPresentation
# derives the wire game type from the selected mission; the record's Co-op value
# remains the fallback for explicit callers that do not request auto derivation.
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
	# The selected mission metadata owns retail g_GameType. The GAME_TYPE widget
	# filters that mission choice; it is not itself the numeric wire bitfield.
	config.game_type_auto = true
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


# The rotation's mission FILE names, in table order (the table cells carry
# the DISPLAY text; the start config needs the catalog file names)
# [orig: the START walk resolves each table row's mission index @0x556dae].
func _selected_missions() -> Array[String]:
	var out: Array[String] = []
	for pool_idx in _selected_pool_rows:
		out.append(String((_host_pool[pool_idx] as MissionCatalogRow).get_file()))
	return out


# --- Aggregate signal + helpers -----------------------------------------------

func _on_widget_value_changed(widget_name: String, kind: String, index: int, _value: String) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if widget_name == "LAN_GAME_LIST" and kind == "list":
		_selected_server = index
	# The GAME_TYPE spin re-filters the available pool
	# [orig: filter_mission_list_by_game_type @0x556fe0 on the spin event].
	if widget_name == "GAME_TYPE" and kind == "spinlist":
		var mission_list := _id("MISSION_LIST")
		if mission_list >= 0:
			_seed_mission_list(mission_list)


# Returns the selected spin-list item's `value=` attribute (the semantic value the
# original reads), not its localized display label. Used to map SERVERTYPE/GAME_TYPE to behavior.
func _spin_attr(name: String, default_value: String) -> String:
	var id := _id(name)
	if id < 0:
		return default_value
	var value := _driver.spin_value_attr(id)
	return value if not value.is_empty() else default_value
