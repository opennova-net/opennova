class_name EditorMcpService
extends Node

## ONED's embedded MCP (agent) service: owns the McpServer node, the log hub,
## and the curated tool catalog (editor-wide tools + mission authoring).
## Started by TerrainEditor at boot (per McpSettings — on by default, off
## headless or with --mcp-off), toggled from the Settings popup.
##
## Security posture: the tool surface is a FIXED, curated catalog routed
## through the editor's own code paths — there is no script or code
## execution. The server binds 127.0.0.1 only and rejects non-local
## Origin/Host headers; any local process can still drive the editor's
## documents, so disable it in Settings on shared machines.

const SERVER_JSON_PATH := "user://oned_mcp/server.json"

signal status_changed

var server: McpServer
var log_hub: McpLogHub
var tools: EditorMcpTools
var mission_tools: EditorMcpMissionTools
var menu_tools: EditorMcpMenuTools
var object_tools: EditorMcpObjectTools
var game_tools: EditorMcpGameTools
var editor: Node = null
var shell: Node = null
var game_run_bridge: EditorGameRunBridge = null

var _last_error := ""
var _runtime_transition_generation := 0
var _runtime_disable_pending := false
var _runtime_disable_status := ""


## Build the registry and (per settings) start listening. Called once from
## TerrainEditor._ready after the workstation shell is bound.
func setup(
		editor_node: Node,
		shell_node: Node,
		game_session: ShellGameSession = null,
		run_game: Callable = Callable(),
		stop_game: Callable = Callable()) -> void:
	editor = editor_node
	shell = shell_node
	game_run_bridge = EditorGameRunBridge.new(
			game_session, run_game, stop_game, runtime_debug_state_changed)
	log_hub = McpLogHub.new()
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = "McpServer"
	server.instructions = EditorMcpTools.INSTRUCTIONS
	server.context_factory = Callable(self, "_make_context")
	server.log_sink = Callable(self, "_on_server_log")
	add_child(server)
	tools = EditorMcpTools.new(self)
	tools.register_all(server.registry)
	mission_tools = EditorMcpMissionTools.new(self)
	mission_tools.register_all(server.registry)
	menu_tools = EditorMcpMenuTools.new(self)
	menu_tools.register_all(server.registry)
	object_tools = EditorMcpObjectTools.new(self)
	object_tools.register_all(server.registry)
	game_tools = EditorMcpGameTools.new(self, game_run_bridge)
	game_tools.register_all(server.registry)
	_bind_game_session_status()
	if McpSettings.resolve_enabled():
		start(McpSettings.resolve_port())


func start(port: int) -> Error:
	if server.is_running():
		_authorize_runtime_debug()
		return OK
	var err := server.start(port)
	if err != OK:
		_last_error = "MCP port %d in use — agent server not started (use --mcp-port N or change it in Settings)." % port \
				if err == ERR_ALREADY_IN_USE else "MCP server failed to start: %s." % error_string(err)
		log_hub.note("server", "error", _last_error)
		_status(_last_error)
		_begin_runtime_debug_disable()
		return err
	_last_error = ""
	var url := server.get_url()
	log_hub.note_server("listening at %s" % url)
	_write_server_json()
	_authorize_runtime_debug()
	return OK


func stop() -> void:
	_stop_listener()
	_last_error = ""
	_begin_runtime_debug_disable()


func _authorize_runtime_debug() -> void:
	_runtime_transition_generation += 1
	_runtime_disable_pending = false
	_runtime_disable_status = ""
	if game_tools != null:
		game_tools.enable_runtime_debug()
	status_changed.emit()


func _begin_runtime_debug_disable() -> void:
	_runtime_transition_generation += 1
	var generation := _runtime_transition_generation
	_runtime_disable_status = ""
	if game_tools == null:
		_runtime_disable_pending = false
		status_changed.emit()
		return
	_runtime_disable_pending = true
	status_changed.emit()
	_finish_runtime_debug_disable(generation)


