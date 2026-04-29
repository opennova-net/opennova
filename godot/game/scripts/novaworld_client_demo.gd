extends Control

## Minimal demo wiring of the NovaWorldClient GDExtension.
##
## Polls the standalone novaworld server's HTTP /api/lobbies for the live
## connection list, renders it in an ItemList, and on "Connect" kicks off
## the UDP gate-probe -> session-hello -> session-join handshake via
## NovaWorldClient.

@export var server_host: String = "127.0.0.1"
@export var gate_port: int = 7597
@export var http_port: int = 8080

@onready var lobby_list: ItemList = %LobbyList
@onready var status_label: Label = %StatusLabel
@onready var connect_button: Button = %ConnectButton
@onready var refresh_button: Button = %RefreshButton

var _http: HTTPRequest
var _client: NovaWorldClient

func _ready() -> void:
	_http = HTTPRequest.new()
	add_child(_http)
	_http.request_completed.connect(_on_lobbies_loaded)

	_client = NovaWorldClient.new()
	_client.host = server_host
	_client.gate_port = gate_port
	_client.player_name = "GodotDemo"
	add_child(_client)
	_client.connected.connect(_on_connected)
	_client.disconnected.connect(_on_disconnected)
	_client.state_changed.connect(_on_state_changed)
	_client.error_occurred.connect(_on_error)
	_client.server_info_received.connect(_on_server_info)

	connect_button.pressed.connect(_on_connect_pressed)
	refresh_button.pressed.connect(_refresh_lobbies)
	_refresh_lobbies()

func _on_connect_pressed() -> void:
	if _client.is_session_active():
		_client.stop()
	else:
		_client.start()

func _refresh_lobbies() -> void:
	var url := "http://%s:%d/api/lobbies" % [server_host, http_port]
	_http.request(url)

func _on_lobbies_loaded(_result: int, code: int, _headers: PackedStringArray, body: PackedByteArray) -> void:
	lobby_list.clear()
	if code != 200:
		lobby_list.add_item("(server unreachable — HTTP %d)" % code)
		return
	var json := JSON.new()
	if json.parse(body.get_string_from_utf8()) != OK:
		lobby_list.add_item("(JSON parse failed)")
		return
	var data: Dictionary = json.data
	var entries: Array = data.get("lobbies", [])
	if entries.is_empty():
		lobby_list.add_item("(no connections — start a Godot client to see one here)")
		return
	for e in entries:
		var label := "%s — id=0x%08x — state=%s — pn=%s" % [
			e.get("addr", "?"), e.get("id", 0), e.get("state", "?"), e.get("pn", "?")
		]
		lobby_list.add_item(label)

func _on_connect_state_text() -> String:
	match _client.get_state():
		NovaWorldClient.STATE_IDLE: return "idle"
		NovaWorldClient.STATE_GATE_PROBING: return "gate_probing"
		NovaWorldClient.STATE_SESSION_HELLO: return "session_hello"
		NovaWorldClient.STATE_SESSION_JOIN: return "session_join"
		NovaWorldClient.STATE_CONNECTED: return "connected"
		NovaWorldClient.STATE_DISCONNECTED: return "disconnected"
		NovaWorldClient.STATE_ERROR: return "error"
	return "unknown"

func _on_state_changed(_state: int) -> void:
	status_label.text = "state: %s" % _on_connect_state_text()
	connect_button.text = "Disconnect" if _client.is_session_active() else "Connect"

func _on_connected() -> void:
	status_label.text = "connected"
	_refresh_lobbies()

func _on_disconnected(reason: String) -> void:
	status_label.text = "disconnected (%s)" % reason
	_refresh_lobbies()

func _on_error(msg: String) -> void:
	status_label.text = "error: %s" % msg

func _on_server_info(info: Dictionary) -> void:
	print("[demo] gate response: ", info)
