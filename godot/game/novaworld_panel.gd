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
var _close_button: Button
var _target_option: OptionButton
var _target: int = NovaWorldSettings.Target.OPENNOVA


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

	_server_list = ItemList.new()
	_server_list.custom_minimum_size = Vector2(440, 220)
	_server_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_server_list)

	var buttons := HBoxContainer.new()
	box.add_child(buttons)

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
	_set_status("Connecting...")
	_create_client()


func _set_status(text: String) -> void:
	if _status_label != null:
		_status_label.text = text


# Map the protocol state to plain language (no wire jargon for the player).
func _on_state_changed(state: int) -> void:
	match state:
		0: _set_status("Ready.")               # Idle
		1: _set_status("Finding the server...") # GateProbing
		2, 3: _set_status("Signing in...")     # SessionHello / SessionJoin
		4: _set_status("Connected.")           # Connected
		5: _set_status("Disconnected.")        # Disconnected
		_: _set_status("Connection problem.")  # Error


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


# Fill the browser from the server list. The live row source (GSB) lands with
# the browse leg of the client session; until then this reflects what the
# client exposes.
func _refresh_servers() -> void:
	_server_list.clear()
	if _client != null and _client.has_method("get_server_rows"):
		for row in _client.get_server_rows():
			_server_list.add_item(String(row.get("name", "server")))
	if _server_list.item_count == 0:
		_server_list.add_item("No games are being hosted yet.")


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
