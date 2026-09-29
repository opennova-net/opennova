class_name EditorMcpService
extends Node

## The OpenNova Editor's MCP endpoint (`opennova-editor`, ADR 0046 d10): the same
## loopback Streamable-HTTP transport the game runs (godot/game/mcp/), started by
## `--mcp-port <n>` on the editor's command line or by a test on port 0, over the
## editor's typed seam (EditorApp.request_json / get_view_json). The editor loads
## this script BY PATH (res://editor/mcp/, which the game products never ship), so
## nothing in the shell names its class. The endpoint lives as long as the editor:
## `editor_request {kind: "quit"}` is the stop.

var server: McpServer = null
var log_hub: McpLogHub = null
var tools: EditorMcpTools = null
var app: Node = null


func _init() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS


## Start the endpoint on `port` (loopback only; 0 binds an ephemeral port for tests).
func setup(editor_app: Node, port: int) -> Error:
	if editor_app == null or not editor_app.has_method("request_json") or port < 0 or port > 65535:
		return ERR_INVALID_PARAMETER
	app = editor_app
	log_hub = McpLogHub.new()
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = "EditorMcpServer"
	server.server_name = "opennova-editor"
	server.server_title = "OpenNova Editor"
	server.instructions = (
			"Drive the OpenNova Editor: editor_state reads the project, its requirements, "
			+ "the open documents, the build and the running game; editor_request raises any "
			+ "typed request by kind; editor_document opens and edits records; editor_build "
			+ "packs the project; editor_play runs the game on the build and reports the "
			+ "game's own MCP port, which game_* tools then drive.")
	server.context_factory = _make_context
	server.log_sink = _on_server_log
	add_child(server)
	tools = EditorMcpTools.new(self, app)
	tools.register_all(server.registry)
	var err := server.start(port)
	if err != OK:
		log_hub.note("server", "error",
				"Editor MCP failed to bind port %d: %s" % [port, error_string(err)])
		return err
	log_hub.note_server("listening at %s" % server.get_url())
	return OK


func is_running() -> bool:
	return server != null and server.is_running()


func get_port() -> int:
	return server.get_port() if server != null else 0


func _make_context(args: Dictionary) -> McpToolContext:
	var ctx := McpToolContext.new()
	ctx.tree = get_tree()
	ctx.args = args
	ctx.log_sink = _on_script_log
	return ctx


func _on_script_log(text: String) -> void:
	if log_hub != null:
		log_hub.note("script", "info", text)


func _on_server_log(text: String) -> void:
	if log_hub != null:
		log_hub.note_server(text)


func _exit_tree() -> void:
	if server != null:
		server.stop()
	if McpLogHub.instance == log_hub:
		McpLogHub.instance = null
