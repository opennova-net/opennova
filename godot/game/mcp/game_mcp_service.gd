class_name GameMcpService
extends McpEndpointService

## The runtime MCP endpoint (`opennova-game`): a loopback Streamable-HTTP
## server the game process runs when launched with `--mcp-port <n>`
## (LaunchFlags; ADR 0041). A normal standalone launch carries no such flag
## and starts nothing. The endpoint lives as long as the game: `game_control
## quit` is the stop. This is the
## runtime's, driven from outside by agents and the scripts/mcp clients.
## GameDebugAdapter.start_runtime_endpoint loads this script BY PATH: the
## transport (godot/game/mcp/) leaves the Runtime export (ADR 0043 d12), so
## no shell script names its class. McpEndpointService carries the hub, the
## server and the listen.

var tools: GameMcpTools = null
var game_adapter: GameMcpAdapter = null
## The game_probe runner (ADR 0041), a child so its watchdog ticks with the
## transport and a frozen shell never stalls it.
var probe_runner: ProbeRunner = null


## Start the endpoint on `port` (loopback only; 0 binds an ephemeral port —
## tests only, since nothing could discover it).
func setup(adapter: GameMcpAdapter, port: int) -> Error:
	if adapter == null or port < 0 or port > 65535:
		return ERR_INVALID_PARAMETER
	game_adapter = adapter
	_open_endpoint("GameMcpServer", "opennova-game", "OpenNova Game Runtime", (
			"Inspect and control the debug-launched real game. For visual parity, "
			+ "wait until MainGame.is_world_loading() is false and a gameplay camera "
			+ "is current, inspect game_render_diagnostics, then use "
			+ "game_capture_bundle for a lossless PNG and correlated JSON sidecar."))
	probe_runner = ProbeRunner.new()
	probe_runner.name = "ProbeRunner"
	probe_runner.shell = adapter.get_shell()
	# The probe model ships without the transport (ADR 0043 d12), so the
	# runner's log lines reach the hub through this sink, never by name.
	probe_runner.log_sink = _on_probe_log
	add_child(probe_runner)
	tools = GameMcpTools.new(self, game_adapter)
	tools.register_all(server.registry)
	return _start_endpoint(port, "Runtime MCP")


func _on_probe_log(text: String) -> void:
	if log_hub != null:
		log_hub.note("probe", "info", text)
