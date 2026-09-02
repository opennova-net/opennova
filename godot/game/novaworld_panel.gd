class_name NovaWorldPanel
extends Control

# The NovaWorld (online multiplayer) front-end. Opened from the menu when the
# player chooses NovaWorld. It owns one NovaWorldClient (the GDExtension pump
# over engine/net/novaworld) and turns its protocol state into artist-facing status,
# a server browser, and a Host-a-Game action. The panel never touches the wire
# itself; the client does.
#
# The browser is a sortable multi-column table over the full GSB row set with a
# filter bar, a selected-server details pane (message, mod, version, locale,
# the live player roster), and the browse-time ping column (the client's sweep
# mirrors retail's list-finalize ping; engine/net/novaworld/ping_sweep.h).
#
# Milestone flow: open -> the client probes the gate and runs the session
# handshake against our server -> on a verified session the browser fills from
# the server list -> Host a Game registers a row other clients can see.

# Where to reach the server. Dev default is localhost (matching the launcher's
# dev mode and the dev compose). Prod sets this from the resolved server IP.
@export var server_host := "127.0.0.1"
@export var gate_port := NovaWorldSettings.GATE_PORT
@export var player_name := "Player"
@export var start_client_on_ready := true
@export var target_override := -1

signal closed()
# The NWJoin handshake resolved the in-match host:port — enter the match as a JOINER. The arg is
# the typed dial target MainGame hands to GameWorld.load_mission_as_joiner
# (the SAME entry the LAN browser + --lan-join launch use — one in-match joiner seam, ADR 0009).
signal join_in_match_requested(target: JoinTarget)
# Host a NovaWorld game. The panel supplies the gate (the server it's connected to); MainGame fills in
# the mission + callsign and stands up a browsable listen host (net_session_drive._maybe_start_nw_host).
signal host_requested(config: HostSessionConfig)

# The table's column order. PLAYERS and PING sort numerically; the rest by text.
enum Column { NAME, MISSION, TYPE, PLAYERS, PING, ACCESS }
const COLUMN_TITLES: PackedStringArray = [
	"Server", "Mission", "Mode", "Players", "Ping", "Access"]

enum Screen { CONNECTING, LOGIN, LOBBY, HOST, MESSAGE }

var _client: NovaWorldClient  # created when the panel opens; torn down on close
var _status_label: Label
var _server_tree: Tree
var _host_button: Button
var _join_button: Button
var _close_button: Button
var _refresh_button: Button
var _target_option: OptionButton
var _username_edit: LineEdit
var _password_edit: LineEdit
var _login_button: Button
var _filter_edit: LineEdit
var _type_filter: OptionButton
var _hide_full_check: CheckBox
var _hide_empty_check: CheckBox
var _hide_locked_check: CheckBox
var _details_label: Label
var _roster_list: ItemList
var _target: int = NovaWorldSettings.Target.OPENNOVA
var _rows: Array = []          # the full GSB row set (Array of Dictionary)
var _view: Array = []          # the filtered + sorted rows the table shows
var _pings: Dictionary = {}    # rid -> ping ms / -2 failed / -3 never (absent = in flight)
var _sort_column: int = Column.NAME
var _sort_ascending := true
var _can_login := false        # true once the gate reply gives us a startup_url
var _logged_in := false
# Stashed at join time: the selected row's mission + our callsign. joined_game carries the
# resolved address plus the APPID join token and CD identity cookie, so we remember the mission
# (the joiner must know the host's mission to load it) and pair them when the join resolves.
var _pending_mission := ""
var _pending_player := ""
# The signed-in NovaWorld handle — the callsign every NW join uses (retail
# parity: the service identity names the player, not the local profile).
var _nw_callsign := ""
# The rid whose first Join press drew the expansion warning; a second press on
# the same row proceeds (the in-match 0x7B reconcile stays authoritative). The
# rid is retail's signed %d splice and can legitimately be negative, so the
# armed state is its own flag rather than a sentinel value.
var _exp_warning_armed := false
var _exp_warning_armed_rid := 0
# The mounted resource root, set by MainGame BEFORE _ready so the host Map picker can list the
# install's .bms missions (the panel owns no mission list; the world's root is null until a load).
var resource_root: ResourceRoot
var _mission_option: OptionButton
var _connecting_view: Control
var _login_view: Control
var _lobby_view: Control
var _host_view: Control
var _message_view: Control
var _message_label: Label
var _message_button: Button
var _population_label: Label
var _empty_label: Label
var _host_start_button: Button
var _host_cancel_button: Button
var _screen := Screen.CONNECTING
var _message_return_screen := Screen.LOGIN
var _message_action: Callable
var _browser_loading := false
var _closing := false
var _suppress_client_messages := false


func _ready() -> void:
	_target = target_override if target_override >= 0 else NovaWorldSettings.load_target()
	_bind_scene_ui()
	_set_screen(Screen.CONNECTING)
	_create_client(start_client_on_ready)


