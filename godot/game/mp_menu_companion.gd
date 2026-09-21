class_name MpMenuCompanion
extends MenuCompanion

# Drives the multiplayer menu (mp.mnu) by control NAME for the LAN co-op path. It is a
# companion the game-agnostic MenuShell (menu_shell.gd) delegates to: when the shell
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
# The host reads the session identity, mission rotation, player cap, and the
# retail spectator controls. LAN search/join call the production LanSession
# discovery seam.

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
	if _lan_session != null \
			and _lan_session.browse_finished.is_connected(_on_lan_browse_finished):
		_lan_session.browse_finished.disconnect(_on_lan_browse_finished)
	_lan_session = session
	if _lan_session != null \
			and not _lan_session.servers_changed.is_connected(_on_servers_changed):
		_lan_session.servers_changed.connect(_on_servers_changed)
	if _lan_session != null \
			and not _lan_session.error_occurred.is_connected(_on_lan_browse_error):
		_lan_session.error_occurred.connect(_on_lan_browse_error)
	if _lan_session != null \
			and not _lan_session.browse_finished.is_connected(_on_lan_browse_finished):
		_lan_session.browse_finished.connect(_on_lan_browse_finished)


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
		# While the 30 s window runs the search button is non-interactive and reads
		# the menutxt MP_STATUS_SEARCHING token; the window's end restores MP_SEARCH.
		_set_lan_search_state(true)
	_refresh_lan_list()


# The browse window closed: LAN_SEARCH is interactive again with its MP_SEARCH text.
func _on_lan_browse_finished() -> void:
	_set_lan_search_state(false)


func _set_lan_search_state(searching: bool) -> void:
	var id := _id("LAN_SEARCH")
	if id < 0:
		return
	_driver.set_widget_disabled(id, searching)
	var key := "MP_STATUS_SEARCHING" if searching else "MP_SEARCH"
	var text := Strings.menu_text(key, "")
	if not text.is_empty():
		_driver.set_widget_text(id, text)


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


func _format_server_row(s: LanServerRow) -> String:
	var name := s.server_name if not s.server_name.is_empty() else "?"
	var cur := s.players
	var max_p := s.max_players
	# Retail LAN enumeration has not joined the session yet, so map identity is
	# deliberately absent here; it arrives in the normal post-auth 0x7B stream.
	# The row format is the witnessed retail pair: with an advertised expansion
	# variant "%s - %s (%ld/%ld)", else "%s (%ld/%ld)". The list walks the
	# translated session record CNapiNetwork_OnSessionDiscovered @0x4c8470 fills
	# from the 0x81 (SN -> +32, NP -> +580, MP -> +584, SUS2 -> +1104), which
	# the row copies 32 bytes of the expansion from.
	# [orig: UI_ProcessLANSessionStateMachine @ 0x558de0 sprintf @0x559493/@0x5594b9]
	var expansion := s.expansion.strip_edges()
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
# Mission rotation and filtering are the native menu host-dialog model.


func _wire_host_settings() -> void:
	_seed_host_controls()
	if _driver.has_widget("MISSION_LIST"):
		seed_host_pool(MissionCatalog.rows(_root))
	_connect_pressed("ADD_MISSIONS", _on_add_missions)
	_connect_pressed("REMOVE_MISSIONS", _on_remove_missions)
	_connect_pressed("START_GAME", _on_host_start)
	_sync_start_gate()


