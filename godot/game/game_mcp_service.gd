class_name GameMcpService
extends Node

## The runtime MCP endpoint (`opennova-game`): a loopback Streamable-HTTP
## server the game process runs when launched with `--mcp-port <n>`
## (LaunchFlags; ADR 0041). A normal standalone launch carries no such flag
## and starts nothing. The endpoint lives as long as the game: `game_control
## quit` is the stop. ONED runs no MCP of its own (ADR 0037); this is the
## runtime's, driven from outside by agents and the scripts/mcp clients.

var server: McpServer = null
var log_hub: McpLogHub = null
var tools: GameMcpTools = null
var game_adapter: GameMcpAdapter = null
## The game_probe runner (ADR 0041), a child so its watchdog ticks with the
## transport and a frozen shell never stalls it.
var probe_runner: ProbeRunner = null


func _init() -> void:
	# Probes may freeze the shell and the world (PROCESS_MODE_DISABLED) while
	# they capture; the transport keeps polling regardless.
	process_mode = Node.PROCESS_MODE_ALWAYS


## Start the endpoint on `port` (loopback only; 0 binds an ephemeral port —
## tests only, since nothing could discover it).
func setup(adapter: GameMcpAdapter, port: int) -> Error:
	if adapter == null or port < 0 or port > 65535:
		return ERR_INVALID_PARAMETER
	game_adapter = adapter
	log_hub = McpLogHub.new()
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = "GameMcpServer"
	server.server_name = "opennova-game"
	server.server_title = "OpenNova Game Runtime"
	server.instructions = (
			"Inspect and control the debug-launched real game. For visual parity, "
			+ "wait until MainGame.is_world_loading() is false and a gameplay camera "
			+ "is current, inspect game_render_diagnostics, then use "
			+ "game_capture_bundle for a lossless PNG and correlated JSON sidecar.")
	server.context_factory = _make_context
	server.log_sink = _on_server_log
	add_child(server)
	probe_runner = ProbeRunner.new()
	probe_runner.name = "ProbeRunner"
	probe_runner.seams = adapter.get_shell_seams()
	add_child(probe_runner)
	tools = GameMcpTools.new(self, game_adapter)
	tools.register_all(server.registry)
	var err := server.start(port)
	if err != OK:
		log_hub.note("server", "error",
				"Runtime MCP failed to bind port %d: %s" % [port, error_string(err)])
		return err
	log_hub.note_server("listening at %s" % server.get_url())
	return OK


func is_running() -> bool:
	return server != null and server.is_running()


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
