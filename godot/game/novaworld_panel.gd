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
#
# Mods (ADR 0074): the Mods page, open before and after the sign-in, lists
# NovaWorld's published public mods of the game this folder holds and the mods
# installed here, and installs, updates and removes them through the updater;
# joining a server that runs a listed mod this install lacks offers its install,
# and the relaunch joins that server once signed in.

@export var gate_port := NovaWorldSettings.GATE_PORT
@export var player_name := "Player"
@export var start_client_on_ready := true

signal closed()
# The NWJoin handshake resolved the in-match host:port — enter the match as a JOINER. The arg is
# the typed dial target MainGame hands to GameWorld.load_mission_as_joiner
# (the SAME entry the LAN browser + --lan-join launch use — one in-match joiner seam, ADR 0009).
signal join_in_match_requested(target: JoinTarget)
# Host a NovaWorld game. The panel's session is already hosting (the service's ServerHostResult
# landed), so MainGame loads the mission as a listen host and the session rides in with it.
signal host_requested(config: HostSessionConfig)
# A mod's install or update started the updater: the game ends for it, as the
# menu's UPDATE ends it (MenuShell's exit_to_desktop_requested).
signal update_started(reason: String)

# The table's column titles, in NovaWorldServerBrowser.Column order (the
# engine's server_browser.h owns the columns, the filters, the sort and the
# cell text; this panel only renders them).
const COLUMN_TITLES: PackedStringArray = [
	"Server", "Mission", "Mode", "Players", "Ping", "Access"]
# The menu file holding the host dialog: the multiplayer menu, whose host
# buttons open its MULTI_PLAYER_HOST with file="mp.mnu" and whose
# NW_MULTI_PLAYER the NovaWorld arm returns to.
const HOST_DIALOG_MENU := "mp.mnu"

enum Screen { CONNECTING, LOGIN, LOBBY, HOST, MESSAGE, MODS }

# The Mods page's columns.
const MOD_COLUMN_TITLES: PackedStringArray = ["Mod", "By", "Version", "Here"]
# How long the mod list's request may take, in seconds.
const MODS_TIMEOUT := 20.0

var _client: NovaWorldClient  # created when the panel opens; torn down on close
var _status_label: Label
var _server_tree: Tree
var _host_button: Button
var _join_button: Button
var _close_button: Button
var _refresh_button: Button
var _username_edit: LineEdit
var _password_edit: LineEdit
var _login_button: Button
var _remember_login_check: CheckBox
var _filter_edit: LineEdit
var _type_filter: OptionButton
var _hide_full_check: CheckBox
var _hide_empty_check: CheckBox
var _hide_locked_check: CheckBox
var _details_label: Label
var _roster_list: ItemList
var _rows: Array[NovaWorldServerRow] = []   # the full GSB row set
var _view: Array[NovaWorldServerRow] = []   # the filtered + sorted rows the table shows
# rid -> ping: a round-trip in ms, or NovaWorldServerBrowser.PING_FAILED /
# PING_NEVER_ATTEMPTED (the sweep's codes); absent = in flight.
var _pings: Dictionary = {}
var _sort_column := NovaWorldServerBrowser.COLUMN_NAME
var _sort_ascending := true
var _can_login := false        # true once the gate reply gives us a startup_url
var _logged_in := false
var _signing_in := false       # a Login's post in flight
# A left press landed on this sign-in field: its release over the field is the click.
var _sign_in_press_field: LineEdit = null
# Stashed at join time: the selected row's mission. joined_game carries the resolved address
# plus the APPID join token and CD identity cookie, so we remember the mission (the joiner must
# know the host's mission to load it) and pair them when the join resolves. The player's name is
# the session's (NetSessionController: the CHAR cookie, else the profile's callsign).
var _pending_mission := ""
var _pending_expansion := ""  # the joined row's GSB Exp: the host's expansion
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
var _mods_view: Control
var _mods_intro: Label
var _mods_tree: Tree
var _mod_install_button: Button
var _mod_update_button: Button
var _mod_remove_button: Button
var _message_back_button: Button
# NovaWorld's mods of this folder's game (GET /api/mods): listed once the first
# server list lands or the Mods page opens, and again on each later open.
var _mods: Array[ModListing] = []
var _mods_listed := false
var _mods_error := ""
var _mods_request: HTTPRequest
# The server the relaunch after a mod's install joins once the list holds it
# (`--novaworld-join`, join_when_listed); 0 for none.
var _pending_join_rid := 0
# The screen the session is on behind the Mods page, which its Back returns to.
var _mods_return_screen := Screen.CONNECTING


func _ready() -> void:
	_bind_scene_ui()
	_set_screen(Screen.CONNECTING)
	_create_client(start_client_on_ready)