func _bind_scene_ui() -> void:
	var base := "Center/Shell/Margin/RootVBox/"
	_target_option = get_node(base + "Header/TargetOption")
	_close_button = get_node(base + "Header/BackButton")
	_status_label = get_node(base + "StatusLabel")
	var pages := base + "Pages/"
	_connecting_view = get_node(pages + "ConnectingView")
	_login_view = get_node(pages + "LoginView")
	_lobby_view = get_node(pages + "LobbyView")
	_host_view = get_node(pages + "HostView")
	_message_view = get_node(pages + "MessageView")
	_username_edit = get_node(pages + "LoginView/LoginCard/LoginMargin/LoginForm/UsernameEdit")
	_password_edit = get_node(pages + "LoginView/LoginCard/LoginMargin/LoginForm/PasswordEdit")
	_login_button = get_node(pages + "LoginView/LoginCard/LoginMargin/LoginForm/LoginButton")
	_filter_edit = get_node(pages + "LobbyView/Toolbar/SearchEdit")
	_type_filter = get_node(pages + "LobbyView/Toolbar/TypeFilter")
	_refresh_button = get_node(pages + "LobbyView/Toolbar/RefreshButton")
	_hide_full_check = get_node(pages + "LobbyView/QuickFilters/NotFullCheck")
	_hide_empty_check = get_node(pages + "LobbyView/QuickFilters/HasPlayersCheck")
	_hide_locked_check = get_node(pages + "LobbyView/QuickFilters/OpenOnlyCheck")
	_population_label = get_node(pages + "LobbyView/PopulationLabel")
	_empty_label = get_node(pages + "LobbyView/BrowserStack/EmptyLabel")
	_server_tree = get_node(pages + "LobbyView/BrowserStack/BrowserSplit/ServerTree")
	_details_label = get_node(pages + "LobbyView/BrowserStack/BrowserSplit/DetailsPanel/DetailsMargin/DetailsScroll/DetailsVBox/DetailsLabel")
	_roster_list = get_node(pages + "LobbyView/BrowserStack/BrowserSplit/DetailsPanel/DetailsMargin/DetailsScroll/DetailsVBox/RosterList")
	_host_button = get_node(pages + "LobbyView/Actions/HostButton")
	_join_button = get_node(pages + "LobbyView/Actions/JoinButton")
	_mission_option = get_node(pages + "HostView/HostCard/HostMargin/HostForm/MissionOption")
	_host_start_button = get_node(pages + "HostView/HostCard/HostMargin/HostForm/HostActions/StartButton")
	_host_cancel_button = get_node(pages + "HostView/HostCard/HostMargin/HostForm/HostActions/CancelButton")
	_message_label = get_node(pages + "MessageView/MessageCard/MessageMargin/MessageForm/MessageLabel")
	_message_button = get_node(pages + "MessageView/MessageCard/MessageMargin/MessageForm/MessageButton")

	_target_option.clear()
	_target_option.add_item("OpenNova", NovaWorldSettings.Target.OPENNOVA)
	_target_option.add_item("Original NovaWorld", NovaWorldSettings.Target.REAL)
	_target_option.select(_target_option.get_item_index(_target))
	_target_option.item_selected.connect(_on_target_selected)
	_close_button.pressed.connect(_on_close_pressed)
	_login_button.pressed.connect(_on_login_pressed)
	_password_edit.text_submitted.connect(func(_text: String) -> void: _on_login_pressed())
	_filter_edit.text_changed.connect(func(_text: String) -> void: _rebuild_view())
	_type_filter.clear()
	_type_filter.add_item("All modes")
	_type_filter.item_selected.connect(func(_index: int) -> void: _rebuild_view())
	_hide_full_check.toggled.connect(func(_on: bool) -> void: _rebuild_view())
	_hide_empty_check.toggled.connect(func(_on: bool) -> void: _rebuild_view())
	_hide_locked_check.toggled.connect(func(_on: bool) -> void: _rebuild_view())
	_refresh_button.pressed.connect(_on_refresh_pressed)
	_host_button.pressed.connect(_on_host_screen_pressed)
	_join_button.pressed.connect(_on_join_pressed)
	_host_start_button.pressed.connect(_on_host_pressed)
	_host_cancel_button.pressed.connect(_on_host_cancel_pressed)
	_message_button.pressed.connect(_on_message_action_pressed)

	_server_tree.hide_root = true
	_server_tree.select_mode = Tree.SELECT_ROW
	_server_tree.columns = COLUMN_TITLES.size()
	_server_tree.column_titles_visible = true
	for column in COLUMN_TITLES.size():
		_server_tree.set_column_expand(column, column == Column.NAME or column == Column.MISSION)
	_server_tree.set_column_custom_minimum_width(Column.TYPE, 90)
	_server_tree.set_column_custom_minimum_width(Column.PLAYERS, 76)
	_server_tree.set_column_custom_minimum_width(Column.PING, 62)
	_server_tree.set_column_custom_minimum_width(Column.ACCESS, 86)
	_server_tree.column_title_clicked.connect(_on_column_title_clicked)
	_server_tree.item_selected.connect(_on_server_selected)
	_server_tree.item_activated.connect(_on_join_pressed)
	_update_column_titles()
	_populate_missions()


