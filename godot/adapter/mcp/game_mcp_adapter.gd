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


func mcp_game_control(_action: String) -> Error:
	return ERR_UNAVAILABLE


func get_debug_session() -> NovaDebugSession:
	return null
