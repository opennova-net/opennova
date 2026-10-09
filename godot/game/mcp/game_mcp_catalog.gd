class_name GameMcpCatalog
extends RefCounted

## Stable tool definitions for the game-side runtime handlers.

## game_probe op=status may long-poll up to PROBE_STATUS_WAIT_MAX_MS; its
## budget stays clear of that.
const PROBE_STATUS_WAIT_MAX_MS := 30_000
const PROBE_TIMEOUT_MS := 45_000

## The one list of public game_control actions. The game_control schema enum,
## Handler registration and the GameDebugAdapter contract test both read this
## list; GameDebugAdapter.mcp_game_control implements each action.
const PUBLIC_GAME_CONTROL_ACTIONS: Array[String] = [
	"pause",
	"resume",
	"step",
	"open_ingame_menu",
	"open_armory",
	"return_to_menu",
	"quit",
]


static func definitions() -> Array[McpToolDef]:
	return [
		McpToolDef.make("game_state",
			"Read the game's shell, mission, runtime and debug-connection state.",
			{}, [], false),
		McpToolDef.make("game_entities",
			"Discover the game's current rendered entity view. op=list returns a bounded page in "
			+ "discovery order; op=inspect returns the full public debug card for one discovery "
			+ "index. `index` and `view_index` describe that transient view, not an edit target. "
			+ "Rows with editable=true take set_entity_health / set_entity_position by their "
			+ "returned `ai_index`; every row with a world card takes set_entity_item_attrib by "
			+ "its `wire_handle`. Joiner rows are read-only.",
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
			+ "Pause/step retain the runtime's multiplayer transport restrictions. "
			+ "open_ingame_menu raises the ESC pause overlay (game.mnu) over a loaded "
			+ "world — game_menu drives it from there; open_armory opens the in-world "
			+ "armory (weapon.mnu WEAPON) over live play — presenter-owned, so inspect "
			+ "it via game_screenshot, and resume hands play back from either screen.",
			{
				"action": {
					"type": "string",
					"enum": PUBLIC_GAME_CONTROL_ACTIONS,
				},
			}, ["action"]),
		McpToolDef.make("game_debug",
			"Use the typed debug-control table the F3 windows' controls also invoke. op=list returns controls; get reads one live "
			+ "control; set writes a value; invoke presses an action; snapshot captures the current "
			+ "runtime catalog. Authority-changing writes require confirm_authority=true. "
			+ "Action args: teleport_local_player {position:[x,y,z],yaw_deg?,pitch_deg?}; "
			+ "set_entity_health {entity,health}; set_entity_position {entity,position:[x,y,z]}, "
			+ "where entity is the editable row's game_entities `ai_index` (never its discovery "
			+ "`index` or `view_index`; non-AI and joiner rows are read-only); "
			+ "set_entity_item_attrib {entity,attrib,attrib2} writes both items.def attrib words "
			+ "on one entity, where entity is the row's `wire_handle` (brainless rows included); "
			+ "runtime_transport {action}; set_mission_variable {index,value}."
			+ " Audio actions: set_audio_bus_volume {bus,volume_db}; "
			+ "set_audio_bus_mute {bus,muted}; set_audio_bus_solo {bus,soloed}; "
			+ "set_audio_bus_bypass {bus,bypassed}."
			+ " Automation actions: deploy_pick {zone} (0 = Default Spawn); "
			+ "set_viewmodel_weapon {weapon} (the viewmodel and its FSM only: the round fired "
			+ "stays the equipped slot's); clear_viewmodel_weapon; "
			+ "kill_group {group}; crew_vehicle {occupant_ssn,vehicle_ssn}; "
			+ "crew_local_player {vehicle_ssn}; local_player_look {dx_px,dy_px}; "
			+ "plus the net_joiner_diagnostics check.",
			{
				"op": {
					"type": "string",
					"enum": ["list", "get", "set", "invoke", "snapshot"],
				},
				"id": {"type": "string"},
				"value": {},
				"args": {
					"description": "Action arguments; use the action-specific object documented "
					+ "above (op=list publishes every action's args schema: name, kind, range, "
					+ "default). Entity mutations require game_entities.ai_index from a row whose "
					+ "editable field is true.",
				},
				"page": {"type": "string"},
				"filter": {"type": "string"},
				"confirm_authority": {"type": "boolean", "default": false},
			}, ["op"]),
		McpToolDef.make("game_render_diagnostics",
			"Read one frame-correlated rendering snapshot from the real game: exact camera "
			+ "and projection matrices, environment/weather/sky/water state, active lights, "
			+ "directional-shadow configuration, and root/shadow/reflection pass counts. "
			+ "This is a read-only diagnostic; capture a durable PNG + JSON pair with "
			+ "game_capture_bundle.",
			{}, [], false),
		McpToolDef.make("game_capture_bundle",
			"Capture the real game's next completed render frame as a full-resolution, "
			+ "lossless PNG and a frame-correlated render-diagnostics JSON sidecar under "
			+ "user://render-captures. world_only hides game Canvas UI for the captured "
			+ "frame and restores it immediately. The result always returns absolute paths, "
			+ "dimensions and SHA-256; include_image also returns the PNG as MCP image content.",
			{
				"label": {
					"type": "string",
					"maxLength": 80,
					"default": "render",
					"description": "Short fixture/pose label used in the generated filenames.",
				},
				"settle_frames": {
					"type": "integer",
					"minimum": 0,
					"maximum": 180,
					"default": 2,
					"description": "Completed process frames to wait before the captured draw.",
				},
				"world_only": {"type": "boolean", "default": true},
				"include_image": {"type": "boolean", "default": true},
			}, [], true, McpScreenshot.CAPTURE_TIMEOUT_MS),
		McpToolDef.make("game_menu",
			"Drive the compiled menu. op=state returns the current file/screen + widget rows "
			+ "(design-space rects); press activates a named widget through the real mouse pump "
			+ "(press+release at its center); press_at pumps at design coords x,y (list rows, "
			+ "combo popups, spin arrows) and returns the hit widget index; move_at (hold_at) "
			+ "pumps one sample there with the button up (held), as a motion does (the hover and "
			+ "its sounds); state's sounds are the last 16 window sounds the menu played "
			+ "({seq, file, trigger}); click_at feeds a REAL "
			+ "left click at design coords through Godot input dispatch (mouse filters and mounts "
			+ "apply; lands next frame); key feeds one key event (keycode + optional unicode); "
			+ "screen jumps within the open .mnu; open loads another .mnu file.",
			{
				"op": {
					"type": "string",
					"enum": ["state", "press", "press_at", "move_at", "hold_at", "click_at", "key", "screen",
						"open"],
				},
				"name": {
					"type": "string",
					"description": "Widget name (press) or screen name (screen).",
				},
				"widgets": {"type": "boolean", "default": true},
				"x": {"type": "number"},
				"y": {"type": "number"},
				"keycode": {"type": "integer"},
				"unicode": {"type": "integer"},
				"file": {"type": "string"},
				"target_screen": {"type": "string", "default": ""},
			}, ["op"]),
		McpToolDef.make("game_screenshot",
			"Capture the real game window, including F3 when it is open and any "
			+ "debug views that are toggled on.",
			McpScreenshot.tool_schema(), [], true, McpScreenshot.CAPTURE_TIMEOUT_MS),
		McpToolDef.make("game_logs",
			"Read runtime MCP, probe, engine (the native io::log ring) and godot "
			+ "(the tailed Godot log file) log entries from the launched game.",
			{
				"cursor": {"type": "integer", "minimum": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": 2000, "default": 200},
				"sources": {
					"type": "array",
					"items": {"type": "string", "enum": ["server", "script", "engine", "godot", "probe"]},
				},
			}, [], false),
		McpToolDef.make("game_probe",
			"Run the registered runtime probes (docs/mcp.md). op=list returns the catalog "
			+ "with each probe's input_schema, preconditions and availability; op=run starts "
			+ "one (args validated against its schema; one probe at a time; refused while a "
			+ "serial tool call is in flight) and returns its run_id; op=status reads a run's "
			+ "state, the log lines after cursor (long-polling up to wait_ms while it runs), "
			+ "progress, verdict and artifacts; op=cancel asks the running probe to stop. "
			+ "run_id defaults to the active or most recent run.",
			{
				"op": {"type": "string", "enum": ["list", "run", "status", "cancel"]},
				"name": {"type": "string", "description": "The probe to run (op=run)."},
				"args": {
					"type": "object",
					"description": "The probe's typed arguments (op=run), per its input_schema.",
				},
				"run_id": {"type": "string"},
				"cursor": {
					"type": "integer",
					"minimum": 0,
					"default": 0,
					"description": "Return log lines with seq greater than this (op=status).",
				},
				"wait_ms": {
					"type": "integer",
					"minimum": 0,
					"maximum": PROBE_STATUS_WAIT_MAX_MS,
					"default": 0,
					"description": "How long op=status may wait for new lines or the end of the run.",
				},
			}, ["op"], false, PROBE_TIMEOUT_MS),
	]