func _set_screen(next: int) -> void:
	if next == Screen.LOBBY and not _logged_in:
		next = Screen.LOGIN
	_screen = next
	if _connecting_view != null:
		_connecting_view.visible = next == Screen.CONNECTING
		_login_view.visible = next == Screen.LOGIN
		_lobby_view.visible = next == Screen.LOBBY
		_host_view.visible = next == Screen.HOST
		_message_view.visible = next == Screen.MESSAGE


func _show_message(text: String, button_text: String, action: Callable,
		return_screen: int) -> void:
	_message_return_screen = return_screen
	_message_action = action
	if _message_label != null:
		_message_label.text = text.strip_edges() if not text.strip_edges().is_empty() \
				else "Could not complete the request."
	if _message_button != null:
		_message_button.text = button_text
	_set_screen(Screen.MESSAGE)


func _on_message_action_pressed() -> void:
	var action := _message_action
	_message_action = Callable()
	if action.is_valid():
		action.call()
	else:
		_set_screen(_message_return_screen)


func _return_to_login() -> void:
	_set_status("Connected. Sign in to continue.")
	_set_screen(Screen.LOGIN)
	if _username_edit != null:
		_username_edit.grab_focus()


func _return_to_lobby() -> void:
	_set_screen(Screen.LOBBY)


func _return_to_host() -> void:
	_set_screen(Screen.HOST)


func _retry_server_list() -> void:
	_set_screen(Screen.LOBBY)
	_on_refresh_pressed()


func _on_host_screen_pressed() -> void:
	if not _logged_in:
		return
	_populate_missions()
	_set_screen(Screen.HOST)


func _on_host_cancel_pressed() -> void:
	_set_screen(Screen.LOBBY)


func _update_column_titles() -> void:
	if _server_tree == null:
		return
	for column in COLUMN_TITLES.size():
		var suffix := ""
		if column == _sort_column:
			suffix = "  ^" if _sort_ascending else "  v"
		_server_tree.set_column_title(column, COLUMN_TITLES[column] + suffix)


func _create_client(start_now: bool = true) -> void:
	_client = NovaWorldClient.new()
	add_child(_client)
	_client.host = _resolved_host()
	_client.gate_port = gate_port
	_client.player_name = player_name
	_client.state_changed.connect(_on_state_changed)
	_client.connected.connect(_on_connected)
	_client.disconnected.connect(_on_disconnected)
	_client.error_occurred.connect(_on_error)
	_client.server_list_updated.connect(_on_server_list_updated)
	_client.server_list_failed.connect(_on_server_list_failed)
	_client.server_pings_updated.connect(_on_server_pings_updated)
	_client.login_succeeded.connect(_on_login_succeeded)
	_client.login_failed.connect(_on_login_failed)
	_client.join_failed.connect(_on_join_failed)
	_client.joined_game.connect(_on_joined_game)
	if start_now:
		_client.start()


# OpenNova uses the configured/injected server_host (dev: localhost). "Original
# NovaWorld" points the same client at NovaLogic's live gate (gs.novaworld.net).
func _resolved_host() -> String:
	if _target == NovaWorldSettings.Target.REAL:
		return NovaWorldSettings.REAL_NOVAWORLD_HOST
	return server_host


func _on_target_selected(index: int) -> void:
	_target = _target_option.get_item_id(index)
	NovaWorldSettings.save_target(_target)
	_reconnect()


func _reconnect() -> void:
	_suppress_client_messages = true
	if _client != null:
		_client.stop()
		_client.queue_free()
		_client = null
	_suppress_client_messages = false
	if _host_button != null:
		_host_button.disabled = true
	if _join_button != null:
		_join_button.disabled = true
	if _login_button != null:
		_login_button.disabled = true
	_can_login = false
	_logged_in = false
	_nw_callsign = ""
	_rows.clear()
	_view.clear()
	_pings.clear()
	_browser_loading = false
	if _password_edit != null:
		_password_edit.clear()
	_set_status("Connecting to matchmaking...")
	_set_screen(Screen.CONNECTING)
	_create_client()


func _set_status(text: String) -> void:
	if _status_label != null:
		_status_label.text = text


# Map the protocol state to plain language (no wire jargon for the player).
func _on_state_changed(state: int) -> void:
	match state:
		NovaWorldClient.STATE_IDLE:
			_set_status("Ready.")
		NovaWorldClient.STATE_GATE_PROBING:
			_set_status("Finding the server...")
		NovaWorldClient.STATE_SESSION_HELLO, NovaWorldClient.STATE_SESSION_JOIN:
			_set_status("Connecting...")
		NovaWorldClient.STATE_CONNECTED:
			_set_status("Connected.")
		NovaWorldClient.STATE_DISCONNECTED:
			_set_status("Disconnected.")
		NovaWorldClient.STATE_ERROR:
			_set_status("Connection problem.")
		NovaWorldClient.STATE_JOINING:
			_set_status("Joining game...")       # NWJoin in flight
		NovaWorldClient.STATE_IN_GAME_HELLO:
			_set_status("Connecting to game host...")  # proto switched
		_:
			_set_status("Connection problem.")


