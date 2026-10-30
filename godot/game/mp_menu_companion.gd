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
# The player chose to join a LAN row whose index names no listed session: the join
# machine refuses it before anything is sent, with gameerr "MP Errors" CVUNABLETOCREATE
# (`text`) on the join screen.
signal lan_join_refused(text: String)
# START_GAME on the host screen, with the co-op-minimal host request (see _read_host_config).
signal lan_host_start_requested(config: HostSessionConfig)
signal novaworld_host_start_requested(config: HostSessionConfig)
signal novaworld_host_cancelled
var host_channel := HostSessionConfig.CHANNEL_LAN
## The host screen's radios that set every weapon or class word, by name.
const HOST_RESTRICTION_RADIOS := ["WPN_ALWAYS", "WPN_NEVER", "WPN_ARMORY", "WPN_MISSION",
	"CLASS_ALWAYS", "CLASS_NEVER", "CLASS_MISSION"]

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
	if not _driver.screen_changed.is_connected(_on_screen_changed):
		_driver.screen_changed.connect(_on_screen_changed)
	_wire_lan_browser()
	_wire_host_settings()
	# The document opened on its screen: that screen's activation.
	_on_screen_changed(_screen)


# A screen's activation. The LAN screen's empties its game list and drains the
# enumerator's session records, each writing its removed line to the process's
# _connectlog.txt (engine: net/npwire/lan_discovery.h drain_records carries the
# witness, D-NET-956).
func _on_screen_changed(screen_name: String) -> void:
	if host_channel == HostSessionConfig.CHANNEL_NOVAWORLD \
			and screen_name.nocasecmp_to(GameCfgStore.block().host_screen()) != 0:
		host_channel = HostSessionConfig.CHANNEL_LAN
		novaworld_host_cancelled.emit()
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if screen_name.nocasecmp_to("LAN_MULTI_PLAYER") != 0 or not _driver.has_widget("LAN_GAME_LIST"):
		return
	# The LAN screen's activation writes the block's network connect type 2 (engine
	# inmatch/game_config.h GameConfig::network_connect_type carries the witness).
	GameCfgStore.block().set_word("networkconnecttype", 2)
	_servers = []
	_selected_server = -1
	_browse_error = ""
	_refresh_lan_list()
	if _lan_session != null:
		_lan_session.drain_records()
	# The activation fills the host screen's WEAPON_LIST again from the block (engine
	# inmatch/host_settings.h populate_host_weapon_list).
	GameCfgStore.block().populate_host_weapon_list(_driver, Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_GAMEUI))


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
	# Within a browse window a newly discovered host is appended to the list and
	# the player's highlighted row stays selected; only a new search (which
	# empties the list) or a snapshot that no longer holds the selected endpoint
	# at its row drops the selection (the LAN state machine adds rows with
	# UIList_AddRow and LAN_SEARCH clears the list first).
	var keep := _selected_server >= 0 and _selected_server < _servers.size() \
			and _selected_server < servers.size() \
			and _same_endpoint(_servers[_selected_server], servers[_selected_server])
	_servers = servers
	if not keep:
		_selected_server = -1
	if not servers.is_empty():
		_browse_error = ""
	_refresh_lan_list()
	if keep:
		var id := _id("LAN_GAME_LIST")
		if id >= 0:
			_driver.select_row(id, _selected_server, false)


static func _same_endpoint(a: LanServerRow, b: LanServerRow) -> bool:
	return a != null and b != null and a.host_ip == b.host_ip and a.port == b.port


func _refresh_lan_list() -> void:
	var id := _id("LAN_GAME_LIST")
	if id < 0:
		return
	var rows := PackedStringArray()
	for s: LanServerRow in _servers:
		# Map identity is deliberately absent: retail LAN enumeration has not
		# joined the session yet, and it arrives in the post-auth 0x7B stream.
		# The row text is the engine's (net/npwire/lan_discovery.h
		# lan_discovery_row_label carries the witness).
		rows.append(s.list_label())
	# Surface a search failure in the list itself (the row is inert: the join
	# guard checks against _servers, which stays empty).
	if rows.is_empty() and not _browse_error.is_empty():
		rows.append("Search failed - %s" % _browse_error)
	_driver.set_widget_items(id, rows)


func _on_list_activated(id: int, row: int) -> void:
	if _driver == null or _driver.get_menu_file() != _wired_file:
		return
	if _driver.widget_name_of(id).nocasecmp_to("LAN_GAME_LIST") == 0:
		_selected_server = row
		_on_lan_join()


