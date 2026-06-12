class_name NovaWorldPanel
extends Control

# The NovaWorld (online multiplayer) front-end. Opened from the menu when the
# player chooses NovaWorld. It owns one NovaWorldClient (the GDExtension pump
# over libs/novaworld) and turns its protocol state into artist-facing status,
# a server browser, and a Host-a-Game action. The panel never touches the wire
# itself; the client does.
#
# Milestone flow: open -> the client probes the gate and runs the session
# handshake against our server -> on a verified session the browser fills from
# the server list -> Host a Game registers a row other clients can see.

# Where to reach the server. Dev default is localhost (matching the launcher's
# dev mode and the dev compose). Prod sets this from the resolved server IP.
@export var server_host := "127.0.0.1"
@export var gate_port := 7597
@export var player_name := "Player"

signal closed()

var _client            # NovaWorldClient (created at runtime if the class exists)
var _status_label: Label
var _server_list: ItemList
var _host_button: Button
var _join_button: Button
var _close_button: Button
var _target_option: OptionButton
var _username_edit: LineEdit
var _password_edit: LineEdit
var _login_button: Button
var _target: int = NovaWorldSettings.Target.OPENNOVA
var _rows: Array = []          # GSB rows, parallel to _server_list items (index -> row)
var _can_login := false        # true once the gate reply gives us a startup_url
var _logged_in := false


func _ready() -> void:
	_target = NovaWorldSettings.load_target()
	_build_ui()
	_create_client()


