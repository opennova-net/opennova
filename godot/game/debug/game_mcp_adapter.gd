class_name GameMcpAdapter
extends Node

## Engine-layer contract consumed by the runtime MCP transport. The game
## application owns the concrete adapter (GameDebugAdapter extends this seam);
## tools depend only on this public surface. It lives beside the game shell's
## debug code, not with the transport, because the shell references it in
## every flavour while the transport (godot/game/mcp/) leaves the Runtime
## export (ADR 0043 d12).


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


## The typed debug-control table (DebugControlTable, ADR 0043 d12) game_debug
## drives; null in a shell that carries none.
func get_debug_controls() -> DebugControlTable:
	return null


## The shell's runtime block of the game_debug op=snapshot payload (the same
## record game_state's `runtime` extends); {} in a shell without a runtime.
func runtime_status() -> Dictionary:
	return {}


## The game shell the probe runner drives (GameShell); null in a
## shell that runs no probes.
func get_shell() -> GameShell:
	return null
