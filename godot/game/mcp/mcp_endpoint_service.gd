class_name McpEndpointService
extends Node

## A loopback MCP endpoint's bring-up and lifetime: the log hub
## (McpLogHub.instance while it lives), the Streamable-HTTP McpServer with its
## per-call tool context and log sink, the listen, and the stop when the node
## leaves the tree. The base of the game's endpoint (GameMcpService,
## `opennova-game`) and the editor's (on the editor trunk, PR #665): a
## subclass opens the endpoint under its own names, adds its own children and
## registers its tools, then starts it.

var server: McpServer = null
var log_hub: McpLogHub = null


func _init() -> void:
	# Probes may freeze the shell and the world (PROCESS_MODE_DISABLED) while
	# they capture; the transport keeps polling regardless.
	process_mode = Node.PROCESS_MODE_ALWAYS


func is_running() -> bool:
	return server != null and server.is_running()


## The bound port (0 before a listen).
func get_port() -> int:
	return server.get_port() if server != null else 0


## The hub and the server made and added as a child, not yet listening:
## `node_name` the server node's name, `server_name`, `server_title` and
## `instructions` what the endpoint's initialize answers.
func _open_endpoint(node_name: String, server_name: String, server_title: String,
		instructions: String) -> void:
	log_hub = McpLogHub.new()
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = node_name
	server.server_name = server_name
	server.server_title = server_title
	server.instructions = instructions
	server.context_factory = _make_context
	server.log_sink = _on_server_log
	add_child(server)


## Listen on `port` (loopback only; 0 binds an ephemeral port, which nothing
## could discover: tests only). A bind failure is noted in the hub, the
## endpoint named `label`, and returned.
func _start_endpoint(port: int, label: String) -> Error:
	var err := server.start(port)
	if err != OK:
		log_hub.note("server", "error",
				"%s failed to bind port %d: %s" % [label, port, error_string(err)])
		return err
	log_hub.note_server("listening at %s" % server.get_url())
	return OK


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