func _on_connected() -> void:
	if _logged_in:
		return # returning from a failed join; the typed failure owns the next screen
	_can_login = true
	_login_button.disabled = false
	_set_status("Connected. Sign in to continue.")
	_set_screen(Screen.LOGIN)
	_username_edit.grab_focus()


func _on_disconnected(reason: String) -> void:
	if _suppress_client_messages or _closing:
		return
	_host_button.disabled = true
	_logged_in = false
	_show_message("The connection to the matchmaking service was closed.\n\n%s" % reason,
			"Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


func _on_error(message: String) -> void:
	if _suppress_client_messages or _closing:
		return
	_host_button.disabled = true
	_logged_in = false
	_show_message("The matchmaking service could not be reached.\n\n%s" % message,
			"Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


# Fill the browser from the GSB server list. Rows arrive asynchronously over
# HTTP once the session is verified; the client re-emits server_list_updated
# whenever it refetches, and connecting/refreshing both call through here.
func _refresh_servers() -> void:
	_rows = []
	_pings = {}
	_exp_warning_armed = false
	if _client != null:
		for row in _client.get_server_rows():
			_rows.append(row)
		_pings = _client.get_server_pings()
	_rebuild_type_filter()
	_rebuild_view()
	_show_population_status()


# The service-wide population line ("58 servers, 214 players online") once the
# list carries totals.
func _show_population_status() -> void:
	if _client == null:
		return
	var totals: Dictionary = _client.get_server_totals()
	var servers := int(totals.get("total_servers", 0))
	var players := int(totals.get("total_players", 0))
	if _population_label != null:
		_population_label.text = "%d game%s • %d player%s online" % [
			servers, "" if servers == 1 else "s",
			players, "" if players == 1 else "s"]


# The Type picker offers the game types present in the list (plus All).
func _rebuild_type_filter() -> void:
	if _type_filter == null:
		return
	var previous := ""
	if _type_filter.selected > 0:
		previous = _type_filter.get_item_text(_type_filter.selected)
	_type_filter.clear()
	_type_filter.add_item("All modes")
	var seen := {}
	for row in _rows:
		var t := String((row as Dictionary).get("game_type", "")).strip_edges()
		if t.is_empty() or seen.has(t.to_lower()):
			continue
		seen[t.to_lower()] = true
		_type_filter.add_item(t)
		if t == previous:
			_type_filter.select(_type_filter.item_count - 1)


# The filter bar's current state as the filter_rows() dictionary.
func _filter_state() -> Dictionary:
	var type_text := ""
	if _type_filter != null and _type_filter.selected > 0:
		type_text = _type_filter.get_item_text(_type_filter.selected)
	return {
		"text": _filter_edit.text if _filter_edit != null else "",
		"game_type": type_text,
		"hide_full": _hide_full_check != null and _hide_full_check.button_pressed,
		"hide_empty": _hide_empty_check != null and _hide_empty_check.button_pressed,
		"hide_locked": _hide_locked_check != null and _hide_locked_check.button_pressed,
	}


# Re-derive the visible table from the full row set: filter, sort, repopulate,
# and keep the selection on the same rid when it survives the rebuild.
func _rebuild_view() -> void:
	if _server_tree == null:
		return
	var selected_rid := -1
	var selected_row := _selected_row()
	if not selected_row.is_empty():
		selected_rid = int(selected_row.get("rid", -1))
	_view = sort_rows(filter_rows(_rows, _filter_state()),
			_sort_column, _sort_ascending, _pings)
	_server_tree.clear()
	var root := _server_tree.create_item()
	var reselected := false
	var first_item: TreeItem
	for row in _view:
		var item := _server_tree.create_item(root)
		if first_item == null:
			first_item = item
		var cells := row_cells(row, _ping_for(row))
		for c in cells.size():
			item.set_text(c, cells[c])
		item.set_tooltip_text(Column.NAME, server_row_tooltip(row))
		item.set_metadata(0, row)
		if int((row as Dictionary).get("rid", -2)) == selected_rid:
			item.select(Column.NAME)
			reselected = true
	if _empty_label != null:
		_empty_label.visible = _view.is_empty()
		_empty_label.text = "Retrieving games..." if _browser_loading else (
				"No games are being hosted right now." if _rows.is_empty()
				else "No games match these filters.")
	_server_tree.visible = not _view.is_empty()
	if not reselected and first_item != null:
		first_item.select(Column.NAME)
		_join_button.disabled = false
		_show_details(first_item.get_metadata(0))
	elif not reselected:
		_join_button.disabled = true
		_show_details({})
	_update_column_titles()


func _ping_for(row: Dictionary):
	return _pings.get(int(row.get("rid", -1)))


func _on_column_title_clicked(column: int, _mouse_button_index: int) -> void:
	if column == _sort_column:
		_sort_ascending = not _sort_ascending
	else:
		_sort_column = column
		_sort_ascending = true
	_rebuild_view()


func _on_refresh_pressed() -> void:
	if _client == null or not _logged_in:
		return
	_browser_loading = true
	_set_status("Refreshing games...")
	_rebuild_view()
	_client.refresh_servers()


# A ping pass landed: refresh the ping cells in place (a ping-sorted view
# re-sorts instead).
func _on_server_pings_updated() -> void:
	if _client == null:
		return
	_pings = _client.get_server_pings()
	if _sort_column == Column.PING:
		_rebuild_view()
		return
	if _server_tree == null or _server_tree.get_root() == null:
		return
	var item := _server_tree.get_root().get_first_child()
	while item != null:
		var row = item.get_metadata(0)
		if row is Dictionary:
			item.set_text(Column.PING, ping_text(_ping_for(row)))
		item = item.get_next()


# --- The pure row helpers (tests drive these on literal dictionaries) --------

## True when the row advertises a password or a lock.
static func row_is_locked(row: Dictionary) -> bool:
	return String(row.get("password", "N")) == "Y" \
			or String(row.get("locked", "N")) == "Y"


## The ping cell's text: in flight (null) shows "...", the sweep's failed (-2)
## and never-attempted (-3) codes show "N/A", a round-trip shows milliseconds.
static func ping_text(ping) -> String:
	if ping == null:
		return "..."
	var value := int(ping)
	if value < 0:
		return "N/A"
	return str(value)


## One table row's cells, in Column order.
static func row_cells(row: Dictionary, ping) -> PackedStringArray:
	return PackedStringArray([
		String(row.get("name", "server")),
		String(row.get("mission_name", "")),
		String(row.get("game_type", "")),
		"%d/%d" % [int(row.get("players", 0)), int(row.get("max_players", 0))],
		ping_text(ping),
		"Password" if row_is_locked(row) else "Open",
	])


## The filter pass. `filters` keys (all optional): `text` — case-insensitive
## substring over name/map/mod; `game_type` — exact type ("" = all);
## `hide_full` / `hide_empty` / `hide_locked` — the quick filters.
static func filter_rows(rows: Array, filters: Dictionary) -> Array:
	var text := String(filters.get("text", "")).strip_edges().to_lower()
	var game_type := String(filters.get("game_type", "")).strip_edges()
	var hide_full := bool(filters.get("hide_full", false))
	var hide_empty := bool(filters.get("hide_empty", false))
	var hide_locked := bool(filters.get("hide_locked", false))
	var out: Array = []
	for entry in rows:
		var row := entry as Dictionary
		if not text.is_empty():
			var haystack := "%s\n%s\n%s" % [String(row.get("name", "")),
					String(row.get("mission_name", "")), String(row.get("mod", ""))]
			if not haystack.to_lower().contains(text):
				continue
		if not game_type.is_empty() \
				and String(row.get("game_type", "")).nocasecmp_to(game_type) != 0:
			continue
		var players := int(row.get("players", 0))
		if hide_full and players >= int(row.get("max_players", 0)):
			continue
		if hide_empty and players <= 0:
			continue
		if hide_locked and row_is_locked(row):
			continue
		out.append(row)
	return out


## The sort pass: PLAYERS and PING compare numerically (an unmeasured/failed
## ping always sorts last, either direction), everything else compares
## case-insensitively with the server name as the tiebreak.
static func sort_rows(rows: Array, column: int, ascending: bool,
		pings: Dictionary) -> Array:
	var out := rows.duplicate()
	var direction := 1 if ascending else -1
	out.sort_custom(func(a, b) -> bool:
		var ra := a as Dictionary
		var rb := b as Dictionary
		var cmp := 0
		match column:
			Column.PLAYERS:
				cmp = signi(int(ra.get("players", 0)) - int(rb.get("players", 0)))
			Column.PING:
				var pa = pings.get(int(ra.get("rid", -1)))
				var pb = pings.get(int(rb.get("rid", -1)))
				var va := int(pa) if pa != null and int(pa) >= 0 else 0x7FFFFFFF
				var vb := int(pb) if pb != null and int(pb) >= 0 else 0x7FFFFFFF
				if va == 0x7FFFFFFF and vb == 0x7FFFFFFF:
					cmp = 0
				elif va == 0x7FFFFFFF or vb == 0x7FFFFFFF:
					# Unmeasured sorts last regardless of direction.
					return vb == 0x7FFFFFFF
				else:
					cmp = signi(va - vb)
			Column.ACCESS:
				cmp = signi(int(row_is_locked(ra)) - int(row_is_locked(rb)))
			_:
				var key := "name"
				match column:
					Column.MISSION: key = "mission_name"
					Column.TYPE: key = "game_type"
				cmp = String(ra.get(key, "")).nocasecmp_to(String(rb.get(key, "")))
		if cmp == 0:
			cmp = String(ra.get("name", "")).nocasecmp_to(String(rb.get("name", "")))
		return cmp * direction < 0)
	return out


## The details pane's labeled lines for one row (empty fields are skipped).
static func server_details_lines(row: Dictionary) -> PackedStringArray:
	# A plain Array so the lambda appends through the captured reference
	# (PackedStringArray is a value type and would capture as a copy).
	var lines: Array = []
	var push := func(label: String, value: String) -> void:
		if not value.strip_edges().is_empty():
			lines.append("%s: %s" % [label, value])
	push.call("Server", String(row.get("name", "")))
	push.call("Message", String(row.get("msg", "")))
	push.call("Map", String(row.get("mission_name", "")))
	push.call("Type", String(row.get("game_type", "")))
	lines.append("Players: %d/%d" % [
		int(row.get("players", 0)), int(row.get("max_players", 0))])
	push.call("Mod", String(row.get("mod", "")))
	push.call("Version", String(row.get("ver1", "")))
	push.call("Expansion", String(row.get("exp", "")))
	push.call("Region", String(row.get("region", "")))
	push.call("Country", String(row.get("country", "")))
	push.call("Time of day", String(row.get("time_of_day", "")))
	push.call("Time left", String(row.get("time_left", "")))
	push.call("Level range", String(row.get("level_range", "")))
	if String(row.get("dedicated", "N")) == "Y":
		lines.append("Dedicated server")
	if String(row.get("pb_server", "")) == "Y":
		lines.append("PunkBuster on")
	if row_is_locked(row):
		lines.append("Password protected")
	return PackedStringArray(lines)


# Hover details for a browser row: the locale/address fields that don't fit
# the table cells.
func server_row_tooltip(row: Dictionary) -> String:
	var parts := PackedStringArray()
	var mission := String(row.get("mission_name", ""))
	if not mission.is_empty():
		parts.append("Mission: %s" % mission)
	var region := String(row.get("region", ""))
	if not region.is_empty():
		parts.append("Region: %s" % region)
	var country := String(row.get("country", ""))
	if not country.is_empty():
		parts.append("Country: %s" % country)
	var exp := String(row.get("exp", ""))
	if not exp.is_empty():
		parts.append("Expansion: %s" % exp)
	var ip := String(row.get("ip", ""))
	if not ip.is_empty() and ip != "0.0.0.0":
		parts.append("Address: %s" % ip)
	return "\n".join(parts)


func _on_server_list_updated(_updated: Array) -> void:
	if not _logged_in:
		return
	_browser_loading = false
	_refresh_servers()
	_set_screen(Screen.LOBBY)


func _on_server_list_failed(reason: String) -> void:
	if not _logged_in or _suppress_client_messages:
		return
	_browser_loading = false
	_rebuild_view()
	_set_status("Could not load games.")
	_show_message(reason, "Retry", Callable(self, "_retry_server_list"), Screen.LOBBY)


# The table's selected row's backing dictionary, or {} when none is selected.
func _selected_row() -> Dictionary:
	if _server_tree == null:
		return {}
	var item := _server_tree.get_selected()
	if item == null:
		return {}
	var row = item.get_metadata(0)
	return row if row is Dictionary else {}


func _on_server_selected() -> void:
	var row := _selected_row()
	_join_button.disabled = row.is_empty()
	_show_details(row)


# The details pane: the labeled facts plus the live player roster.
func _show_details(row: Dictionary) -> void:
	if _details_label == null:
		return
	if row.is_empty():
		_details_label.text = "Select a server for details."
		_roster_list.clear()
		return
	_details_label.text = "\n".join(server_details_lines(row))
	_roster_list.clear()
	var roster: PackedStringArray = row.get("player_names", PackedStringArray())
	for player in roster:
		_roster_list.add_item(player)
	if roster.is_empty():
		_roster_list.add_item("(no players reported)")


# --- Login (ADR 0010 Phase 3) -------------------------------------------

func _on_login_pressed() -> void:
	if _client == null:
		_show_message("Login is not available in this build.", "Back to Sign In",
				Callable(self, "_return_to_login"), Screen.LOGIN)
		return
	var user := _username_edit.text.strip_edges()
	var pwd := _password_edit.text
	if user.is_empty() or pwd.is_empty():
		_show_message("Enter both your account name and password.", "Back to Sign In",
				Callable(self, "_return_to_login"), Screen.LOGIN)
		return
	_login_button.disabled = true
	_set_status("Signing in as %s..." % user)
	_client.login(user, pwd)


func _on_login_succeeded(nwhandle: String) -> void:
	_logged_in = true
	_login_button.disabled = true
	_password_edit.clear()
	set_signed_in_handle(nwhandle)
	_host_button.disabled = _target == NovaWorldSettings.Target.REAL
	_rows.clear()
	_pings.clear()
	_browser_loading = true
	_rebuild_type_filter()
	_rebuild_view()
	if _population_label != null:
		_population_label.text = "Retrieving games..."
	_set_status("Signed in as %s." % nwhandle)
	_set_screen(Screen.LOBBY)


# On NovaWorld the account handle IS the in-game callsign: the host rosters
# the player under the service identity (the NAMEINFO cookie name), and a
# stock client sends that handle as its ClientAuth NA. Wire-witnessed live
# 2026-08-31 (stock `na="ljim"` = the host's 0x7B/0x46 roster name); a local
# callsign would leave the joiner's name-match waiting for a roster row that
# never exists, parked in the pre-spawn free-cam forever.
func set_signed_in_handle(nwhandle: String) -> void:
	if not nwhandle.strip_edges().is_empty():
		_nw_callsign = nwhandle.strip_edges()


# The callsign a join uses: the signed-in NW handle, else the local callsign.
func join_callsign() -> String:
	return _nw_callsign if not _nw_callsign.is_empty() else player_name


func _on_login_failed(reason: String) -> void:
	_logged_in = false
	_password_edit.clear()
	if _can_login:
		_login_button.disabled = false
	_set_status("Sign-in failed.")
	_show_message(reason, "Back to Sign In", Callable(self, "_return_to_login"), Screen.LOGIN)


# --- Join (ADR 0010 Phase 5) --------------------------------------------

func _on_join_pressed() -> void:
	if not _logged_in:
		return
	var row := _selected_row()
	if row.is_empty():
		_show_message("Select a game before joining.", "Back to Games",
				Callable(self, "_return_to_lobby"), Screen.LOBBY)
		return
	var rid := int(row.get("rid", 0))
	# Browse-time expansion advisory: warn BEFORE the join when the row's
	# advertised expansion cannot be honored locally, instead of letting the
	# in-match 0x7B reconcile abort the load minutes later (D-NET-178 stays the
	# authoritative gate — a stale/absent GSB `exp` never blocks; pressing Join
	# again proceeds anyway so the authoritative check has the last word).
	if expansion_advisory_blocks_first_press(row, rid):
		return
	# Remember what we need for the in-match join — joined_game only carries the resolved address.
	_pending_mission = String(row.get("mission_name", ""))
	# The NW handle when signed in (retail: your account name is your callsign
	# in NovaWorld games — see set_signed_in_handle); the local callsign otherwise.
	_pending_player = join_callsign()
	_set_status("Joining %s..." % String(row.get("name", "server")))
	_join_button.disabled = true
	if _client != null:
		_client.join(rid)


func _on_join_failed(reason: String) -> void:
	if not _logged_in or _suppress_client_messages:
		return
	_join_button.disabled = _selected_row().is_empty()
	_show_message(reason, "Back to Games", Callable(self, "_return_to_lobby"), Screen.LOBBY)


# True only on the FIRST Join press for a row whose advertised expansion the
# local install cannot supply (the policy's FAIL decision); sets the warning
# status and arms the second-press override.
func expansion_advisory_blocks_first_press(row: Dictionary, rid: int) -> bool:
	var host_exp := String(row.get("exp", "")).strip_edges()
	if host_exp.is_empty() or resource_root == null:
		return false
	if _exp_warning_armed and _exp_warning_armed_rid == rid:
		_exp_warning_armed = false
		return false
	var action: int = NetSessionPolicy.new().decide_expansion(
		host_exp, String(resource_root.get_expansion()),
		resource_root.list_expansions(resource_root.get_root_dir()))
	if action != NetSessionPolicy.ACTION_FAIL:
		return false
	_exp_warning_armed = true
	_exp_warning_armed_rid = rid
	_set_status("This server runs expansion '%s' which is not installed (installed: %s). Press Join again to try anyway." % [
		host_exp,
		NetSessionPolicy.describe_installed(
			resource_root.list_expansions(resource_root.get_root_dir()))])
	return true


# The NWJoin handshake resolved the host's in-match address. Hand it (with the
# stashed browse-time mission hint + callsign) to MainGame. The in-match runtime
# authenticates again, then S2C 0x7B/0x0B owns the actual mission load exactly as
# for LAN; the lobby hint is never a local-BMS requirement (D-NET-194).
func _on_joined_game(host: String, port: int, app_id: String, cd_cookie: PackedByteArray) -> void:
	_set_status("Entering %s:%d as %s..." % [host, port, _pending_player])
	var target := JoinTarget.new()
	target.host_ip = host
	target.port = port
	target.mission = _pending_mission
	target.player_name = _pending_player
	# The APPID join token (decoded .joi CK) the host validates (code 9), and the
	# CD identity cookie (packed PUB* blob) it validates in the 0x00 JOIN (code 23).
	target.app_id = app_id
	target.cd_cookie = cd_cookie
	join_in_match_requested.emit(target)


# Host a NovaWorld game: hand the gate (the server we're connected to) up to MainGame, which fills in
# the mission + callsign and stands up a browsable listen host (net_session_drive._maybe_start_nw_host). We
# register on the OpenNova gate only — never advertise a host on NovaLogic's live service.
func _on_host_pressed() -> void:
	if not _logged_in and start_client_on_ready:
		return
	if _target == NovaWorldSettings.Target.REAL:
		_show_message("Hosting is available on OpenNova servers only.", "Back to Games",
				Callable(self, "_return_to_lobby"), Screen.LOBBY)
		return
	var mission := _selected_mission()
	if mission.is_empty():
		_set_status("No missions are available to host.")
		_show_message("No missions are available to host. Check the game folder.",
				"Back to Host Game", Callable(self, "_return_to_host"), Screen.HOST)
		return
	_set_status("Starting host...")
	var config := HostSessionConfig.new()
	config.channel = HostSessionConfig.CHANNEL_NOVAWORLD
	config.nw_gate_host = _resolved_host()
	config.nw_gate_port = gate_port
	config.server_name = "%s's Game" % player_name
	config.mission = mission
	host_requested.emit(config)


# Populate the Map picker from the injected resource root. Empty (no root / no .bms) leaves the
# dropdown empty; Host then reports "no missions" rather than emitting an unhostable request.
func _populate_missions() -> void:
	if _mission_option == null:
		return
	_mission_option.clear()
	for m in MissionCatalog.mission_names(resource_root):
		_mission_option.add_item(m)
	if _mission_option.item_count > 0:
		_mission_option.select(0)


# --- Typed seams (tests drive the panel off-tree through these) -------------

## The wired client event source. Tests leave it unstarted and emit signals
## through the same boundary used by the live client.
func client_for_test() -> NovaWorldClient:
	return _client


## Install a literal row set (no client) and derive the view — the browser-table
## test seam.
func set_rows_for_test(rows: Array) -> void:
	_rows = rows
	_rebuild_type_filter()
	_rebuild_view()


## Install ping results (rid -> ping) — the ping-column test seam.
func set_pings_for_test(pings: Dictionary) -> void:
	_pings = pings
	_rebuild_view()


## The visible (filtered + sorted) rows, in table order.
func visible_rows() -> Array:
	return _view


## One visible table cell's text.
func visible_cell(row: int, column: int) -> String:
	if row < 0 or row >= _view.size():
		return ""
	return row_cells(_view[row], _ping_for(_view[row]))[column]


## Drive the filter bar (the controls, so the real signal path rebuilds).
func apply_filter_for_test(text: String, hide_full: bool, hide_empty: bool,
		hide_locked: bool) -> void:
	_filter_edit.text = text
	_hide_full_check.button_pressed = hide_full
	_hide_empty_check.button_pressed = hide_empty
	_hide_locked_check.button_pressed = hide_locked
	_rebuild_view()


## Drive a column-header click (sort toggle).
func click_column_for_test(column: int) -> void:
	_on_column_title_clicked(column, MOUSE_BUTTON_LEFT)


## The details pane's current text.
func details_text() -> String:
	return _details_label.text if _details_label != null else ""


func mission_count() -> int:
	return _mission_option.item_count if _mission_option != null else 0


func mission_name_at(index: int) -> String:
	return _mission_option.get_item_text(index) if _mission_option != null else ""


func select_mission(index: int) -> void:
	if _mission_option != null:
		_mission_option.select(index)


## The Map dropdown's current selection (the .bms basename), or "" when none.
func selected_mission() -> String:
	return _selected_mission()


## The Host button's press path.
func press_host() -> void:
	_on_host_pressed()


func status_text() -> String:
	return _status_label.text if _status_label != null else ""


func current_screen() -> int:
	return _screen


func browser_visible() -> bool:
	return _lobby_view != null and _lobby_view.visible


func login_visible() -> bool:
	return _login_view != null and _login_view.visible


func message_text() -> String:
	return _message_label.text if _message_label != null else ""


func server_item_count() -> int:
	return _view.size()


func host_enabled() -> bool:
	return _host_button != null and not _host_button.disabled


# The Map dropdown's current selection (the .bms basename), or "" when none.
func _selected_mission() -> String:
	if _mission_option == null or _mission_option.item_count == 0:
		return ""
	var idx := _mission_option.selected
	return _mission_option.get_item_text(idx) if idx >= 0 else ""


# Called by MainGame when a requested host could not start (no mission, load failed). Reports it on
# the panel instead of leaving the stale "Starting..." status, and re-enables Host (REAL stays off).
func host_failed(reason: String) -> void:
	_set_status(reason)
	_show_message(reason, "Back to Games", Callable(self, "_return_to_lobby"), Screen.LOBBY)
	if _host_button != null:
		_host_button.disabled = (_target == NovaWorldSettings.Target.REAL)


func _on_close_pressed() -> void:
	_closing = true
	if _client != null:
		_client.stop()
	closed.emit()
