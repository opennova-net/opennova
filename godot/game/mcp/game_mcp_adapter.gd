class_name GameMcpAdapter
extends Node

## Engine-layer contract consumed by the runtime MCP transport. The game
## application owns the concrete adapter; tools depend only on this public seam.


func get_mcp_game_state() -> Variant:
	return {}


func get_mcp_game_entities(_offset: int, _limit: int) -> Variant:
	return {}


func get_mcp_game_entity(_index: int) -> Variant:
	return {}


## Exact read-only renderer state at the game-shell seam. The concrete adapter
## samples the current GameWorld; transports never inspect its scene internals.
func get_mcp_render_diagnostics() -> Variant:
	return {}


## Durable lossless capture at the same seam. Successful implementations return
## bundle metadata plus a transport-only `image_bytes` PackedByteArray.
func capture_mcp_render_bundle(
		_args: Dictionary,
		_cancel_requested: Callable = Callable()) -> Variant:
	return {}


func mcp_game_control(_action: String) -> Error:
	return ERR_UNAVAILABLE


func mcp_game_menu(_args: Dictionary) -> Variant:
	return {}


func get_debug_controls() -> DebugControls:
	return null


## The shell seams the probe runner drives (GameShellSeams); null in a
## shell that runs no probes.
func get_shell_seams() -> GameShellSeams:
	return null