func _bind_scene_ui() -> void:
	var base := "Center/Shell/Margin/RootVBox/"
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
	_remember_login_check = get_node(
			pages + "LoginView/LoginCard/LoginMargin/LoginForm/RememberLoginCheck")
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
	_message_back_button = get_node(
			pages + "MessageView/MessageCard/MessageMargin/MessageForm/MessageBackButton")
	_mods_view = get_node(pages + "ModsView")
	_mods_intro = get_node(pages + "ModsView/ModsIntro")
	_mods_tree = get_node(pages + "ModsView/ModsTree")
	_mod_install_button = get_node(pages + "ModsView/ModsActions/InstallButton")
	_mod_update_button = get_node(pages + "ModsView/ModsActions/UpdateButton")
	_mod_remove_button = get_node(pages + "ModsView/ModsActions/RemoveButton")

	_close_button.pressed.connect(_on_close_pressed)
	_login_button.pressed.connect(_on_login_pressed)
	# The sign-in page's REMEMBER_LOGIN checkbox (JO's and DFX2's templates' "Remember my login
	# information."): a click flips it and stores its state (_on_remember_login_toggled); it takes
	# no keyboard, as the page's checkbox takes only clicks.
	_remember_login_check.toggled.connect(_on_remember_login_toggled)
	# Enter in a sign-in field lets the keyboard go, then runs the page's VK_RETURN hotkey (each
	# page's submit carries one: JO's ACCEPT, DFX2's LOGIN), which presses the submit when it is
	# shown and enabled; with no field holding the keyboard, Enter runs the same hotkey
	# (_unhandled_key_input). So one Enter submits, from either field or from neither. The page's
	# edits take the keys as the menu's do (engine: MenuRuntime::commit_edit_ and handle_key, the
	# edit's Enter and the focus-gated hotkey scan, D-NET-1336); the commit event an edit's
	# Enter emits has no taker on the sign-in page.
	for edit: LineEdit in [_username_edit, _password_edit]:
		edit.text_submitted.connect(_on_sign_in_field_submitted.bind(edit))
	# The sign-in fields as the page's edits take input (engine: LobbyHttpFlow's remembered
	# login): a click (a press and its release over the field) clears the remembered fill, a
	# keystroke does not; a field takes the keyboard only from its own click or from its
	# partner's Tab (the page's TAB actions), and each keystroke re-runs the submit gate. The
	# page's edits take 16 characters (MAXCHAR="16") and have no context menu.
	for edit: LineEdit in [_username_edit, _password_edit]:
		edit.focus_mode = Control.FOCUS_CLICK
		edit.max_length = 16
		edit.context_menu_enabled = false
		edit.text_changed.connect(func(_text: String) -> void: _refresh_sign_in_gate())
	_username_edit.gui_input.connect(_on_username_input)
	_password_edit.gui_input.connect(_on_password_input)
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
	_message_back_button.pressed.connect(_on_message_back_pressed)
	# The Mods page opens from the header on every screen, and from the games list.
	for path in [base + "Header/ModsButton", pages + "LobbyView/Actions/ModsButton"]:
		var mods_button: Button = get_node(path)
		mods_button.pressed.connect(_on_mods_pressed)
	var mods_back: Button = get_node(pages + "ModsView/ModsActions/BackButton")
	mods_back.pressed.connect(_on_mods_back_pressed)
	_mod_install_button.pressed.connect(_on_mod_install_pressed)
	_mod_update_button.pressed.connect(_on_mod_update_pressed)
	_mod_remove_button.pressed.connect(_on_mod_remove_pressed)
	_mods_tree.hide_root = true
	_mods_tree.select_mode = Tree.SELECT_ROW
	_mods_tree.columns = MOD_COLUMN_TITLES.size()
	_mods_tree.column_titles_visible = true
	for column in MOD_COLUMN_TITLES.size():
		_mods_tree.set_column_title(column, MOD_COLUMN_TITLES[column])
		_mods_tree.set_column_expand(column, column == 0)
	_mods_tree.item_selected.connect(_update_mod_buttons)

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
	# The Mods page stays up over the session's moves behind it (a connect, the
	# sign-in page, a list landing): its Back returns to where the session is.
	if _screen == Screen.MODS and next in [Screen.CONNECTING, Screen.LOGIN, Screen.LOBBY]:
		_mods_return_screen = next
		return
	_show_screen(next)


func _show_screen(next: int) -> void:
	_screen = next
	if _connecting_view != null:
		_connecting_view.visible = next == Screen.CONNECTING
		_login_view.visible = next == Screen.LOGIN
		_lobby_view.visible = next == Screen.LOBBY
		_host_view.visible = next == Screen.HOST
		_message_view.visible = next == Screen.MESSAGE
		_mods_view.visible = next == Screen.MODS


# `back_text`: a second button, which returns to `return_screen` without the
# action (a question's No); none when empty.
func _show_message(text: String, button_text: String, action: Callable,
		return_screen: int, back_text := "") -> void:
	_message_return_screen = return_screen
	_message_action = action
	if _message_label != null:
		_message_label.text = text.strip_edges() if not text.strip_edges().is_empty() \
				else "Could not complete the request."
	if _message_button != null:
		_message_button.text = button_text
	if _message_back_button != null:
		_message_back_button.text = back_text
		_message_back_button.visible = not back_text.is_empty()
	_set_screen(Screen.MESSAGE)