func _on_lan_join() -> void:
	if _selected_server < 0 or _selected_server >= _servers.size():
		return
	# The join takes the listed session at the selected row's index, which after a
	# replaced session is not the row's own, and is refused when the index names none
	# (engine: net/npwire/lan_discovery.h join_session carries the witness). A
	# session-less companion (the menu seam's harness) joins the row as listed.
	var row: LanServerRow = _servers[_selected_server]
	if _lan_session != null:
		# The press hands the frame to the join machine: the LAN screen's browse pumps no
		# more, its records and socket staying (engine: LanDiscoveryBrowser::stop carries
		# the witness), so a refused join's screen sends and reads nothing.
		_lan_session.stop_enumerator()
		row = _lan_session.get_join_row(_selected_server)
	if row == null:
		lan_join_refused.emit(Strings.lookup(Strings.TABLE_GAMEERR, "MP Errors", "CVUNABLETOCREATE"))
		return
	var target := JoinTarget.from_lan_row(row)
	# The join goes on with the search's socket (D-NET-1019), which stays open past
	# the window's end.
	if _lan_session != null:
		target.transport = _lan_session.take_transport()
	lan_join_requested.emit(target)


# --- Host-settings screen (the title's host screen: MULTI_PLAYER_HOST, DFX2's NW_MULTI_PLAYER) ---
# Mission rotation and filtering are the native menu host-dialog model.


func _wire_host_settings() -> void:
	_seed_host_controls()
	if _driver.has_widget("MISSION_LIST"):
		seed_host_pool(MissionCatalog.rows(_root))
	_connect_pressed("ADD_MISSIONS", _on_add_missions)
	_connect_pressed("REMOVE_MISSIONS", _on_remove_missions)
	_connect_pressed("START_GAME", _on_host_start)
	for control in HOST_RESTRICTION_RADIOS:
		_connect_pressed(control, _on_host_restriction_radio.bind(control))
	if not _driver.table_cell_clicked.is_connected(_on_host_table_cell_clicked):
		_driver.table_cell_clicked.connect(_on_host_table_cell_clicked)
	_sync_start_gate()


## The SELECTED_MISSIONS Switch cell (column 2): a click toggles the row's launch
## option, the Attack-and-Defend halves (engine menu::HostDialog::toggle_switch).
## The restriction lists' cell (column 1): a press steps the row's weapon or class
## word (engine inmatch/host_settings.h host_restriction_list_pressed).
func _on_host_table_cell_clicked(_id: int, widget_name: String, row: int, column: int,
		_state: int, _cell_value: int, _double_click: bool) -> void:
	if widget_name == "SELECTED_MISSIONS" and column == 2:
		_driver.toggle_host_mission_switch(row)
	elif GameCfgStore.block().host_restriction_list_pressed(_driver, widget_name, row, column,
			Strings.get_table(Strings.TABLE_GAMEUI)):
		_sync_start_gate()


## A radio that sets every weapon or class word (engine inmatch/host_settings.h
## host_restriction_radio_pressed).
func _on_host_restriction_radio(control: String) -> void:
	if GameCfgStore.block().host_restriction_radio_pressed(_driver, control,
			Strings.get_table(Strings.TABLE_GAMEUI)):
		_sync_start_gate()


# The host screen opens showing the process's cfg block, the populate step the
# retail dialog init runs before the mission pool (GameConfigBlock over the
# engine's populate_host_screen, which carries the witness: the cap clamped to
# the screen's, each control from the block, PunkBuster 0 and disabled with no
# pb\pbcl.dll). A user-cleared edit is still read verbatim at START.
# Then the restriction lists, which the init fills after its populate (engine
# inmatch/host_settings.h populate_host_weapon_list / populate_host_class_list).
func _seed_host_controls() -> void:
	GameCfgStore.block().populate_host_screen(_driver)
	GameCfgStore.block().populate_host_restrictions(_driver, Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_GAMEUI))


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


# START: the screen read into the process's cfg block, the block saved, and the
# session request made of the block, as retail's LAN start reads the dialog,
# saves game.cfg and creates the session from the block (the witness is
# engine runtime/inmatch/host_settings.h).
func _on_host_start() -> void:
	if not _driver.can_start_host():
		return
	var config := _read_host_config()
	config.channel = host_channel
	if host_channel == HostSessionConfig.CHANNEL_NOVAWORLD:
		novaworld_host_start_requested.emit(config)
		return
	GameCfgStore.save()
	lan_host_start_requested.emit(config)


# Read the screen's controls by name into the cfg block (HostDialog_ReadSettings,
# the engine's read_host_screen), then the host request from the block.
# Unread controls (weapon restrictions) still render. MissionRoot derives the
# wire game type from the selected mission; the record's Co-op value remains the
# fallback for explicit callers that do not request auto derivation.
func _read_host_config() -> HostSessionConfig:
	GameCfgStore.block().read_host_screen(_driver)
	var config := HostSessionConfig.new()
	config.missions.assign(_driver.selected_host_missions())
	config.mission_launch_options.assign(_driver.selected_host_launch_options())
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
