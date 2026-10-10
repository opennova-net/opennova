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

# Where to reach the server. Dev default is localhost (matching the dev
# compose). Prod sets this from the resolved server IP.
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
# Host a NovaWorld game. The panel's session is already hosting (the service's ServerHostResult
# landed), so MainGame loads the mission as a listen host and the session rides in with it.
signal host_requested(config: HostSessionConfig)

# The table's column titles, in NovaWorldServerBrowser.Column order (the
# engine's server_browser.h owns the columns, the filters, the sort and the
# cell text; this panel only renders them).
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
var _rows: Array[NovaWorldServerRow] = []   # the full GSB row set
var _view: Array[NovaWorldServerRow] = []   # the filtered + sorted rows the table shows
# rid -> ping: a round-trip in ms, or NovaWorldServerBrowser.PING_FAILED /
# PING_NEVER_ATTEMPTED (the sweep's codes); absent = in flight.
var _pings: Dictionary = {}
var _sort_column := NovaWorldServerBrowser.COLUMN_NAME
var _sort_ascending := true
var _can_login := false        # true once the gate reply gives us a startup_url
var _logged_in := false
# Stashed at join time: the selected row's mission + our callsign. joined_game carries the
# resolved address plus the APPID join token and CD identity cookie, so we remember the mission
# (the joiner must know the host's mission to load it) and pair them when the join resolves.
var _pending_mission := ""
var _pending_player := ""
var _pending_expansion := ""  # the joined row's GSB Exp: the host's expansion
# The signed-in NovaWorld handle — the callsign every NW join uses (retail
# parity: the service identity names the player, not the local profile).
var _nw_callsign := ""
# The install's expansions, listed once per server-list fill (join_block_reason
# reads it for every row the table draws).
var _installed_expansions := PackedStringArray()
var _installed_listed := false
# rid -> the reason a server refused this install earlier in the session (its
# game version, an expansion mismatch, a banned address): retrying cannot work.
# The controller keeps the record across panel instances (mark_refused).
var _refused_servers: Dictionary = {}
# The rid of the row the last Join press went to (the controller reads it when
# the join leaves the panel, to remember a refusal against it).
var _joining_rid := 0
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
# The host request awaiting its ServerHostResult (start_hosting in flight).
var _pending_host_config: HostSessionConfig


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
		_server_tree.set_column_expand(column, column == NovaWorldServerBrowser.COLUMN_NAME
				or column == NovaWorldServerBrowser.COLUMN_MISSION)
	_server_tree.set_column_custom_minimum_width(NovaWorldServerBrowser.COLUMN_TYPE, 90)
	_server_tree.set_column_custom_minimum_width(NovaWorldServerBrowser.COLUMN_PLAYERS, 76)
	_server_tree.set_column_custom_minimum_width(NovaWorldServerBrowser.COLUMN_PING, 62)
	_server_tree.set_column_custom_minimum_width(NovaWorldServerBrowser.COLUMN_ACCESS, 86)
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
	_client.gametext = Strings.get_table(Strings.TABLE_GAMETEXT)
	_wire_client(_client)
	if start_now:
		_client.start()


# The panel's handlers on its session's signals: wired while the panel holds the
# session, cut when it hands the session to the match.
func _client_handlers() -> Dictionary:
	return {
		"state_changed": _on_state_changed,
		"connected": _on_connected,
		"disconnected": _on_disconnected,
		"error_occurred": _on_error,
		"server_list_updated": _on_server_list_updated,
		"server_list_failed": _on_server_list_failed,
		"server_pings_updated": _on_server_pings_updated,
		"login_succeeded": _on_login_succeeded,
		"login_failed": _on_login_failed,
		"join_failed": _on_join_failed,
		"joined_game": _on_joined_game,
		"hosting_started": _on_hosting_started,
		"host_failed": _on_host_failed,
	}


func _wire_client(client: NovaWorldClient) -> void:
	var handlers := _client_handlers()
	for signal_name: String in handlers:
		var handler: Callable = handlers[signal_name]
		if not client.is_connected(signal_name, handler):
			client.connect(signal_name, handler)