func _on_message_action_pressed() -> void:
	var action := _message_action
	_message_action = Callable()
	if action.is_valid():
		action.call()
	else:
		_set_screen(_message_return_screen)


func _on_message_back_pressed() -> void:
	_message_action = Callable()
	_set_screen(_message_return_screen)


func _return_to_login() -> void:
	_set_status("Connected. Sign in to continue.")
	_set_screen(Screen.LOGIN)
	# The page opens with no field holding the keyboard: one is taken by a click.
	if _username_edit != null:
		_username_edit.release_focus()
		_password_edit.release_focus()
	_refresh_sign_in_gate()


# A failed sign-in's way back: the sign-in page shown again, its fields as the page makes them
# (empty) and its remembered fill re-run (engine: LobbyHttpFlow::reshow_sign_in).
func _reshow_sign_in() -> void:
	if _client != null:
		_client.reshow_sign_in()
	if _username_edit != null:
		_username_edit.clear()
		_password_edit.clear()
		_password_edit.placeholder_text = ""
	_return_to_login()
	_apply_remembered_login()


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
	# The session speaks the mounted game's title (its identity and gate tag).
	_client.resource_root = resource_root
	# The session socket scans the process's cfg block's mpnovaworld quad.
	_client.set_game_config(GameCfgStore.block())
	add_child(_client)
	# The game's NovaWorld is OpenNova's alone (ADR 0075): the build's own deployment
	# (NovaWorldSettings.opennova_host).
	_client.host = NovaWorldSettings.opennova_host()
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
		"web_lobby_opened": _on_web_lobby_opened,
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
	_signing_in = false
	_rows = []
	_view = []
	_pings = {}
	_browser_loading = false
	if _population_label != null:
		_population_label.text = ""
	_pending_mission = ""
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
	_return_to_login()


