class_name GameMcpCatalog
extends RefCounted

## Stable tool definitions shared by the game-side handlers and ONED's proxy.
## Keeping the schemas here prevents the editor and runtime catalogs drifting
## while their handlers remain correctly separated by the process boundary.

const SCREENSHOT_TIMEOUT_MS := 60_000

## The one list of public game_control actions. The game_control schema enum,
## ONED's proxy allowlist, and the GameDebugAdapter contract test all read it, so
## the three cannot drift; GameDebugAdapter.mcp_game_control implements each.
const PUBLIC_GAME_CONTROL_ACTIONS: Array[String] = [
	"pause",
	"resume",
	"step",
	"return_to_menu",
	"quit",
]


static func definitions() -> Array[McpToolDef]:
	return [
		McpToolDef.make("game_state",
			"Read the editor-launched game's shell, mission, runtime and debug-connection state.",
			{}, [], false),
		McpToolDef.make("game_entities",
			"Discover the game's current rendered entity view. op=list returns a bounded page in "
			+ "discovery order; op=inspect returns the full public debug card for one discovery "
			+ "index. `index` and `view_index` describe that transient view, not an edit target. "
			+ "Only rows with editable=true can be mutated; pass their returned `ai_index` to "
			+ "set_entity_health or set_entity_position. Non-AI and joiner rows are read-only.",
			{
				"op": {"type": "string", "enum": ["list", "inspect"]},
				"offset": {
					"type": "integer",
					"minimum": 0,
					"default": 0,
					"description": "Offset into the current discovery-order entity view.",
				},
				"limit": {
					"type": "integer",
					"minimum": 1,
					"maximum": 128,
					"default": 64,
					"description": "Maximum discovery rows to return.",
				},
				"index": {
					"type": "integer",
					"minimum": 0,
					"description": "Transient discovery index returned by game_entities op=list.",
				},
			}, ["op"], false),
		McpToolDef.make("game_control",
			"Control the real game process. action is %s. " % ", ".join(
					PUBLIC_GAME_CONTROL_ACTIONS)
			+ "Pause/step retain the runtime's multiplayer transport restrictions.",
			{
				"action": {
					"type": "string",
					"enum": PUBLIC_GAME_CONTROL_ACTIONS,
				},
			}, ["action"]),
		McpToolDef.make("game_debug",
			"Use the same UI-free debug catalog as F3. op=list returns controls; get reads one live "
			+ "control; set writes a value; invoke presses an action; snapshot captures the current "
			+ "runtime catalog. Authority-changing writes require confirm_authority=true. "
			+ "Action args: teleport_local_player {position:[x,y,z],yaw_deg?,pitch_deg?}; "
			+ "set_entity_health {entity,health}; set_entity_position {entity,position:[x,y,z]}, "
			+ "where entity is the editable row's game_entities `ai_index` (never its discovery "
			+ "`index` or `view_index`; non-AI and joiner rows are read-only); "
			+ "runtime_transport {action}; set_mission_variable {index,value}."
			+ " Audio actions: set_audio_bus_volume {bus,volume_db}; "
			+ "set_audio_bus_mute {bus,muted}; set_audio_bus_solo {bus,soloed}; "
			+ "set_audio_bus_bypass {bus,bypassed}.",
			{
				"op": {
					"type": "string",
					"enum": ["list", "get", "set", "invoke", "snapshot"],
				},
				"id": {"type": "string"},
				"value": {},
				"args": {
					"description": "Action arguments; use the action-specific object documented "
					+ "above. Entity mutations require game_entities.ai_index from a row whose "
					+ "editable field is true.",
				},
				"page": {"type": "string"},
				"filter": {"type": "string"},
				"confirm_authority": {"type": "boolean", "default": false},
			}, ["op"]),
		McpToolDef.make("game_screenshot",
			"Capture the real game window, including F3 when it is open. Requested expensive "
			+ "debug views are activated for this capture, then suspended again.",
			{
				"max_dim": {"type": "integer", "minimum": 64, "maximum": 4096, "default": 1280},
				"format": {"type": "string", "enum": ["webp", "png"], "default": "webp"},
				"quality": {"type": "number", "minimum": 0.1, "maximum": 1.0, "default": 0.8},
			}, [], true, SCREENSHOT_TIMEOUT_MS),
		McpToolDef.make("game_logs",
			"Read runtime MCP and engine log entries from the launched game.",
			{
				"cursor": {"type": "integer", "minimum": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": 2000, "default": 200},
				"sources": {
					"type": "array",
					"items": {"type": "string", "enum": ["server", "script", "engine"]},
				},
			}, [], false),
	]


static func definition(name: String) -> McpToolDef:
	for def in definitions():
		if def.name == name:
			return def
	return null