# The host screen opens seeded from the host configuration, the populate step
# the retail dialog init runs before the mission pool: the engine's
# host_dialog_value (HostSessionOptions.dialog_value) is the inverse of the
# START read. Edits take the text, spins select the item whose authored
# value= matches, GAME_LOCATION the item whose name shares the country's
# first three characters, checkboxes the nonzero value. Without a persisted
# game.cfg the seed is the record's defaults, with the session name the
# Menu/UNTITLED gametext the retail config defaults copy in. A user-cleared
# edit is still read verbatim at START.
func _seed_host_controls() -> void:
	var defaults := HostSessionConfig.new()
	defaults.server_name = Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_MENU,
			"UNTITLED", defaults.server_name)
	for control in HostSessionOptions.dialog_controls():
		var id := _id(control)
		if id < 0:
			continue
		var value: String = defaults.dialog_value(control)
		if control == "SERVER_PUNKBUSTER":
			# No pb\pbcl.dll ships with OpenNova; the populate selects 0 and
			# disables the spin whenever that file is absent.
			value = "0"
			_driver.set_widget_disabled(id, true)
		match _driver.widget_kind_of(id):
			MnuDocument.TYPE_SPINLIST:
				if control == "GAME_LOCATION":
					_driver.select_host_location(id, value)
				else:
					_driver.select_row_by_value(id, value, false)
			MnuDocument.TYPE_CHECKBOX:
				_driver.set_widget_checked(id, value != "0")
			_:
				_driver.set_widget_text(id, value)


## The catalog comes from the mounted resource root; tests may supply catalog rows.
func seed_host_pool(rows: Array[MissionCatalogRow]) -> void:
	_driver.seed_host_pool(rows)


func _on_add_missions() -> void:
	_driver.add_host_missions(Strings.get_table(Strings.TABLE_GAMETEXT))


func _on_remove_missions() -> void:
	_driver.remove_host_missions()


func _sync_start_gate() -> void:
	var start := _id("START_GAME")
	if start >= 0:
		_driver.set_widget_disabled(start, not _driver.can_start_host())


func _on_host_start() -> void:
	if not _driver.can_start_host():
		return
	lan_host_start_requested.emit(_read_host_config())


# Read the host request off the loaded document by control name. Unread controls
# (weapon restrictions) still render. MissionRoot
# derives the wire game type from the selected mission; the record's Co-op value
# remains the fallback for explicit callers that do not request auto derivation.
func _read_host_config() -> HostSessionConfig:
	var config := HostSessionConfig.new()
	for control in HostSessionOptions.dialog_controls():
		var id := _id(control)
		if id < 0:
			continue
		var value := _driver.get_widget_text(id)
		match _driver.widget_kind_of(id):
			MnuDocument.TYPE_SPINLIST:
				value = _driver.item_text(id, _driver.selected_row(id)) \
						if control == "GAME_LOCATION" else _driver.spin_value_attr(id)
			MnuDocument.TYPE_CHECKBOX:
				value = "1" if _driver.is_widget_checked(id) else "0"
		config.apply_dialog_control(control, value)
	config.missions.assign(_driver.selected_host_missions())
	if config.missions.size() > 0:
		config.mission = config.missions[0]
	config.game_type_attr = _spin_attr("GAME_TYPE", "")
	# The selected mission metadata owns retail g_GameType. The GAME_TYPE widget
	# filters that mission choice; it is not itself the numeric wire bitfield.
	config.game_type_auto = true
	# Retail's game-name field is the expansion currently mounted by the game,
	# and is empty for the base game. Never substitute a captured expansion id.
	config.expansion = _root.get_expansion() if _root != null else ""
	return config


# --- Aggregate signal + helpers -----------------------------------------------

func _on_widget_value_changed(widget_name: String, kind: String, index: int, _value: String) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if widget_name == "LAN_GAME_LIST" and kind == "list":
		_selected_server = index
		# A single click on the list makes LAN_JOINGAME interactive; the double
		# click joins.
		var join := _id("LAN_JOINGAME")
		if join >= 0:
			_driver.set_widget_disabled(join, false)
	if widget_name == "GAME_TYPE" and kind == "spinlist":
		_driver.filter_host_missions()


# Returns the selected spin-list item's `value=` attribute (the semantic value the
# original reads), not its localized display label. Used to map SERVERTYPE/GAME_TYPE to behavior.
func _spin_attr(name: String, default_value: String) -> String:
	var id := _id(name)
	if id < 0:
		return default_value
	var value := _driver.spin_value_attr(id)
	return value if not value.is_empty() else default_value