func _on_disconnected(reason: String) -> void:
	if _suppress_client_messages or _closing:
		return
	_reset_connection_state()
	_show_message("The connection to the matchmaking service was closed.\n\n%s" % reason,
			"Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


# A connect failure the client reports as an NWEC tag (the gate's NWEC15, a give-up
# of the first connect's NWEC40..NWEC48) shows its menutxt text, as the retail
# NovaWorld error dialog does; any other message keeps the plain-language lead.
func _on_error(message: String) -> void:
	if _suppress_client_messages or _closing:
		return
	_reset_connection_state()
	var text := error_message(message)
	if text == message:
		text = "The matchmaking service could not be reached.\n\n%s" % message
	_show_message(text, "Retry", Callable(self, "_reconnect"), Screen.CONNECTING)


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
	if not _mods_listed:
		_request_mods()
	if _pending_join_rid != 0:
		_join_pending()


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
## 0x7B reconcile would fail the load on (D-NET-178) and NovaWorld lists no mod
## of (a listed one's Join offers its install). A row with no expansion field
## (base game, or absent GSB data) is otherwise joinable.
func join_block_reason(row: NovaWorldServerRow) -> String:
	if row == null:
		return ""
	if _refused_servers.has(row.rid):
		return String(_refused_servers[row.rid])
	if not _lacks_expansion(row):
		return ""
	var host_exp := row.exp.strip_edges()
	# A mod NovaWorld lists for this game: Join offers its install (ADR 0074).
	if _mod_for(host_exp) != null:
		return ""
	return "Requires expansion '%s', which is not installed (installed: %s)." % [
		host_exp, NetSessionPolicy.describe_installed(_installed_expansions)]


# Whether the row's host plays an expansion this install does not have, which
# the in-match 0x7B reconcile would fail the load on (D-NET-178).
func _lacks_expansion(row: NovaWorldServerRow) -> bool:
	var host_exp := row.exp.strip_edges()
	if resource_root == null or host_exp.is_empty():
		return false
	if not _installed_listed:
		_installed_expansions = resource_root.list_expansions(resource_root.get_root_dir())
		_installed_listed = true
	var action: int = NetSessionPolicy.new().decide_expansion(
		host_exp, String(resource_root.get_expansion()), _installed_expansions)
	return action == NetSessionPolicy.ACTION_FAIL


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
	if not _can_login or _signing_in:
		return  # no session, or a sign-in in flight (Login is disabled; this guards Enter)
	var user := _username_edit.text
	var pwd := _password_edit.text
	# The page's submit gate (DFX2's LOGIN; JO's ACCEPT posts whatever the fields hold).
	if not _client.is_sign_in_submit_enabled(user, pwd):
		return
	_signing_in = true
	_login_button.disabled = true
	_set_status("Signing in as %s..." % user)
	# The sign-in page's remembered login, no field clicked: its Login posts the remembered name,
	# an empty password and the remembered data (engine: LobbyHttpFlow::login_remembered).
	if pwd.is_empty() and _client.has_remembered_login():
		_client.login_remembered()
		return
	_client.login(user, pwd)


func _on_login_succeeded(nwhandle: String) -> void:
	if not _can_login:
		return  # the session was reset while the sign-in was in flight
	_logged_in = true
	_signing_in = false
	_login_button.disabled = true
	_password_edit.clear()
	_host_button.disabled = false
	_rows.clear()
	_pings.clear()
	_browser_loading = true
	_rebuild_type_filter()
	_rebuild_view()
	if _population_label != null:
		_population_label.text = "Retrieving games..."
	_set_status("Signed in as %s." % nwhandle)
	_set_screen(Screen.LOBBY)


func _on_login_failed(reason: String) -> void:
	if not _can_login:
		return  # the reset already reported the session
	_logged_in = false
	_signing_in = false
	_password_edit.clear()
	_login_button.disabled = false
	_set_status("Sign-in failed.")
	_show_message(reason, "Back to Sign In", Callable(self, "_reshow_sign_in"), Screen.LOGIN)


# --- Join (ADR 0010 Phase 5) --------------------------------------------

func _on_join_pressed() -> void:
	if not _logged_in:
		return
	var row := _selected_row()
	if row == null:
		_show_message("Select a game before joining.", "Back to Games",
				Callable(self, "_return_to_lobby"), Screen.LOBBY)
		return
	# A row this install cannot join never starts one (its Join is disabled;
	# this guards the press path itself).
	var blocked := _join_row(row)
	if not blocked.is_empty():
		_set_status(blocked)


# Join the row, or say why not: "" when the join started or the install of the
# mod its host plays was offered (ADR 0074), else the reason it cannot start.
func _join_row(row: NovaWorldServerRow) -> String:
	var blocked := join_block_reason(row)
	if not blocked.is_empty():
		return blocked
	if _lacks_expansion(row):
		_offer_mod_install(row, _mod_for(row.exp.strip_edges()))
		return ""
	var rid := row.rid
	# Remember what we need for the in-match join — joined_game only carries the resolved address.
	_joining_rid = rid
	_pending_mission = row.mission_name
	_pending_expansion = row.exp.strip_edges()
	_set_status("Joining %s..." % row.name)
	_join_button.disabled = true
	if _client != null:
		_client.join(rid)
	return ""


# The server's mod is not installed here: its install, then the join, are
# offered (ADR 0074). The install ends the game for the updater, and the
# relaunch joins the server once signed in and listed (join_when_listed).
func _offer_mod_install(row: NovaWorldServerRow, mod: ModListing) -> void:
	_show_message(("%s plays %s, a mod that is not installed here.\n\nInstall it and join? "
			+ "OpenNova closes while the updater installs it, then starts again and joins this "
			+ "server once you sign in.") % [row.name, mod_title(mod)],
			"Install and Join", _install_mod.bind(mod, row.rid), Screen.LOBBY, "Back to Games")


## The relaunch after a mod's install for a join (`--novaworld-join`, the shell's
## NetSessionController): the server is joined as soon as a signed-in list holds it,
## at once when one is up.
func join_when_listed(rid: int) -> void:
	_pending_join_rid = rid
	if _logged_in and not _rows.is_empty():
		_join_pending()


func _join_pending() -> void:
	var rid := _pending_join_rid
	_pending_join_rid = 0
	for row: NovaWorldServerRow in _rows:
		if row.rid != rid:
			continue
		_select_rid(rid)
		var blocked := _join_row(row)
		if not blocked.is_empty():
			_show_message(blocked, "Back to Games", Callable(), Screen.LOBBY)
		return
	_show_message("The server you were joining before the update is no longer listed.",
			"Back to Games", Callable(), Screen.LOBBY)


# Select the table's row of the server, when the filters show it.
func _select_rid(rid: int) -> void:
	if _server_tree == null or _server_tree.get_root() == null:
		return
	var item := _server_tree.get_root().get_first_child()
	while item != null:
		var row := item.get_metadata(0) as NovaWorldServerRow
		if row != null and row.rid == rid:
			item.select(NovaWorldServerBrowser.COLUMN_NAME)
			_on_server_selected()
			return
		item = item.get_next()


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
			return _with_tag(localized, reason)
	# The in-match join's CV* refusals are gameerr "MP Errors" keys.
	var mp_error := Strings.lookup_or(Strings.TABLE_GAMEERR, "MP Errors", reason, "")
	if not mp_error.is_empty() and mp_error != reason:
		return _with_tag(mp_error, reason)
	return reason


# The text with its tag beside it, unless the text already names the tag (the
# stock menutxt texts end in "(Code NWECnn)").
static func _with_tag(text: String, tag: String) -> String:
	if text.contains(tag):
		return text
	return "%s\n\n(%s)" % [text, tag]


# The NWJoin handshake resolved the host's in-match address. Hand it (with the
# stashed browse-time mission hint) to MainGame, which names the player from the session. The in-match runtime
# authenticates again, then S2C 0x7B/0x0B owns the actual mission load exactly as
# for LAN; the lobby hint is never a local-BMS requirement (D-NET-194).
func _on_joined_game(host: String, port: int, app_id: String, cd_cookie: PackedByteArray) -> void:
	# A join that resolves after the session was reset has no stash to enter with.
	if not _logged_in or _suppress_client_messages:
		return
	_set_status("Entering %s:%d..." % [host, port])
	var target := JoinTarget.new()
	# The NovaWorld connect type the menu picked (the squad talk row's gate).
	target.network_type = JoinTarget.NETWORK_NOVAWORLD
	target.host_ip = host
	target.port = port
	target.mission = _pending_mission
	# The host registered its expansion as the GSB Exp; the join switches to it
	# before it dials (the enumeration's SUS2 refines it when it answers).
	target.expansion = _pending_expansion
	target.expansion_known = true
	# The APPID join token (decoded .joi CK) the host validates (code 9), and the
	# CD identity cookie (packed PUB* blob) it validates in the 0x00 JOIN (code 23).
	target.app_id = app_id
	target.cd_cookie = cd_cookie
	# The gate's GLSVSSREQUEST, which the NovaWorld 0x00 JOIN carries last (D-NET-723).
	target.glsvss_request = _client.get_glsvss_request()
	# The proxy-assisted join fields (the .joi NI/NP/BK) and the LN lobby number
	# ride the target so the in-match joiner can install its rendezvous config
	# and pick the LAN-discovered endpoint over the relay.
	if _client.has_join_proxy():
		_client.copy_join_proxy_to(target)
	target.lobby_number = _client.get_join_lobby_number()
	join_in_match_requested.emit(target)


# Host a NovaWorld game on this logged-in session: the client sends ClientHostRequest and waits for
# the service's ServerHostResult; only a hosting session hands the request up to MainGame, which
# loads the mission as a listen host.
func _on_host_pressed() -> void:
	if not _logged_in and start_client_on_ready:
		return
	var mission := _selected_mission()
	if mission.is_empty():
		_set_status("No missions are available to host.")
		_show_message("No missions are available to host. Check the game folder.",
				"Back to Host Game", Callable(self, "_return_to_host"), Screen.HOST)
		return
	_set_status("Starting host...")
	# Retail's NovaWorld host is its host dialog, the LAN host's MULTI_PLAYER_HOST, whose START
	# reads the controls back into the process's cfg block before its NovaWorld arm opens the
	# hosting URL; this page's map picker stands in for that dialog, untouched. It opens on
	# the block (the populate writes the cap clamped to the screen's into it) and reads back
	# every control as the mounted menu's screen shows it, so the block keeps game.cfg's words
	# the screen can show, the session's name among them (no name of the page's own), and
	# takes the screen's for the rest: a spin's row 0 for a word no row holds, the location
	# row's own name, with no pb\pbcl.dll PunkBuster's 0 (engine inmatch/host_settings.h
	# read_untouched_host_dialog carries the witness). The request seeds from the block
	# (HostSessionConfig), and every mission start applies the block again (engine
	# inmatch::apply_block_session_settings).
	GameCfgStore.block().read_untouched_host_dialog(_host_dialog_document())
	var config := HostSessionConfig.new()
	config.channel = HostSessionConfig.CHANNEL_NOVAWORLD
	config.mission = mission
	# The authority plays the starting map's mode: g_GameType comes from the
	# mission, as the LAN host screen's request derives it.
	config.game_type_auto = true
	if resource_root != null:
		config.expansion = String(resource_root.get_expansion())
	_pending_host_config = config
	if _host_start_button != null:
		_host_start_button.disabled = true
	# The NovaWorld page's launch saves the player profile, then game.cfg,
	# before it hosts or dials, whatever the hosting answers
	# (docs/playerinfo/player-sav-re.md and docs/gamecfg/game-cfg-re.md, the
	# save points; engine: runtime/profile/player_profiles.h,
	# runtime/inmatch/game_config_block.h).
	PlayerProfile.save()
	GameCfgStore.save()
	if _client == null:
		_on_host_failed("NWEC01")
		return
	_client.start_hosting(config.to_session_options())


# The mounted menu holding the host dialog (HOST_DIALOG_MENU); null when the mount has
# none, which leaves only the populate's clamp.
func _host_dialog_document() -> MnuDocument:
	if resource_root == null:
		return null
	var bytes := resource_root.read_file(HOST_DIALOG_MENU)
	if bytes.is_empty():
		return null
	var doc := MnuDocument.new()
	return doc if doc.load_from_bytes(bytes) == OK else null


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


# --- Mods (ADR 0074) ------------------------------------------------------
# NovaWorld's published public mods of the game this folder holds (GET /api/mods,
# by the game's command file: JOPTR.CMD, DFX2.CMD, ACIDGAMBIT.CMD or a player
# game's), each installed into expansion/<code>/ by the updater: Install writes
# that folder's UPDATE.WIZ and runs UPDATE, Update runs UPDATE, Remove deletes
# the folder. The site is the game's NovaWorld's (NovaWorldSettings.web_base). The
# runtime holds no web session of the account, so its owner's private mods are
# not listed here.

func _on_mods_pressed() -> void:
	if _screen != Screen.MODS:
		_mods_return_screen = _screen
	_show_screen(Screen.MODS)
	_request_mods()
	_show_mods()


func _on_mods_back_pressed() -> void:
	var back := _mods_return_screen
	if back == Screen.LOBBY and not _logged_in:
		back = Screen.LOGIN
	_show_screen(back)


## The mod list's request: GET <site>/api/mods?game=<command file>.
static func mods_url(web_base: String, command_file: String) -> String:
	return "%s/api/mods?game=%s" % [web_base, command_file.uri_encode()]


func _request_mods() -> void:
	if _mods_request != null:
		return
	_mods_request = HTTPRequest.new()
	_mods_request.timeout = MODS_TIMEOUT
	add_child(_mods_request)
	_mods_request.request_completed.connect(_on_mods_completed)
	var url := mods_url(NovaWorldSettings.web_base(), LaunchFlags.product_command_file())
	if _mods_request.request(url) != OK:
		_end_mods_request()
		_mods_failed("NovaWorld's mods could not be requested.")
		return
	_show_mods()


func _end_mods_request() -> void:
	if _mods_request != null:
		_mods_request.queue_free()
		_mods_request = null


func _on_mods_completed(result: int, code: int, _headers: PackedStringArray,
		body: PackedByteArray) -> void:
	_end_mods_request()
	if result != HTTPRequest.RESULT_SUCCESS:
		_mods_failed("NovaWorld could not be reached for its mods.")
		return
	apply_mods_reply(code, body.get_string_from_utf8())


## The mod list's reply read into the page and the joins' install offers (the
## HTTP leg's test seam): a 200's installable mods (engine mod_listing.h).
func apply_mods_reply(code: int, text: String) -> void:
	var listed: Array[ModListing] = []
	if code != 200 or not ModListing.read_reply(text, listed).is_empty():
		_mods_failed("NovaWorld's mods did not load (HTTP %d)." % code)
		return
	_mods = listed
	_mods_listed = true
	_mods_error = ""
	_show_mods()
	_rebuild_view()


func _mods_failed(reason: String) -> void:
	_mods_error = reason
	_show_mods()


## The listed mod whose expansion code is `code` (any case), else null.
func _mod_for(code: String) -> ModListing:
	for mod in _mods:
		if mod.code.to_lower() == code.to_lower():
			return mod
	return null


## A mod's name as the page shows it: its name, else its code.
static func mod_title(mod: ModListing) -> String:
	if mod == null:
		return ""
	var title := mod.name.strip_edges()
	return title if not title.is_empty() else mod.code


# The page's rows: the listed mods, then each mod installed here the list does
# not hold (a private one, a retired one, another service's), by its folder's
# name, marked as not listed once a list has loaded, which only Remove takes.
func _show_mods() -> void:
	if _mods_tree == null:
		return
	var selected_code := _selected_code()
	_mods_tree.clear()
	var root := _mods_tree.create_item()
	for mod in _mods:
		var item := _mods_tree.create_item(root)
		item.set_text(0, mod_title(mod))
		item.set_text(1, mod.owner)
		item.set_text(2, mod.version)
		item.set_text(3, "Installed" if RuntimeUpdates.has_mod(mod.code) else "")
		item.set_metadata(0, mod)
		if mod.code == selected_code:
			item.select(0)
	for code in RuntimeUpdates.installed_mods():
		if _mod_for(code) != null:
			continue
		var item := _mods_tree.create_item(root)
		item.set_text(0, code)
		item.set_text(3, "Installed, not listed" if _mods_listed else "Installed")
		item.set_metadata(0, code)
		if code == selected_code:
			item.select(0)
	var game := LaunchFlags.product_name()
	if _mods_request != null and not _mods_listed:
		_mods_intro.text = "Retrieving mods..."
	elif not _mods_error.is_empty():
		_mods_intro.text = _mods_error
	elif _mods.is_empty():
		_mods_intro.text = "No mods of %s are published on NovaWorld yet." % game
	else:
		_mods_intro.text = ("Mods of %s on NovaWorld. Install and Update close OpenNova while the "
				+ "updater runs; it starts again when it is done.") % game
	_update_mod_buttons()


# The selected row's listed mod; null for none, or for a mod the list does not hold.
func _selected_mod() -> ModListing:
	var meta: Variant = _selected_row_meta()
	return meta if meta is ModListing else null


# The selected row's expansion code, listed or not; "" for none.
func _selected_code() -> String:
	var meta: Variant = _selected_row_meta()
	if meta is ModListing:
		return (meta as ModListing).code
	return meta if meta is String else ""


# A Mods page row's metadata: its ModListing, or the folder name of a mod the list
# does not hold; null for no selection.
func _selected_row_meta() -> Variant:
	if _mods_tree == null or _mods_tree.get_selected() == null:
		return null
	return _mods_tree.get_selected().get_metadata(0)


func _update_mod_buttons() -> void:
	var mod := _selected_mod()
	var code := _selected_code()
	var installed := not code.is_empty() and RuntimeUpdates.has_mod(code)
	_mod_install_button.disabled = mod == null or installed
	_mod_update_button.disabled = mod == null or not installed
	_mod_remove_button.disabled = not installed


func _on_mod_install_pressed() -> void:
	var mod := _selected_mod()
	if mod != null:
		_install_mod(mod, 0)


# `join_rid`: the server the relaunch joins (0 for none).
func _install_mod(mod: ModListing, join_rid: int) -> void:
	if RuntimeUpdates.install_mod(mod, join_rid):
		update_started.emit("the updater started for the mod %s" % mod_title(mod))
		return
	_show_message("%s could not be installed.\n\n%s" % [mod_title(mod), RuntimeUpdates.last_error],
			"Back", Callable(), Screen.MODS if join_rid == 0 else Screen.LOBBY)


func _on_mod_update_pressed() -> void:
	if RuntimeUpdates.launch_update():
		update_started.emit("the updater started for the mod %s" % mod_title(_selected_mod()))
		return
	_show_message("The update did not start.\n\n%s" % RuntimeUpdates.last_error, "Back",
			Callable(), Screen.MODS)


func _on_mod_remove_pressed() -> void:
	var code := _selected_code()
	if code.is_empty():
		return
	var title := mod_title(_selected_mod()) if _selected_mod() != null else code
	if resource_root != null and String(resource_root.get_expansion()).to_lower() == code.to_lower():
		_show_message(("%s is the mod the game is playing now. Choose the base game under "
				+ "Options, Mods, then remove it.") % title, "Back", Callable(), Screen.MODS)
		return
	if not RuntimeUpdates.remove_mod(code):
		_show_message("%s could not be removed.\n\n%s" % [title, RuntimeUpdates.last_error],
				"Back", Callable(), Screen.MODS)
		return
	_installed_listed = false
	_show_mods()
	_rebuild_view()


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
## verified and leaves its hosting and its play (each statement only from its own
## states), with no reconnect (the connect chain returns at once on a live session),
## and the menu opens its web lobby again as every entry does: the startup URL, whose
## pages land on the lobby for a login the service remembers, its list loading, or on
## the sign-in page (the engine's LobbyHttpFlow::enter carries the witness;
## docs/net/novaworld-net-re.md). A session that is not verified lands on sign-in.
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
	_logged_in = false
	_signing_in = false
	_wire_client(client)
	client.stop_hosting()
	client.stop_playing()
	_host_button.disabled = true
	_rows.clear()
	_pings.clear()
	_browser_loading = false
	_rebuild_type_filter()
	_rebuild_view()
	if client.get_state() != NovaWorldClient.STATE_CONNECTED:
		_return_to_login()
		return
	_set_status("Opening NovaWorld...")
	_set_screen(Screen.CONNECTING)
	client.open_web_lobby()


# The entry's page (the first entry's, which loads behind the sign-in screen,
# or a re-entry's): the lobby (a login the service remembers; the list loads
# behind it) or the sign-in page, whose Login posts its form.
func _on_web_lobby_opened(signed_in: bool, nwhandle: String) -> void:
	if not _can_login:
		return  # the session was reset while the entry was in flight
	if signed_in:
		_on_login_succeeded(nwhandle)
		return
	_logged_in = false
	_login_button.disabled = false
	if _screen != Screen.LOGIN:
		_return_to_login()
	_apply_remembered_login()


# The sign-in page's remembered login (engine: LobbyHttpFlow::remembered_login_name): the name
# fills the account box, and while the fill stands the password box shows the masked placeholder
# until a field is clicked (after a LOGIN press, until the home page shows, the name alone); the
# REMEMBER_LOGIN checkbox shows the state the page's activation set from game.cfg's
# rememberlogin; then the submit gate.
func _apply_remembered_login() -> void:
	if _client == null or _username_edit == null or _password_edit == null:
		return
	_remember_login_check.set_pressed_no_signal(_client.is_remember_login_checked())
	var remembered := _client.get_remembered_login_name()
	if not remembered.is_empty():
		_username_edit.text = remembered
	if _client.has_remembered_login():
		_password_edit.placeholder_text = "********"
	_refresh_sign_in_gate()


# The REMEMBER_LOGIN checkbox clicked, `checked` its state after the click: the browser state's
# property and game.cfg's rememberlogin take it (engine: LobbyHttpFlow::store_remember_login); the
# shell's game.cfg save points write it, and the next sign-in page's fill reads it.
func _on_remember_login_toggled(checked: bool) -> void:
	if _client != null:
		_client.store_remember_login(checked)


# A sign-in field clicked: the remembered login's fill clears, then the submit gate (engine:
# LobbyHttpFlow::clear_remembered_login).
func _clear_remembered_login() -> void:
	if _password_edit != null:
		_password_edit.placeholder_text = ""
	if _client != null:
		_client.clear_remembered_login()
	_refresh_sign_in_gate()


# The page's submit gate over the fields as they stand (engine:
# LobbyHttpFlow::sign_in_submit_enabled), on a connected session's sign-in page; a sign-in in
# flight keeps Login disabled.
func _refresh_sign_in_gate() -> void:
	if _client == null or _login_button == null or not _can_login or _signing_in or _logged_in:
		return
	if _client.get_state() != NovaWorldClient.STATE_CONNECTED:
		return
	_login_button.disabled = not _client.is_sign_in_submit_enabled(
			_username_edit.text, _password_edit.text)


func _on_username_input(event: InputEvent) -> void:
	_on_sign_in_field_input(_username_edit, _password_edit, event)


func _on_password_input(event: InputEvent) -> void:
	_on_sign_in_field_input(_password_edit, _username_edit, event)


# A sign-in field's input as the page's edit takes it: a left press and its release over the
# field is a click, which clears the remembered fill (a keystroke does not); Tab moves the
# keyboard to the partner field (the page's TAB action), with no click.
func _on_sign_in_field_input(field: LineEdit, partner: LineEdit, event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button != null and button.button_index == MOUSE_BUTTON_LEFT:
		if button.pressed:
			_sign_in_press_field = field
		else:
			var over := Rect2(Vector2.ZERO, field.size).has_point(button.position)
			if _sign_in_press_field == field and over:
				_clear_remembered_login()
			_sign_in_press_field = null
		return
	var key := event as InputEventKey
	if key != null and key.pressed and key.keycode == KEY_TAB:
		partner.grab_focus()
		field.accept_event()


# Enter in a sign-in field (see the wiring in _bind_scene_ui): the field lets the keyboard go,
# then the page's VK_RETURN hotkey.
func _on_sign_in_field_submitted(_text: String, field: LineEdit) -> void:
	field.release_focus()
	_press_sign_in_submit()


# The sign-in page's VK_RETURN hotkey: the submit pressed when the page shows and the submit is
# enabled (DFX2's LOGIN gate, a sign-in in flight, no session: disabled).
func _press_sign_in_submit() -> void:
	if _screen != Screen.LOGIN or not login_enabled():
		return
	_on_login_pressed()


# Enter with no sign-in field holding the keyboard (a focused field takes its own Enter): the
# page's VK_RETURN hotkey.
func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null or not key.pressed:
		return
	if key.keycode != KEY_ENTER and key.keycode != KEY_KP_ENTER:
		return
	if _screen != Screen.LOGIN or not is_visible_in_tree():
		return
	get_viewport().set_input_as_handled()
	_press_sign_in_submit()


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


func mods_visible() -> bool:
	return _mods_view != null and _mods_view.visible


func mod_count() -> int:
	return _mods.size()


## The Mods page's rows: the listed mods and the installed ones the list does not hold.
func mod_row_count() -> int:
	return _mods_tree.get_root().get_child_count() if _mods_tree.get_root() != null else 0


## The Mods page's row text: its four columns.
func mod_cells(index: int) -> PackedStringArray:
	var item := _mods_tree.get_root().get_child(index)
	var cells := PackedStringArray()
	for column in MOD_COLUMN_TITLES.size():
		cells.append(item.get_text(column))
	return cells


func mods_intro_text() -> String:
	return _mods_intro.text if _mods_intro != null else ""


## Select a Mods page row through the tree (the real selection signal path).
func select_mod_for_test(index: int) -> void:
	_mods_tree.get_root().get_child(index).select(0)
	_update_mod_buttons()


## The Mods page's buttons, as [install, update, remove] enabled.
func mod_buttons_enabled() -> Array[bool]:
	return [not _mod_install_button.disabled, not _mod_update_button.disabled,
			not _mod_remove_button.disabled]


func press_mods() -> void:
	_on_mods_pressed()


func press_mods_back() -> void:
	_on_mods_back_pressed()


func press_mod_install() -> void:
	_on_mod_install_pressed()


func press_mod_remove() -> void:
	_on_mod_remove_pressed()


## The message's buttons: its action, and its second (a question's No).
func press_message_action() -> void:
	_on_message_action_pressed()


func message_back_visible() -> bool:
	return _message_back_button != null and _message_back_button.visible


# The Map dropdown's current selection (the .bms basename), or "" when none.
func _selected_mission() -> String:
	if _mission_option == null or _mission_option.item_count == 0:
		return ""
	var idx := _mission_option.selected
	return _mission_option.get_item_text(idx) if idx >= 0 else ""


# Called by MainGame when a requested host could not start (no mission, load failed). Reports it on
# the panel instead of leaving the stale "Starting..." status, and re-enables Host.
func host_failed(reason: String) -> void:
	# A hosting the service granted but the shell could not start is left again.
	if _client != null and _client.is_hosting():
		_client.stop_hosting()
	_set_status(reason)
	_show_message(reason, "Back to Games", Callable(self, "_return_to_lobby"), Screen.LOBBY)
	if _host_button != null:
		_host_button.disabled = false


func _on_close_pressed() -> void:
	# The player's ESC while the menu waits on the first connect or a play: the client gives
	# it up with its NWEC (NWEC41 / NWEC47, the play's NWEC03), which this panel shows (D-NET-571).
	if _client != null and _client.escape_connect():
		return
	_closing = true
	if _client != null:
		_client.stop()
	closed.emit()
