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
@export var gate_port := NovaWorldSettings.GATE_PORT
@export var player_name := "Player"

signal closed()
# The NWJoin handshake resolved the in-match host:port — enter the match as a JOINER. The arg is
# the typed dial target MainGame hands to GameWorld.load_mission_as_joiner
# (the SAME entry the LAN browser + NW_LAN_JOIN env use — one in-match joiner seam, ADR 0009).
signal join_in_match_requested(target: JoinTarget)
# Host a NovaWorld game. The panel supplies the gate (the server it's connected to); MainGame fills in
# the mission + callsign and stands up a browsable listen host (net_session_drive._maybe_start_nw_host).
signal host_requested(config: HostSessionConfig)

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
# Stashed at join time: the selected row's mission + our callsign. joined_game(host, port) carries
# only the resolved address, so we remember the mission (the joiner must know the host's mission to
# load it) and pair them when the join resolves.
var _pending_mission := ""
var _pending_player := ""
# The mounted resource root, set by MainGame BEFORE _ready so the host Map picker can list the
# install's .bms missions (the panel owns no mission list; the world's root is null until a load).
var resource_root  # NovaResourceRoot
var _mission_option: OptionButton


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

	# Map to host: the install's .bms missions (from the mounted root MainGame injects). Empty when
	# no root/missions — Host then reports it via host_failed instead of hanging.
	var map_row := HBoxContainer.new()
	box.add_child(map_row)
	var map_label := Label.new()
	map_label.text = "Map:"
	map_row.add_child(map_label)
	_mission_option = OptionButton.new()
	_mission_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	map_row.add_child(_mission_option)
	_populate_missions()

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
	_host_button.tooltip_text = "Host a game and register it on the gate. Available on OpenNova servers only — we never advertise a host on NovaLogic's live service."
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
	# Hosting registers a game on the gate — allowed only on OpenNova servers, never on NovaLogic's
	# live service. Disable (don't just block-on-click) the Host button when "Original NovaWorld" is the
	# target so it reads as unavailable rather than broken.
	var can_host := _target != NovaWorldSettings.Target.REAL
	_host_button.disabled = not can_host
	if can_host:
		_set_status("Connected. Choose a server or host your own.")
	else:
		_set_status("Connected to NovaWorld. Choose a server to join — hosting is OpenNova-only.")
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
			var idx := _server_list.add_item(format_server_row(row))
			_server_list.set_item_tooltip(idx, server_row_tooltip(row))
	if _server_list.item_count == 0:
		_server_list.add_item("No games are being hosted yet.")
	# A fresh list clears any prior selection.
	_join_button.disabled = true


# "ServerName  (3/16)  AAS  198.51.100.23  [locked]" — name, occupancy, game
# type, the server's address, and a lock marker when passworded or locked.
func format_server_row(row: Dictionary) -> String:
	var name := String(row.get("name", "server"))
	var players := int(row.get("players", 0))
	var max_players := int(row.get("max_players", 0))
	var label := "%s  (%d/%d)" % [name, players, max_players]
	var game_type := String(row.get("game_type", ""))
	if not game_type.is_empty():
		label += "  " + game_type
	# The GSB row's host address (the one the browser pings). 0.0.0.0 means the
	# server did not report one — show nothing rather than a bogus address.
	var ip := String(row.get("ip", ""))
	if not ip.is_empty() and ip != "0.0.0.0":
		label += "  " + ip
	if String(row.get("password", "N")) == "Y" or String(row.get("locked", "N")) == "Y":
		label += "  [locked]"
	return label


# Hover details for a browser row: the mission and locale fields that don't fit
# the one-line label.
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
	var ip := String(row.get("ip", ""))
	if not ip.is_empty() and ip != "0.0.0.0":
		parts.append("Address: %s" % ip)
	return "\n".join(parts)


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
	# Remember what we need for the in-match join — joined_game only carries the resolved address.
	_pending_mission = String(row.get("mission_name", ""))
	_pending_player = player_name
	_set_status("Joining %s..." % String(row.get("name", "server")))
	if _client != null and _client.has_method("join"):
		_client.join(rid)


# The NWJoin handshake resolved the host's in-match address. Hand it (with the stashed mission +
# callsign) up to MainGame, which loads the mission as a co-op JOINER and runs the witnessed in-match
# join through NovaSimulation (the SAME path the LAN browser / NW_LAN_JOIN env use). The lobby client
# does not send the in-match ClientHello itself — the joiner runtime owns it.
func _on_joined_game(host: String, port: int) -> void:
	if _pending_mission.is_empty():
		_set_status("Joined %s:%d, but the host's mission is unknown — cannot enter the match." % [host, port])
		return
	_set_status("Entering %s:%d as %s..." % [host, port, _pending_player])
	var target := JoinTarget.new()
	target.host_ip = host
	target.port = port
	target.mission = _pending_mission
	target.player_name = _pending_player
	join_in_match_requested.emit(target)


# Host a NovaWorld game: hand the gate (the server we're connected to) up to MainGame, which fills in
# the mission + callsign and stands up a browsable listen host (net_session_drive._maybe_start_nw_host). We
# register on the OpenNova gate only — never advertise a host on NovaLogic's live service.
func _on_host_pressed() -> void:
	if _target == NovaWorldSettings.Target.REAL:
		_set_status("Hosting is available on OpenNova servers only.")
		return
	var mission := _selected_mission()
	if mission.is_empty():
		_set_status("No missions are available to host (check the game folder).")
		return
	_set_status("Starting a NovaWorld host...")
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
	if _host_button != null:
		_host_button.disabled = (_target == NovaWorldSettings.Target.REAL)


func _on_close_pressed() -> void:
	if _client != null:
		_client.stop()
	closed.emit()