func _unwire_client(client: NovaWorldClient) -> void:
	var handlers := _client_handlers()
	for signal_name: String in handlers:
		var handler: Callable = handlers[signal_name]
		if client.is_connected(signal_name, handler):
			client.disconnect(signal_name, handler)


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
	_reset_connection_state()
	_set_status("Connecting to matchmaking...")
	_set_screen(Screen.CONNECTING)
	_create_client()


# Everything the panel derived from its session: the sign-in, the rows, their
# pings and the population line, the join and host requests in flight, and the
# actions they armed. A reconnect, a disconnect and an error each leave the
# panel with none of it, so a late answer to a sign-in, join, host or ping
# request finds nothing to act on and a stale row cannot be joined. A later
# `connected` still arms Login afresh, as it should after a soft gate error,
# which reports error_occurred while the session lives on.
func _reset_connection_state() -> void:
	if _host_button != null:
		_host_button.disabled = true
	if _join_button != null:
		_join_button.disabled = true
	if _login_button != null:
		_login_button.disabled = true
	_can_login = false
	_logged_in = false
	_nw_callsign = ""
	_rows = []
	_view = []
	_pings = {}
	_browser_loading = false
	if _population_label != null:
		_population_label.text = ""
	_pending_mission = ""
	_pending_player = ""
	_pending_expansion = ""
	_joining_rid = 0
	# The host Start button is held only while a host request is in flight.
	_pending_host_config = null
	if _host_start_button != null:
		_host_start_button.disabled = false
	if _password_edit != null:
		_password_edit.clear()
	_rebuild_type_filter()
	_rebuild_view()


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
		NovaWorldClient.STATE_HOSTING_REQUESTED, NovaWorldClient.STATE_HOSTING:
			_set_status("Starting host...")  # ClientHostRequest out / granted
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
	_reset_connection_state()
	_show_message("The connection to the matchmaking service was closed.\n\n%s" % reason,
			"Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


func _on_error(message: String) -> void:
	if _suppress_client_messages or _closing:
		return
	_reset_connection_state()
	_show_message("The matchmaking service could not be reached.\n\n%s" % message,
			"Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


# Fill the browser from the GSB server list. Rows arrive asynchronously over
# HTTP once the session is verified; the client re-emits server_list_updated
# whenever it refetches, and connecting/refreshing both call through here.
func _refresh_servers() -> void:
	_rows = []
	_pings = {}
	_installed_listed = false
	if _client != null:
		_rows = _client.get_server_rows()
		_pings = _client_pings()
	_rebuild_type_filter()
	_rebuild_view()
	_show_population_status()


# The service-wide population line ("58 servers, 214 players online") once the
# list carries totals.
func _show_population_status() -> void:
	if _client == null:
		return
	var servers := _client.get_total_servers()
	var players := _client.get_total_players()
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
		var t := row.game_type.strip_edges()
		if t.is_empty() or seen.has(t.to_lower()):
			continue
		seen[t.to_lower()] = true
		_type_filter.add_item(t)
		if t == previous:
			_type_filter.select(_type_filter.item_count - 1)


# The filter bar's current state, applied through the engine's filter.
func _filtered_rows() -> Array[NovaWorldServerRow]:
	var type_text := ""
	if _type_filter != null and _type_filter.selected > 0:
		type_text = _type_filter.get_item_text(_type_filter.selected)
	return NovaWorldServerBrowser.filter_rows(_rows,
			_filter_edit.text if _filter_edit != null else "",
			type_text,
			_hide_full_check != null and _hide_full_check.button_pressed,
			_hide_empty_check != null and _hide_empty_check.button_pressed,
			_hide_locked_check != null and _hide_locked_check.button_pressed)


# Re-derive the visible table from the full row set: filter, sort, repopulate,
# and keep the selection on the same rid when it survives the rebuild.
func _rebuild_view() -> void:
	if _server_tree == null:
		return
	var selected_rid := -1
	var selected_row := _selected_row()
	if selected_row != null:
		selected_rid = selected_row.rid
	_view = NovaWorldServerBrowser.sort_rows(_filtered_rows(),
			_sort_column, _sort_ascending, _pings)
	_server_tree.clear()
	var root := _server_tree.create_item()
	var reselected := false
	var first_item: TreeItem
	for row: NovaWorldServerRow in _view:
		var item := _server_tree.create_item(root)
		if first_item == null:
			first_item = item
		var cells := NovaWorldServerBrowser.row_cells(row, _ping_for(row))
		for c in cells.size():
			item.set_text(c, cells[c])
		var tooltip := NovaWorldServerBrowser.row_tooltip(row)
		var blocked := join_block_reason(row)
		if not blocked.is_empty():
			# A server this install cannot join reads greyed out, like a
			# disabled control, and says why on hover.
			var dim := _server_tree.get_theme_color("font_disabled_color", "Button")
			for c in cells.size():
				item.set_custom_color(c, dim)
			tooltip = blocked + "\n" + tooltip
		item.set_tooltip_text(NovaWorldServerBrowser.COLUMN_NAME, tooltip)
		item.set_metadata(0, row)
		if row.rid == selected_rid:
			item.select(NovaWorldServerBrowser.COLUMN_NAME)
			reselected = true
	if _empty_label != null:
		_empty_label.visible = _view.is_empty()
		_empty_label.text = "Retrieving games..." if _browser_loading else (
				"No games are being hosted right now." if _rows.is_empty()
				else "No games match these filters.")
	_server_tree.visible = not _view.is_empty()
	if not reselected and first_item != null:
		first_item.select(NovaWorldServerBrowser.COLUMN_NAME)
		_update_join_button(first_item.get_metadata(0))
		_show_details(first_item.get_metadata(0))
	elif not reselected:
		_update_join_button(null)
		_show_details(null)
	_update_column_titles()


## The row's ping so far: the sweep's result, else PING_PENDING while in flight.
func _ping_for(row: NovaWorldServerRow) -> int:
	return int(_pings.get(row.rid, NovaWorldServerBrowser.PING_PENDING))


func _on_column_title_clicked(column: int, _mouse_button_index: int) -> void:
	if column == _sort_column:
		_sort_ascending = not _sort_ascending
	else:
		_sort_column = column as NovaWorldServerBrowser.Column
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
	if _client == null or not _logged_in:
		return
	_pings = _client_pings()
	if _sort_column == NovaWorldServerBrowser.COLUMN_PING:
		_rebuild_view()
		return
	if _server_tree == null or _server_tree.get_root() == null:
		return
	var item := _server_tree.get_root().get_first_child()
	while item != null:
		var row := item.get_metadata(0) as NovaWorldServerRow
		if row != null:
			item.set_text(NovaWorldServerBrowser.COLUMN_PING,
					NovaWorldServerBrowser.ping_text(_ping_for(row)))
		item = item.get_next()


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


# The table's selected row, or null when none is selected.
func _selected_row() -> NovaWorldServerRow:
	if _server_tree == null:
		return null
	var item := _server_tree.get_selected()
	if item == null:
		return null
	return item.get_metadata(0) as NovaWorldServerRow


func _on_server_selected() -> void:
	var row := _selected_row()
	_update_join_button(row)
	_show_details(row)


# Join is live only for a selected row this install can join.
func _update_join_button(row: NovaWorldServerRow) -> void:
	_join_button.disabled = row == null or not join_block_reason(row).is_empty()


## Why this install cannot join the row, or "" when it can: the server already
## refused this install this session (the refusal's own text), or the row
## advertises an expansion the install does not have, the decision the in-match
## 0x7B reconcile would fail the load on (D-NET-178). A row with no expansion
## field (base game, or absent GSB data) is otherwise joinable.
func join_block_reason(row: NovaWorldServerRow) -> String:
	if row == null:
		return ""
	if _refused_servers.has(row.rid):
		return String(_refused_servers[row.rid])
	if resource_root == null:
		return ""
	var host_exp := row.exp.strip_edges()
	if host_exp.is_empty():
		return ""
	if not _installed_listed:
		_installed_expansions = resource_root.list_expansions(resource_root.get_root_dir())
		_installed_listed = true
	var action: int = NetSessionPolicy.new().decide_expansion(
		host_exp, String(resource_root.get_expansion()), _installed_expansions)
	if action != NetSessionPolicy.ACTION_FAIL:
		return ""
	return "Requires expansion '%s', which is not installed (installed: %s)." % [
		host_exp, NetSessionPolicy.describe_installed(_installed_expansions)]


# The details pane: the labeled facts plus the live player roster.
func _show_details(row: NovaWorldServerRow) -> void:
	if _details_label == null:
		return
	if row == null:
		_details_label.text = "Select a server for details."
		_roster_list.clear()
		return
	var lines := NovaWorldServerBrowser.details_lines(row)
	var blocked := join_block_reason(row)
	if not blocked.is_empty():
		lines.insert(0, "Cannot join: " + blocked)
	_details_label.text = "\n".join(lines)
	_roster_list.clear()
	var roster := row.player_names
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
	if not _can_login:
		return  # no session to sign in on (its Login is disabled; this guards Enter)
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
	if not _can_login:
		return  # the session was reset while the sign-in was in flight
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
	if not _can_login:
		return  # the reset already reported the session
	_logged_in = false
	_password_edit.clear()
	_login_button.disabled = false
	_set_status("Sign-in failed.")
	_show_message(reason, "Back to Sign In", Callable(self, "_return_to_login"), Screen.LOGIN)


# --- Join (ADR 0010 Phase 5) --------------------------------------------

func _on_join_pressed() -> void:
	if not _logged_in:
		return
	var row := _selected_row()
	if row == null:
		_show_message("Select a game before joining.", "Back to Games",
				Callable(self, "_return_to_lobby"), Screen.LOBBY)
		return
	var rid := row.rid
	# A row this install cannot join never starts one (its Join is disabled;
	# this guards the press path itself).
	var blocked := join_block_reason(row)
	if not blocked.is_empty():
		_set_status(blocked)
		return
	# Remember what we need for the in-match join — joined_game only carries the resolved address.
	_joining_rid = rid
	_pending_mission = row.mission_name
	_pending_expansion = row.exp.strip_edges()
	# The NW handle when signed in (retail: your account name is your callsign
	# in NovaWorld games — see set_signed_in_handle); the local callsign otherwise.
	_pending_player = join_callsign()
	_set_status("Joining %s..." % row.name)
	_join_button.disabled = true
	if _client != null:
		_client.join(rid)


func _on_join_failed(reason: String) -> void:
	if not _logged_in or _suppress_client_messages:
		return
	_update_join_button(_selected_row())
	_show_message(error_message(reason), "Back to Games", Callable(self, "_return_to_lobby"),
			Screen.LOBBY)


# The NovaWorld error dialog's text: NWEC / CV tags are keys in menutxt.bin
# (the tag stays beside the text for diagnostics); HTTP failures already carry
# prose (the nw_error.mnx dialog, docs/net/novaworld-net-re.md).
static func error_message(reason: String) -> String:
	var menutxt := Strings.get_table(Strings.TABLE_MENUTXT)
	if menutxt != null and menutxt.has_string(reason):
		var localized := menutxt.get_string(reason)
		if not localized.is_empty() and localized != reason:
			return "%s\n\n(%s)" % [localized, reason]
	# The in-match join's CV* refusals are gameerr "MP Errors" keys.
	var mp_error := Strings.lookup_or(Strings.TABLE_GAMEERR, "MP Errors", reason, "")
	if not mp_error.is_empty() and mp_error != reason:
		return "%s\n\n(%s)" % [mp_error, reason]
	return reason


# The NWJoin handshake resolved the host's in-match address. Hand it (with the
# stashed browse-time mission hint + callsign) to MainGame. The in-match runtime
# authenticates again, then S2C 0x7B/0x0B owns the actual mission load exactly as
# for LAN; the lobby hint is never a local-BMS requirement (D-NET-194).
func _on_joined_game(host: String, port: int, app_id: String, cd_cookie: PackedByteArray) -> void:
	# A join that resolves after the session was reset has no stash to enter with.
	if not _logged_in or _suppress_client_messages:
		return
	_set_status("Entering %s:%d as %s..." % [host, port, _pending_player])
	var target := JoinTarget.new()
	# The NovaWorld connect type the menu picked (the squad talk row's gate).
	target.network_type = JoinTarget.NETWORK_NOVAWORLD
	target.host_ip = host
	target.port = port
	target.mission = _pending_mission
	target.player_name = _pending_player
	# The host registered its expansion as the GSB Exp; the join switches to it
	# before it dials (the enumeration's SUS2 refines it when it answers).
	target.expansion = _pending_expansion
	target.expansion_known = true
	# The APPID join token (decoded .joi CK) the host validates (code 9), and the
	# CD identity cookie (packed PUB* blob) it validates in the 0x00 JOIN (code 23).
	target.app_id = app_id
	target.cd_cookie = cd_cookie
	# The proxy-assisted join fields (the .joi NI/NP/BK) and the LN lobby number
	# ride the target so the in-match joiner can install its rendezvous config
	# and pick the LAN-discovered endpoint over the relay.
	if _client.has_join_proxy():
		target.proxy_node = _client.get_join_proxy_node()
		target.proxy_relay = _client.get_join_proxy_relay()
		target.proxy_cookie = _client.get_join_proxy_cookie()
	target.lobby_number = _client.get_join_lobby_number()
	join_in_match_requested.emit(target)


# Host a NovaWorld game on this logged-in session: the client sends ClientHostRequest and waits for
# the service's ServerHostResult; only a hosting session hands the request up to MainGame, which
# loads the mission as a listen host. We host on the OpenNova gate only — never advertise a host on
# NovaLogic's live service.
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
	config.server_name = "%s's Game" % player_name
	config.mission = mission
	# The authority plays the starting map's mode: g_GameType comes from the
	# mission, as the LAN host screen's request derives it.
	config.game_type_auto = true
	if resource_root != null:
		config.expansion = String(resource_root.get_expansion())
	_pending_host_config = config
	if _host_start_button != null:
		_host_start_button.disabled = true
	if _client == null:
		_on_host_failed("NWEC01")
		return
	_client.start_hosting(config.to_session_options())


# The service accepted the hosting (the session is in state 6): the mission
# starts now, with the session riding in.
func _on_hosting_started() -> void:
	var config := _pending_host_config
	_pending_host_config = null
	if _host_start_button != null:
		_host_start_button.disabled = false
	if config == null:
		return
	host_requested.emit(config)


func _on_host_failed(reason: String) -> void:
	var config := _pending_host_config
	_pending_host_config = null
	if _host_start_button != null:
		_host_start_button.disabled = false
	# A failure for a request the reset already dropped reports nothing.
	if _suppress_client_messages or config == null:
		return
	host_failed(error_message(reason))


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

## Hand the lobby session over at the in-match handoff: retail keeps the
## NovaWorld session playing through the match, so the client leaves the panel
## (which is dismissed next) instead of dying with it. Null when none is held.
func release_client() -> NovaWorldClient:
	var client := _client
	_client = null
	if client == null:
		return null
	_unwire_client(client)
	if client.get_parent() == self:
		remove_child(client)
	return client


## The NovaWorld menu's re-entry after a match: the session comes back still
## verified, leaves its hosting and its play (each statement only from its own
## states), and the player lands back in the logged-in lobby with the list
## refreshed -- no reconnect, no sign-in (UI_EnterNovaWorldMenu's chain short-circuits
## on a live session; docs/net/novaworld-net-re.md).
func adopt_client(client: NovaWorldClient) -> void:
	if client == null:
		return
	_suppress_client_messages = true
	if _client != null and _client != client:
		_client.stop()
		_client.queue_free()
	_suppress_client_messages = false
	_client = client
	if client.get_parent() != self:
		if client.get_parent() != null:
			client.get_parent().remove_child(client)
		add_child(client)
	_can_login = true
	_logged_in = client.is_authenticated()
	_wire_client(client)
	client.stop_hosting()
	client.stop_playing()
	_host_button.disabled = not _logged_in or _target == NovaWorldSettings.Target.REAL
	_rows.clear()
	_pings.clear()
	_browser_loading = _logged_in
	_rebuild_type_filter()
	_rebuild_view()
	if not _logged_in:
		_return_to_login()
		return
	_set_status("Signed in.")
	_set_screen(Screen.LOBBY)
	client.refresh_servers()


## A match that ended with an error text shows it in the NovaWorld error dialog
## first. Its Back re-enters NovaWorld (a fresh connect: the session was reset)
## when the player had left from the NovaWorld menu, else closes back to the
## main menu (the menu shell's return and the dialog's BACK, docs/net/novaworld-net-re.md).
func show_post_mission_error(reason: String, reenter_novaworld: bool) -> void:
	_suppress_client_messages = true
	if _client != null:
		_client.stop()
		_client.queue_free()
		_client = null
	_suppress_client_messages = false
	var text := error_message(reason if not reason.is_empty() else "CVUNKNOWN")
	var back := Callable(self, "_reconnect") if reenter_novaworld else Callable(self, "_on_close_pressed")
	_set_status(text)
	_show_message(text, "Back", back, Screen.CONNECTING)


## The wired client event source. Tests leave it unstarted and emit signals
## through the same boundary used by the live client.
func client_for_test() -> NovaWorldClient:
	return _client


## Install a literal row set (no client) and derive the view — the browser-table
## test seam.
func set_rows_for_test(rows: Array[NovaWorldServerRow]) -> void:
	_rows = rows
	_rebuild_type_filter()
	_rebuild_view()


## Install ping results (rid -> ping) — the ping-column test seam.
func set_pings_for_test(pings: Dictionary) -> void:
	_pings = pings
	_rebuild_view()


# The client's sweep results (parallel rid / ping arrays) as the rid -> ping
# map the view sorts and renders by.
func _client_pings() -> Dictionary:
	var pings := {}
	var rids := _client.get_server_ping_rids()
	var values := _client.get_server_ping_values()
	for i in range(mini(rids.size(), values.size())):
		pings[rids[i]] = values[i]
	return pings


## The visible (filtered + sorted) rows, in table order.
func visible_rows() -> Array[NovaWorldServerRow]:
	return _view


## One visible table cell's text.
func visible_cell(row: int, column: NovaWorldServerBrowser.Column) -> String:
	if row < 0 or row >= _view.size():
		return ""
	return NovaWorldServerBrowser.row_cells(_view[row], _ping_for(_view[row]))[column]


## A server that refused this install this session, with the refusal's text
## (the controller replays its record into each panel instance).
func mark_refused(rid: int, reason: String) -> void:
	_refused_servers[rid] = reason
	_rebuild_view()


## The rid of the row the last Join press went to.
func joining_rid() -> int:
	return _joining_rid


## Select a visible row through the table (the real selection signal path).
func select_visible_row_for_test(index: int) -> void:
	var item := _server_tree.get_root().get_child(index)
	item.select(NovaWorldServerBrowser.COLUMN_NAME)
	_on_server_selected()


## True when the visible row is drawn greyed out (a row this install cannot join).
func visible_row_dimmed(index: int) -> bool:
	var item := _server_tree.get_root().get_child(index)
	return item.get_custom_color(NovaWorldServerBrowser.COLUMN_NAME) \
			== _server_tree.get_theme_color("font_disabled_color", "Button")


func join_enabled() -> bool:
	return _join_button != null and not _join_button.disabled


func login_enabled() -> bool:
	return _login_button != null and not _login_button.disabled


## The Join button's press path (the table's double-click takes the same one).
func press_join() -> void:
	_on_join_pressed()


## Drive the filter bar (the controls, so the real signal path rebuilds).
func apply_filter_for_test(text: String, hide_full: bool, hide_empty: bool,
		hide_locked: bool) -> void:
	_filter_edit.text = text
	_hide_full_check.button_pressed = hide_full
	_hide_empty_check.button_pressed = hide_empty
	_hide_locked_check.button_pressed = hide_locked
	_rebuild_view()


## Drive a column-header click (sort toggle).
func click_column_for_test(column: NovaWorldServerBrowser.Column) -> void:
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
	# A hosting the service granted but the shell could not start is left again.
	if _client != null and _client.is_hosting():
		_client.stop_hosting()
	_set_status(reason)
	_show_message(reason, "Back to Games", Callable(self, "_return_to_lobby"), Screen.LOBBY)
	if _host_button != null:
		_host_button.disabled = (_target == NovaWorldSettings.Target.REAL)


func _on_close_pressed() -> void:
	_closing = true
	if _client != null:
		_client.stop()
	closed.emit()
