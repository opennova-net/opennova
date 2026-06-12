class_name EditorMcpService
extends Node

## ONED's embedded MCP (agent) service: owns the McpServer node, the log hub,
## the built-in tool catalog, and the persisted agent-defined tools. Started
## by TerrainEditor at boot (per McpSettings — on by default, off headless or
## with --mcp-off), toggled from the Settings popup.
##
## Security posture, stated plainly: execute_script and custom tools are
## arbitrary local code execution by design. The server binds 127.0.0.1 only
## and rejects non-local Origin/Host headers, but anything on this machine
## that can POST to the port can drive the editor.

const SERVER_JSON_PATH := "user://oned_mcp/server.json"

var server: McpServer
var log_hub: McpLogHub
var dynamic_tools: McpDynamicTools
var tool_host: EditorMcpTools
var editor: Node = null
var shell: Node = null

var _last_error := ""


## Build the registry and (per settings) start listening. Called once from
## TerrainEditor._ready after the workstation shell is bound.
func setup(editor_node: Node, shell_node: Node) -> void:
	editor = editor_node
	shell = shell_node
	log_hub = McpLogHub.new()
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = "McpServer"
	server.instructions = EditorMcpTools.INSTRUCTIONS
	server.context_factory = Callable(self, "_make_context")
	server.log_sink = Callable(self, "_on_server_log")
	add_child(server)
	tool_host = EditorMcpTools.new(self)
	tool_host.register_all(server.registry)
	dynamic_tools = McpDynamicTools.new(server.registry)
	var report := dynamic_tools.load_all()
	for failure: Dictionary in report["failed"]:
		log_hub.note("server", "warn", "custom tool %s failed to load: %s" % [failure["file"], failure["errors"]])
	if McpSettings.resolve_enabled():
		start(McpSettings.resolve_port())


func start(port: int) -> Error:
	if server.is_running():
		return OK
	var err := server.start(port)
	if err != OK:
		_last_error = "MCP port %d in use — agent server not started (use --mcp-port N or change it in Settings)." % port \
				if err == ERR_ALREADY_IN_USE else "MCP server failed to start: %s." % error_string(err)
		log_hub.note("server", "error", _last_error)
		_status(_last_error)
		return err
	_last_error = ""
	var url := server.get_url()
	print("OpenNova MCP: ", url)
	log_hub.note_server("listening at %s" % url)
	_write_server_json()
	return OK


func stop() -> void:
	if server != null and server.is_running():
		server.stop()
		log_hub.note_server("stopped")
	_remove_server_json()


func is_running() -> bool:
	return server != null and server.is_running()


## Status line for the Settings popup.
func get_status_text() -> String:
	if is_running():
		return "Running at %s" % server.get_url()
	if not _last_error.is_empty():
		return _last_error
	return "Stopped"


## The Settings toggle: persists the choice and starts/stops live.
func set_enabled(value: bool) -> void:
	McpSettings.set_enabled(value)
	if value and not is_running():
		start(McpSettings.resolve_port())
	elif not value and is_running():
		stop()


## The Settings port field: persists and rebinds when running.
func apply_port(port: int) -> void:
	McpSettings.set_port(port)
	if is_running():
		stop()
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