func _build_ui() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.custom_minimum_size = Vector2(480, 360)
	add_child(panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	panel.add_child(box)

	var title := Label.new()
	title.text = "NovaWorld"
	box.add_child(title)

	var target_row := HBoxContainer.new()
	box.add_child(target_row)
	var target_label := Label.new()
	target_label.text = "Server:"
	target_row.add_child(target_label)
	_target_option = OptionButton.new()
	_target_option.add_item("OpenNova", NovaWorldSettings.Target.OPENNOVA)
	_target_option.add_item("Original NovaWorld", NovaWorldSettings.Target.REAL)
	_target_option.select(_target_option.get_item_index(_target))
	_target_option.item_selected.connect(_on_target_selected)
	target_row.add_child(_target_option)

	_status_label = Label.new()
	_status_label.text = "Connecting..."
	box.add_child(_status_label)

	# Account login (session-only — nothing is persisted).
	var login_row := HBoxContainer.new()
	box.add_child(login_row)
	_username_edit = LineEdit.new()
	_username_edit.placeholder_text = "Username"
	_username_edit.custom_minimum_size = Vector2(150, 0)
	login_row.add_child(_username_edit)
	_password_edit = LineEdit.new()
	_password_edit.placeholder_text = "Password"
	_password_edit.secret = true
	_password_edit.custom_minimum_size = Vector2(150, 0)
	login_row.add_child(_password_edit)
	_login_button = Button.new()
	_login_button.text = "Log In"
	_login_button.disabled = true
	_login_button.pressed.connect(_on_login_pressed)
	login_row.add_child(_login_button)

	_server_list = ItemList.new()
	_server_list.custom_minimum_size = Vector2(440, 220)
	_server_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_server_list.item_selected.connect(_on_server_selected)
	box.add_child(_server_list)

	var buttons := HBoxContainer.new()
	box.add_child(buttons)

	_join_button = Button.new()
	_join_button.text = "Join"
	_join_button.disabled = true
	_join_button.pressed.connect(_on_join_pressed)
	buttons.add_child(_join_button)

	_host_button = Button.new()
	_host_button.text = "Host a Game"
	_host_button.disabled = true
	_host_button.pressed.connect(_on_host_pressed)
	buttons.add_child(_host_button)

	_close_button = Button.new()
	_close_button.text = "Back"
	_close_button.pressed.connect(_on_close_pressed)
	buttons.add_child(_close_button)


func _create_client() -> void:
	if not ClassDB.class_exists("NovaWorldClient"):
		_set_status("NovaWorld is unavailable in this build.")
		return
	_client = ClassDB.instantiate("NovaWorldClient")
	add_child(_client)
	_client.host = _resolved_host()
	_client.gate_port = gate_port
	_client.player_name = player_name
	_client.state_changed.connect(_on_state_changed)
	_client.connected.connect(_on_connected)
	_client.disconnected.connect(_on_disconnected)
	_client.error_occurred.connect(_on_error)
	if _client.has_signal("server_list_updated"):
		_client.server_list_updated.connect(_on_server_list_updated)
	if _client.has_signal("server_info_received"):
		_client.server_info_received.connect(_on_server_info_received)
	if _client.has_signal("login_succeeded"):
		_client.login_succeeded.connect(_on_login_succeeded)
	if _client.has_signal("login_failed"):
		_client.login_failed.connect(_on_login_failed)
	if _client.has_signal("joined_game"):
		_client.joined_game.connect(_on_joined_game)
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
	if _client != null:
		_client.stop()
		_client.queue_free()
		_client = null
	if _host_button != null:
		_host_button.disabled = true
	if _join_button != null:
		_join_button.disabled = true
	if _login_button != null:
		_login_button.disabled = true
	_can_login = false
	_logged_in = false
	_set_status("Connecting...")
	_create_client()


func _set_status(text: String) -> void:
	if _status_label != null:
		_status_label.text = text


# Map the protocol state to plain language (no wire jargon for the player).
func _on_state_changed(state: int) -> void:
	match state:
		0: _set_status("Ready.")                # Idle
		1: _set_status("Finding the server...") # GateProbing
		2, 3: _set_status("Connecting...")      # SessionHello / SessionJoin
		4: _set_status("Connected.")            # Connected
		5: _set_status("Disconnected.")         # Disconnected
		6: _set_status("Connection problem.")   # Error
		7: _set_status("Joining game...")       # Joining (NWJoin in flight)
		8: _set_status("Connecting to game host...")  # InGameHello (proto switched)
		_: _set_status("Connection problem.")


func _on_connected() -> void:
	_set_status("Connected. Choose a server or host your own.")
	_host_button.disabled = false
	_refresh_servers()


func _on_disconnected(reason: String) -> void:
	_set_status("Disconnected (%s)." % reason)
	_host_button.disabled = true


func _on_error(message: String) -> void:
	_set_status("Could not connect: %s" % message)
	_host_button.disabled = true


# Fill the browser from the GSB server list. Rows arrive asynchronously over
# HTTP once the session is verified; the client re-emits server_list_updated
# whenever it refetches, and connecting/refreshing both call through here.
func _refresh_servers() -> void:
	_server_list.clear()
	_rows = []
	if _client != null and _client.has_method("get_server_rows"):
		for row in _client.get_server_rows():
			_rows.append(row)
			_server_list.add_item(_format_server_row(row))
	if _server_list.item_count == 0:
		_server_list.add_item("No games are being hosted yet.")
	# A fresh list clears any prior selection.
	_join_button.disabled = true


# "ServerName  (3/16)  AAS  [locked]" — name, occupancy, game type, and a lock
# marker when the server is passworded or locked.
func _format_server_row(row: Dictionary) -> String:
	var name := String(row.get("name", "server"))
	var players := int(row.get("players", 0))
	var max_players := int(row.get("max_players", 0))
	var label := "%s  (%d/%d)" % [name, players, max_players]
	var game_type := String(row.get("game_type", ""))
	if not game_type.is_empty():
		label += "  " + game_type
	if String(row.get("password", "N")) == "Y" or String(row.get("locked", "N")) == "Y":
		label += "  [locked]"
	return label


func _on_server_list_updated(_updated: Array) -> void:
	_refresh_servers()


# Enable Join only for a real server row (the placeholder "No games..." item has
# no backing row).
func _on_server_selected(index: int) -> void:
	_join_button.disabled = index < 0 or index >= _rows.size()


# --- Login (ADR 0010 Phase 3) -------------------------------------------

func _on_server_info_received(_info: Dictionary) -> void:
	# The gate reply gives us the startup_url the login chain needs.
	_can_login = true
	if _login_button != null and not _logged_in:
		_login_button.disabled = false


func _on_login_pressed() -> void:
	if _client == null or not _client.has_method("login"):
		_set_status("Login is not available in this build.")
		return
	var user := _username_edit.text.strip_edges()
	var pwd := _password_edit.text
	if user.is_empty() or pwd.is_empty():
		_set_status("Enter a username and password.")
		return
	_login_button.disabled = true
	_set_status("Signing in as %s..." % user)
	_client.login(user, pwd)


func _on_login_succeeded(nwhandle: String) -> void:
	_logged_in = true
	_login_button.disabled = true
	_set_status("Signed in as %s. Choose a server to join." % nwhandle)


func _on_login_failed(reason: String) -> void:
	_logged_in = false
	if _can_login:
		_login_button.disabled = false
	_set_status("Login failed: %s" % reason)


# --- Join (ADR 0010 Phase 5) --------------------------------------------

func _on_join_pressed() -> void:
	var selected := _server_list.get_selected_items()
	if selected.is_empty():
		_set_status("Select a server to join.")
		return
	var index := int(selected[0])
	if index < 0 or index >= _rows.size():
		return
	var row: Dictionary = _rows[index]
	var rid := int(row.get("rid", 0))
	_set_status("Joining %s..." % String(row.get("name", "server")))
	if _client != null and _client.has_method("join"):
		_client.join(rid)


# The client has switched the connection protocol from the lobby (NOVAWORLDUDP)
# to the in-match game (JointOperations) by sending the ClientHello to the host.
# In-match gameplay is not implemented yet, so this is where the flow ends.
func _on_joined_game(host: String, port: int) -> void:
	_set_status("Joined — switched to JointOperations (%s:%d)." % [host, port])


func _on_host_pressed() -> void:
	if _client != null and _client.has_method("host_game"):
		_client.host_game()
		_set_status("Hosting a game...")
	else:
		_set_status("Hosting is not available yet in this build.")


func _on_close_pressed() -> void:
	if _client != null:
		_client.stop()
	closed.emit()