func _finish_runtime_debug_disable(generation: int) -> void:
	var outcome: Variant = await game_tools.disable_runtime_debug()
	if generation != _runtime_transition_generation:
		return
	_runtime_disable_pending = false
	var outcome_error := FAILED
	if not (outcome is Dictionary):
		_runtime_disable_status = "Agent server stopped, but runtime debug shutdown returned an invalid result."
	else:
		outcome_error = int(outcome.get("error", FAILED))
		_runtime_disable_status = String(outcome.get("message", ""))
		if outcome_error != OK \
				and _runtime_disable_status.is_empty():
			_runtime_disable_status = "Agent server stopped, but its runtime debug endpoint may still be active."
	if not _runtime_disable_status.is_empty():
		log_hub.note("server",
				"warn" if outcome_error == OK else "error",
				_runtime_disable_status)
		_status(_runtime_disable_status)
	status_changed.emit()


func _stop_listener() -> void:
	if server != null and server.is_running():
		server.stop()
		log_hub.note_server("stopped")
	_remove_server_json()


func is_running() -> bool:
	return server != null and server.is_running()


## Status line for the Settings popup.
func get_status_text() -> String:
	if is_running():
		if game_tools != null \
				and bool(game_tools.current_runtime_needs_relaunch()):
			return "Running at %s; relaunch the current game to enable runtime debug." \
					% server.get_url()
		return "Running at %s" % server.get_url()
	if _runtime_disable_pending:
		return "Stopping Agent server and runtime debug..."
	if not _runtime_disable_status.is_empty():
		return _runtime_disable_status
	if not _last_error.is_empty():
		return _last_error
	return "Stopped"


## Called by the runtime adapter when an in-flight retirement wins a race with
## re-enable and by managed-run lifecycle edges that change handshake identity.
func runtime_debug_state_changed() -> void:
	status_changed.emit()


func _bind_game_session_status() -> void:
	var session: ShellGameSession = game_run_bridge.session \
			if game_run_bridge != null else null
	if session == null:
		return
	if not session.run_started.is_connected(_on_game_run_started):
		session.run_started.connect(_on_game_run_started)
	if not session.run_stopped.is_connected(_on_game_run_stopped):
		session.run_stopped.connect(_on_game_run_stopped)


func _on_game_run_started(
		_run_id: String,
		_descriptor_path: String,
		_pid: int) -> void:
	runtime_debug_state_changed()


func _on_game_run_stopped(
		_run_id: String,
		_descriptor_path: String) -> void:
	runtime_debug_state_changed()


## The Settings toggle: persists the choice and starts/stops live.
func set_enabled(value: bool) -> void:
	McpSettings.set_enabled(value)
	if value and not is_running():
		start(McpSettings.resolve_port())
	elif not value:
		stop()


## The Settings port field: persists and rebinds when running.
func apply_port(port: int) -> void:
	McpSettings.set_port(port)
	if is_running():
		# Rebinding the editor listener is not a security-disable transition;
		# preserve the already-authorized child endpoint when the new port binds.
		_stop_listener()
		start(McpSettings.resolve_port())


func _make_context(args: Dictionary) -> McpToolContext:
	var ctx := McpToolContext.new()
	ctx.editor = editor
	ctx.shell = shell
	ctx.tree = get_tree()
	ctx.args = args
	ctx.log_sink = Callable(self, "_on_script_log")
	return ctx


func _on_script_log(text: String) -> void:
	log_hub.note("script", "info", text)


func _on_server_log(text: String) -> void:
	log_hub.note_server(text)


func _status(text: String) -> void:
	if shell != null and shell.has_method("show_status_message"):
		shell.show_status_message(text, 6.0)


func _write_server_json() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SERVER_JSON_PATH.get_base_dir()))
	var file := FileAccess.open(SERVER_JSON_PATH, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({
		"url": server.get_url(),
		"port": server.get_port(),
		"transport": "streamable-http",
		"pid": OS.get_process_id(),
		"version": str(ProjectSettings.get_setting("application/config/version", "0.0.0")),
		"started_at": Time.get_datetime_string_from_system(),
	}, "\t"))
	file.close()


func _remove_server_json() -> void:
	if FileAccess.file_exists(SERVER_JSON_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(SERVER_JSON_PATH))


func _exit_tree() -> void:
	stop()
	if McpLogHub.instance == log_hub:
		McpLogHub.instance = null
