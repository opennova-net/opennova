class_name EditorMcpTools
extends RefCounted

## The editor-wide MCP tool catalog for ONED: thin handlers over the editor's
## existing surface (EditorWorkstation facade, EditorWorkspace capability
## hooks, the Nova* GDExtension classes). Mission authoring lives in
## EditorMcpMissionTools. Descriptions are agent-facing prompt text — they
## carry the conventions (path resolution, dirty rules) so every tool teaches
## the protocol. The surface is a fixed, curated catalog: every mutation
## routes through the editor's own code paths, and there is no script/code
## execution.

const INSTRUCTIONS := """ONED — the OpenNova editor (Godot-based). You are connected to a live editor a human may be watching.

Start with get_editor_state: the mounted resource root, every workspace (open document, dirty, capabilities), the active workspace, managed game-run status, and a log cursor.

Conventions:
- Paths: a bare resource name ("alpha.bms", resolved case-insensitively in the mounted resource root, including inside PFF archives) or an absolute path. Results carry the resolved path.
- Coordinates: tools take WORLD-space x/z (y is up) and ground everything on the terrain for you — never guess or supply heights. Entity records read back are BMS mission-space (z up); describe_api(topic="coordinates") has the mapping.
- Edits mark documents dirty and are undoable (undo/redo tools). NEVER save unless the user explicitly asked — saving is save_mission's job alone.
- Running never writes editor state. F5/run_game mode="game" launches the saved loose assets; F6/mode="mission" launches the current saved top-level loose .bms. Unsaved edits are deliberately excluded.
- After anything surprising, call get_logs — engine errors and editor status messages land there.

Typical flows:
- Study assets: list_assets(kind=...) -> describe_asset(path) / analyze_mission(path) / analyze_menu(path) -> read_file for raw bytes.
- Author a mission: open_in_workspace(workspace="mission", path=...) -> list_items -> place_entities (grounded for you) -> edit_waypoint_path -> set_mission_header -> set_camera + screenshot to inspect. Repair floating/sunken layouts with reground_mission.
- Test in the real game: run_game(op="start", mode="game"|"mission") -> game_state / game_debug / game_screenshot -> run_game(op="stop").
- Author a menu: open_in_workspace(workspace="mnu", path=...) or menu_tabs(op="new") -> get_menu -> add_menu_widgets / edit_menu_widget / set_widget_actions -> menu_screenshot to look -> preview_menu to click through the navigation. describe_api(topic="menus") has the vocabulary.

Be a good guest: narrate risky operations with show_status_message; the human's unsaved work matters."""

const TOPICS := {
	"coordinates": """# Coordinate spaces

- Tool INPUTS are Godot world space: x/z across the map, y up. Placement, marker, and camera tools sample the terrain for y — you never supply it.
- Entity records in results (get_mission_entities, analyze_mission) are BMS mission space, as stored in the .bms: {x, y, z} with z up.
- Mapping: world = (bms.x, bms.z, -bms.y); bms = (world.x, -world.z, world.y). Tools label which space each position is in; entity results also carry a world_position echo.""",
	"camera": """# Aiming the editor camera (then screenshot)

set_camera does it all: frame_entity={kind,index} (select + frame a mission entity), frame_point={x,z,radius,...} (orbit a terrain point at its ground height — radius is roughly how much terrain stays in view), or position+look_at for an exact pose. Then screenshot(target="viewport").

The terrain/mission views share an orbit camera (distance is about radius x 1.35, pitch default about -32 degrees). Object-workspace preview cameras only support position+look_at.""",
	"workspaces": """# Workspaces

Ids: terrain, object, mission, fonts, credits, strings, mnu, music, sound, environment (a popup over the active 3D view).

open_in_workspace is the one open path; undo/redo route per-workspace; dirty state and capabilities are in get_editor_state. The mission workspace carries authored-data tools (list_items, place_entities, edit_mission_entity, edit_waypoint_path, set_mission_header, reground_mission) — open a mission there first. Real runtime inspection is exposed separately through run_game and the game_* tools. The mnu workspace carries the menu tools (get_menu, menu_tabs, add_menu_widgets, edit_menu_widget, set_widget_actions, edit_widget_items, preview_menu, menu_screenshot) and is MULTIDOC: tabs via menu_tabs, and undo/redo act on the ACTIVE tab (each tab keeps its own history). describe_api(name=...) reflects engine classes and live editor objects when you need a result shape explained.""",
	"menus": """# Menu authoring (the Menus workspace, .mnu)

Vocabulary: a MENU is one .mnu document; a SCREEN is a full-canvas layout (one visible at a time; navigation moves between them); every tree node is a WINDOW; a WIDGET is a Window of a specific type (BUTTON, LIST, TABLE, ...). An ACTION is behavior the file itself expresses — navigate to a screen/menu, show/hide a window, pop, URL — wired with set_widget_actions. A COMMAND is game behavior the engine binds to a widget's NAME (start mission, apply settings): names are hooks, so reuse shipped names exactly and never rename shipped widgets casually.

Conventions:
- Rects are [x, y, w, h] in menu-space pixels (typically a 640x480 board), parent-relative; child order is z-order. edit_menu_widget move_rects does layout passes in one undo step. A null/-1 w or h is AUTO-SIZE (the engine stretches a widget's appearance art across an explicit width, so box-art toggles and auto-height labels omit it).
- text + string_type: "id" means text is a key into the screen's text_rsrc string table (.bin); "" means a literal.
- Color/font values like %TITLE_COLOR% are stylesheet (menu_style.mns) references — preserve them verbatim. Text with no FG color (own or inherited from an ancestor FONT) renders unreadable in the game — statics want a literal hex (e.g. c4c4c4), pressables the %DEF_TEXT_*% vars.
- The "Tab" pattern: sibling buttons whose window Actions hide each other's panels and show their own.

Game-shaping (the original engine is strict where the editor preview is forgiving — analyze_menu's game_safety audits all of this, and the build tools apply most of it for you):
- A screen action MUST carry file= even to navigate within the same .mnu — set_widget_actions auto-fills the menu's own filename (save an Untitled tab first).
- New screens get a game-shaped root automatically (a MAIN window, full position, an appearance row); edit_menu_screen add/set takes background (an image .tga backdrop — without it the root is engine-painted, so a video underneath shows through) and frame ({stencil, brush, monogram} that DRAW_FRAME panels render with).
- Pressables carry per-state APPEARANCE rows: text buttons four empty ones (auto-added), box toggles need image rows (map_state 0..3 state-strip art like btn5.tga) — author them with the appearances prop.

Menus are MULTIDOC: tabs via menu_tabs/get_menu; undo/redo act on the ACTIVE tab. The Interactive preview (preview_menu) plays the menu sandboxed — pressing widgets walks the real navigation; all editing tools are locked until op="off". Look at the board with menu_screenshot; study shipped menus (and audit your own) with analyze_menu.""",
}

const WORKSPACE_TO_KIND := {
	"terrain": "terrain", "object": "object", "mission": "mission",
	"fonts": "font", "credits": "credits", "strings": "strings",
	"mnu": "menu", "music": "music", "sound": "sound", "environment": "environment",
}

## Watchdog budgets for the slow tools: opening a mission loads its terrain
## and places its objects; screenshots wait on render frames.
const _OPEN_TIMEOUT_MS := 120000
const _SCREENSHOT_TIMEOUT_MS := 30000

const EXTRA_API_CLASSES: Array[String] = ["EnvFile", "RtxtStringFile", "CbinCreditsResource", "MnsStyleSheet", "PerfTimeline", "FlyCamera"]

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("get_editor_state",
			"Deep snapshot of the ONED editor: version, mounted resource root, every workspace (open document, dirty, capabilities), the active workspace, managed game-run status, camera pose, recent perf lines, and a log cursor. Call this first in a session and after any surprising result.",
			{}), Callable(self, "_tool_editor_state"))
	registry.register(McpToolDef.make("get_logs",
			"Editor log since a cursor: server/status entries plus engine lines (print, push_warning, push_error) tailed from Godot's log file. Omit cursor to resume from this session's last read (first call: recent tail). Use after any failed or surprising operation.",
			{
				"cursor": { "type": "integer", "description": "Resume after this seq (from a previous next_cursor or get_editor_state.log_cursor). Omit to resume the session cursor." },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"sources": { "type": "array", "items": { "type": "string", "enum": ["server", "script", "status", "engine"] }, "description": "Filter by source; omit for all." },
			}, [], false), Callable(self, "_tool_get_logs"))
	registry.register(McpToolDef.make("show_status_message",
			"Show a transient message in the editor's status bar — visible to the human at the keyboard. Use it to narrate what you are about to do.",
			{
				"text": { "type": "string" },
				"duration_s": { "type": "number", "default": 4.0 },
			}, ["text"]), Callable(self, "_tool_show_status"))
	registry.register(McpToolDef.make("set_fullscreen",
			"Put the editor window into (or out of) fullscreen — the core-engine window mode (NovaWindow), the same F11 toggles. Do this before screenshots so the 3D viewport fills the display. Omit `enabled` to toggle. Returns the resulting fullscreen state.",
			{
				"enabled": { "type": "boolean", "description": "true = fullscreen, false = windowed; omit to toggle." },
			}), Callable(self, "_tool_set_fullscreen"))
	registry.register(McpToolDef.make("set_resource_root",
			"Mount a different game resource root (loose asset dir or install dir with PFFs) — the same seam as the editor's folder picker. Persists like the UI action; note the previous dir from get_editor_state first if you plan to restore it.",
			{
				"dir": { "type": "string", "description": "Absolute directory to mount." },
			}, ["dir"]), Callable(self, "_tool_set_resource_root"))
	registry.register(McpToolDef.make("set_node_visible",
			"Show/hide a live scene node (CanvasItem/Node3D `visible`) — tier/layer isolation while troubleshooting rendering. Path rules match get_node_state. Restore what you hide.",
			{
				"path": { "type": "string" },
				"visible": { "type": "boolean" },
			}, ["path", "visible"]), Callable(self, "_tool_set_node_visible"))
	registry.register(McpToolDef.make("get_node_state",
			"Troubleshooting X-ray for a live scene node: resolve it by absolute path or recursive name search, read named properties, and call zero-arg read-only query methods (get_*/is_*/has_* names only). Returns class, script, and child names for orientation. Read-only.",
			{
				"path": { "type": "string", "description": "Absolute node path (\"/root/...\"), or a bare node NAME searched recursively from the scene root (first match; e.g. \"FoliageDispatcher\")." },
				"properties": { "type": "array", "items": { "type": "string" }, "description": "Property names to read." },
				"call": { "type": "array", "items": { "type": "string" }, "description": "Zero-arg query methods to call — get_*/is_*/has_* names only." },
			}, ["path"]), Callable(self, "_tool_get_node_state"))
	registry.register(McpToolDef.make("describe_api",
			"Read-only API reference: with no args, lists topics, engine classes (Nova*), and live editor objects. name: methods/properties/constants of a class (\"NovaMissionData\") or live object (\"shell\", \"editor\", \"mission_controller\", \"game_session\", \"camera\", \"resource_root\", \"workspace:strings\") — useful for understanding result shapes. topic: a guide (\"coordinates\", \"camera\", \"workspaces\", \"menus\").",
			{
				"name": { "type": "string", "description": "Class or live-object name." },
				"topic": { "type": "string", "description": "Guide topic." },
			}), Callable(self, "_tool_describe_api"))
	registry.register(McpToolDef.make("list_assets",
			"List game assets in the mounted resource root by kind: mission, terrain, environment, object (3dp/3di/ase), font, credits, strings, menu, music (banks + scripts), sound — or \"\" for everything. filter is a case-insensitive substring over name and relative path.",
			{
				"kind": { "type": "string", "default": "" },
				"filter": { "type": "string", "default": "" },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"offset": { "type": "integer", "default": 0 },
			}), Callable(self, "_tool_list_assets"))
	registry.register(McpToolDef.make("read_file",
			"Read a file by resource name (resolved through loose dirs and PFF archives, with SCR/BFC1 decode) or absolute path. Returns text (lossy for binary) or base64; window large files with offset/max_bytes.",
			{
				"path": { "type": "string" },
				"as": { "type": "string", "enum": ["text", "base64"], "default": "text" },
				"offset": { "type": "integer", "default": 0 },
				"max_bytes": { "type": "integer", "default": 65536, "maximum": 1048576 },
			}, ["path"]), Callable(self, "_tool_read_file"))
	registry.register(McpToolDef.make("describe_asset",
			"Structured JSON summary of a game asset by name/path, dispatched on type: mission (header info, entity counts, waypoints, logic), strings (sections + texts), menu (screen/widget tree), font (pages, glyph metrics), environment (full property set + time-of-day samples), sound profile, 3di (shallow), pff (entry list). depth=\"full\" adds bounded detail.",
			{
				"path": { "type": "string" },
				"depth": { "type": "string", "enum": ["summary", "full"], "default": "summary" },
				"limit": { "type": "integer", "default": 100, "description": "Per-collection cap in full depth." },
			}, ["path"]), Callable(self, "_tool_describe_asset"))
	registry.register(McpToolDef.make("open_in_workspace",
			"Open an asset in its workspace and switch to it — the same path the editor's own cross-jumps use. workspace: terrain|object|mission|fonts|credits|strings|mnu|music|sound|environment. Fails if that workspace has unsaved changes unless discard=true. Opening a mission also loads its terrain and places its objects (can take seconds).",
			{
				"workspace": { "type": "string" },
				"path": { "type": "string" },
				"discard": { "type": "boolean", "default": false, "description": "Allow replacing an unsaved document." },
				"focus": { "type": "object", "description": "Workspace-defined focus target (e.g. {\"key\": ...} for strings)." },
			}, ["workspace", "path"], true, _OPEN_TIMEOUT_MS), Callable(self, "_tool_open_in_workspace"))
	registry.register(McpToolDef.make("set_camera",
			"Aim the editor's 3D camera, then screenshot to see the result. One mode per call: frame_entity={kind, index} selects and frames a mission entity; frame_point={x, z, radius?, yaw_deg?, pitch_deg?} orbits a world-space terrain point at its ground height (radius is roughly how much terrain stays in view, default 60); position=[x,y,z] with look_at=[x,y,z] sets an exact pose. Works in terrain/mission/object 3D views; object-preview cameras support only position+look_at. Returns the resulting pose.",
			{
				"frame_entity": { "type": "object", "properties": { "kind": { "type": "integer" }, "index": { "type": "integer" } } },
				"frame_point": { "type": "object", "properties": { "x": { "type": "number" }, "z": { "type": "number" }, "radius": { "type": "number", "default": 60 }, "yaw_deg": { "type": "number", "default": 0 }, "pitch_deg": { "type": "number", "default": -32 } } },
				"position": { "type": "array", "items": { "type": "number" }, "minItems": 3, "maxItems": 3 },
				"look_at": { "type": "array", "items": { "type": "number" }, "minItems": 3, "maxItems": 3 },
			}), Callable(self, "_tool_set_camera"))
	registry.register(McpToolDef.make("undo",
			"Undo the last edit in a workspace (default: the active one) — mission edits, terrain strokes, whatever that workspace's history holds. steps repeats it. Returns what remains undoable.",
			{
				"workspace": { "type": "string", "default": "" },
				"steps": { "type": "integer", "default": 1, "minimum": 1, "maximum": 50 },
			}), Callable(self, "_tool_undo"))
	registry.register(McpToolDef.make("redo",
			"Redo previously undone edits in a workspace (default: the active one). steps repeats it.",
			{
				"workspace": { "type": "string", "default": "" },
				"steps": { "type": "integer", "default": 1, "minimum": 1, "maximum": 50 },
			}), Callable(self, "_tool_redo"))
	registry.register(McpToolDef.make("screenshot",
			"Capture the editor as an image. target=\"viewport\": the 3D view (what the camera sees; falls back to the window in 2D workspaces). target=\"window\": the whole editor UI. Returns the image plus a caption (workspace, document, camera pose). Aim first — see describe_api(topic=\"camera\").",
			{
				"target": { "type": "string", "enum": ["viewport", "window"], "default": "viewport" },
				"max_dim": { "type": "integer", "default": 1280, "minimum": 64, "maximum": 4096 },
				"format": { "type": "string", "enum": ["webp", "png"], "default": "webp" },
				"quality": { "type": "number", "default": 0.8 },
			}, [], true, _SCREENSHOT_TIMEOUT_MS), Callable(self, "_tool_screenshot"))


func _tool_set_resource_root(args: Dictionary, ctx: McpToolContext) -> Variant:
	var dir := String(args.get("dir", ""))
	if dir.is_empty():
		return McpToolResult.error("dir is required.")
	if not DirAccess.dir_exists_absolute(dir):
		return McpToolResult.error("Directory does not exist: %s" % dir)
	var shell := ctx.shell
	if shell == null or not shell.has_method("set_resource_root_dir"):
		return McpToolResult.error("The editor shell is not bound or has no set_resource_root_dir.")
	var previous: String = shell.get_resource_root_dir() if shell.has_method("get_resource_root_dir") else ""
	shell.set_resource_root_dir(dir)
	return {
		"ok": true,
		"dir": dir,
		"previous": previous,
		"mounted": ctx.root() != null,
	}


func _resolve_live_node(path: String) -> Node:
	var tree_root: Node = service.get_tree().root
	if path.begins_with("/"):
		return tree_root.get_node_or_null(NodePath(path))
	return tree_root.find_child(path, true, false)


func _tool_set_node_visible(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var path := String(args.get("path", ""))
	var node := _resolve_live_node(path)
	if node == null:
		return McpToolResult.error("No node found for '%s'." % path)
	if not ("visible" in node):
		return McpToolResult.error("Node '%s' (%s) has no `visible` property." % [path, node.get_class()])
	node.set("visible", bool(args.get("visible", true)))
	return { "ok": true, "path": String(node.get_path()), "visible": bool(node.get("visible")) }


func _tool_get_node_state(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var path := String(args.get("path", ""))
	if path.is_empty():
		return McpToolResult.error("path is required.")
	var node := _resolve_live_node(path)
	if node == null:
		return McpToolResult.error("No node found for '%s' (absolute path or recursive name search from the scene root)." % path)
	var script_path := ""
	var script: Variant = node.get_script()
	if script != null and script is Resource:
		script_path = (script as Resource).resource_path
	var out := {
		"path": String(node.get_path()),
		"class": node.get_class(),
		"script": script_path,
		"children": node.get_children().map(func(c: Node) -> String: return String(c.name)),
	}
	var prop_names: Variant = args.get("properties", [])
	if prop_names is Array and not (prop_names as Array).is_empty():
		var props := {}
		for p in prop_names:
			var pname := String(p)
			# ":"-paths reach into resources (e.g. "multimesh:instance_count").
			props[pname] = _node_state_jsonable(node.get_indexed(pname) if pname.contains(":") else node.get(pname))
		out["properties"] = props
	var call_names: Variant = args.get("call", [])
	if call_names is Array and not (call_names as Array).is_empty():
		var calls := {}
		for m in call_names:
			var mname := String(m)
			if not (mname.begins_with("get_") or mname.begins_with("is_") or mname.begins_with("has_")):
				calls[mname] = "SKIPPED: only get_*/is_*/has_* query methods"
			elif not node.has_method(mname):
				calls[mname] = "SKIPPED: no such method"
			else:
				calls[mname] = _node_state_jsonable(node.call(mname))
		out["calls"] = calls
	return out


static func _node_state_jsonable(v: Variant, depth: int = 0) -> Variant:
	if depth > 4:
		return "<depth capped>"
	match typeof(v):
		TYPE_NIL, TYPE_BOOL, TYPE_INT, TYPE_FLOAT, TYPE_STRING:
			return v
		TYPE_DICTIONARY:
			var d := {}
			for k in v:
				d[str(k)] = _node_state_jsonable(v[k], depth + 1)
			return d
		TYPE_ARRAY:
			var a: Array = []
			for e in v:
				a.append(_node_state_jsonable(e, depth + 1))
				if a.size() >= 64:
					a.append("<truncated at 64>")
					break
			return a
		TYPE_OBJECT:
			if v == null:
				return null
			var o: Object = v
			var suffix := ""
			if o is Resource and (o as Resource).resource_path != "":
				suffix = ":" + (o as Resource).resource_path
			elif o is Node:
				suffix = ":" + String((o as Node).name)
			return "<%s%s>" % [o.get_class(), suffix]
		TYPE_PACKED_BYTE_ARRAY, TYPE_PACKED_INT32_ARRAY, TYPE_PACKED_INT64_ARRAY, \
		TYPE_PACKED_FLOAT32_ARRAY, TYPE_PACKED_FLOAT64_ARRAY, TYPE_PACKED_STRING_ARRAY, \
		TYPE_PACKED_VECTOR2_ARRAY, TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_COLOR_ARRAY:
			return "<packed array, size %d>" % [v.size()]
		_:
			return var_to_str(v)


func _tool_editor_state(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var shell := ctx.shell
	if shell == null:
		return McpToolResult.error("The editor shell is not bound yet — ONED is still booting.")
	var workspaces: Array = []
	var table: Variant = shell.get("_workspaces")
	var instances: Array = table.values() if table is Dictionary else []
	var popups: Variant = shell.get("_popup_workspaces")
	if popups is Dictionary:
		instances.append_array(popups.values())
	for ws in instances:
		workspaces.append(_workspace_state(ws))
	var active: Variant = ctx.workspace()
	var out := {
		"app": {
			"name": "ONED",
			"version": str(ProjectSettings.get_setting("application/config/version", "0.0.0")),
			"godot": Engine.get_version_info().get("string", ""),
		},
		"resource_root": {
			"dir": shell.get_resource_root_dir() if shell.has_method("get_resource_root_dir") else "",
			"mounted": ctx.root() != null,
			"expansion": NovaResourceDirSettings.get_expansion(),
			"game": NovaResourceDirSettings.get_game(),
		},
		"active_workspace": String(active.get_workspace_id()) if active != null else "",
		"workspaces": workspaces,
		"mission": _mission_state(ctx),
		"game_run": _game_run_state(shell),
		"camera": _camera_state(ctx),
		"perf_recent": _perf_lines(),
		"log_cursor": service.log_hub.latest_cursor() if service != null and service.get("log_hub") != null else 0,
	}
	return out


func _workspace_state(ws: Variant) -> Dictionary:
	if ws == null:
		return {}
	var out := {
		"id": String(ws.get_workspace_id()),
		"label": String(ws.get_workspace_label()),
		"open_path": String(ws.get_current_resource_path()),
		"title": String(ws.get_project_title()),
		"dirty": bool(ws.has_unsaved_changes()),
		"busy": bool(ws.is_busy()),
		"has_3d_view": ws.get_viewport_camera() != null,
		"can": {
			"new": bool(ws.can_new()), "open": bool(ws.can_open()),
			"save": bool(ws.can_save()), "save_as": bool(ws.can_save_as()),
			"export": bool(ws.can_export()),
			"undo": bool(ws.can_undo()), "redo": bool(ws.can_redo()),
		},
	}
	if ws.supports_document_tabs():
		out["documents"] = { "tabs": DocumentTabRow.to_dict_rows(ws.get_document_tabs()), "active": ws.get_active_document_index() }
	return out


func _mission_state(ctx: McpToolContext) -> Dictionary:
	var controller: Variant = ctx.mission()
	var out := { "loaded": false }
	if controller != null and controller.has_method("is_loaded"):
		out["loaded"] = bool(controller.is_loaded())
	if controller != null and controller.has_method("get_current_path"):
		out["path"] = controller.get_current_path()
	if controller != null and controller.has_method("get_stats"):
		out["stats"] = controller.get_stats()
	return out


func _game_run_state(shell: Variant) -> Dictionary:
	if shell == null or not shell.has_method("get_game_run_session"):
		return {"state": "unavailable", "running": false}
	var session: Variant = shell.get_game_run_session()
	if session == null or not session.has_method("get_state"):
		return {"state": "unavailable", "running": false}
	return session.get_state()


func _camera_state(ctx: McpToolContext) -> Dictionary:
	var camera := ctx.camera()
	if camera == null:
		return { "available": false }
	return {
		"available": true,
		"position": camera.global_position,
		"rotation_deg": camera.rotation_degrees,
	}


static func _perf_lines() -> Array:
	var lines: Array = []
	for timeline in PerfTimeline.history():
		lines.append(timeline.summary())
		if lines.size() >= 4:
			break
	return lines


func _tool_get_logs(args: Dictionary, ctx: McpToolContext) -> Variant:
	var hub: McpLogHub = service.log_hub
	hub.ingest_engine()
	var session: Dictionary = service.server.session(String(ctx.args.get("_session_id", "")))
	var cursor := int(args.get("cursor", -1))
	if cursor < 0:
		cursor = int(session.get("log_cursor", 0)) if not session.is_empty() else 0
	var sources := PackedStringArray()
	for source in args.get("sources", []) if args.get("sources") is Array else []:
		sources.append(String(source))
	var page := hub.get_entries(cursor, int(args.get("limit", 200)), sources)
	if not session.is_empty():
		session["log_cursor"] = page["next_cursor"]
	return page


func _tool_show_status(args: Dictionary, ctx: McpToolContext) -> Variant:
	if ctx.shell == null or not ctx.shell.has_method("show_status_message"):
		return McpToolResult.error("The editor shell is not bound yet.")
	ctx.shell.show_status_message(String(args.get("text", "")), float(args.get("duration_s", 4.0)))
	return { "ok": true }


func _tool_set_fullscreen(args: Dictionary, ctx: McpToolContext) -> Variant:
	var tree := ctx.main_tree()
	var window: Window = tree.root if tree != null else null
	if window == null:
		return McpToolResult.error("No window is available.")
	var fullscreen: bool
	if args.has("enabled"):
		NovaWindow.set_fullscreen(window, bool(args["enabled"]))
		fullscreen = NovaWindow.is_fullscreen(window)
	else:
		fullscreen = NovaWindow.toggle_fullscreen(window)
	return { "ok": true, "fullscreen": fullscreen }


func _tool_describe_api(args: Dictionary, ctx: McpToolContext) -> Variant:
	var topic := String(args.get("topic", ""))
	if not topic.is_empty():
		if not TOPICS.has(topic):
			return McpToolResult.error("Unknown topic '%s'. Topics: %s" % [topic, ", ".join(TOPICS.keys())])
		return { "kind": "guide", "topic": topic, "markdown": TOPICS[topic] }
	var name := String(args.get("name", ""))
	if name.is_empty():
		return {
			"topics": TOPICS.keys(),
			"classes": _api_classes(),
			"objects": ["shell", "editor", "mission_controller", "game_session", "camera", "resource_root",
					"workspace:terrain", "workspace:mission", "workspace:strings", "..."],
			"hint": "describe_api(name=...) for a class or live object; describe_api(topic=...) for a guide.",
		}
	if ClassDB.class_exists(name):
		return _describe_native_class(name)
	var live: Variant = _live_object(ctx, name)
	if live != null:
		return _describe_live_object(name, live)
	return McpToolResult.error("'%s' is neither a registered class nor a live object — call describe_api() with no args for the index. (Live objects can be null until their workspace/document exists.)" % name)


static func _api_classes() -> Array:
	var names: Array = []
	for cls in ClassDB.get_class_list():
		var text := String(cls)
		if text.begins_with("Nova") or EXTRA_API_CLASSES.has(text):
			names.append(text)
	names.sort()
	return names


static func _describe_native_class(name: String) -> Dictionary:
	var methods: Array = []
	for method: Dictionary in ClassDB.class_get_method_list(name, true):
		methods.append(_method_brief(method))
	var properties: Array = []
	for prop: Dictionary in ClassDB.class_get_property_list(name, true):
		if int(prop.get("usage", 0)) & PROPERTY_USAGE_EDITOR == 0:
			continue
		properties.append({ "name": prop["name"], "type": type_string(int(prop["type"])) })
	var constants := {}
	for constant in ClassDB.class_get_integer_constant_list(name, true):
		constants[String(constant)] = ClassDB.class_get_integer_constant(name, constant)
	return { "kind": "native_class", "name": name, "methods": methods, "properties": properties, "constants": constants }


static func _describe_live_object(name: String, object: Variant) -> Dictionary:
	var script: Variant = object.get_script() if object is Object else null
	if script == null:
		var native := _describe_native_class(object.get_class())
		native["kind"] = "live_object"
		native["object"] = name
		return native
	var methods: Array = []
	for method: Dictionary in script.get_script_method_list():
		if String(method["name"]).begins_with("_"):
			continue
		methods.append(_method_brief(method))
	return {
		"kind": "live_object",
		"object": name,
		"class": object.get_class(),
		"script_class": String(script.get_global_name()) if script.has_method("get_global_name") else "",
		"methods": methods,
	}


static func _method_brief(method: Dictionary) -> Dictionary:
	var arg_names: Array = []
	for arg: Dictionary in method.get("args", []):
		var type_name := String(arg.get("class_name", ""))
		if type_name.is_empty():
			type_name = type_string(int(arg.get("type", TYPE_NIL)))
		arg_names.append("%s: %s" % [arg.get("name", "?"), type_name])
	var ret: Dictionary = method.get("return", {})
	var ret_name := String(ret.get("class_name", ""))
	if ret_name.is_empty():
		ret_name = type_string(int(ret.get("type", TYPE_NIL)))
	return { "name": method.get("name", "?"), "args": arg_names, "returns": ret_name }


static func _live_object(ctx: McpToolContext, name: String) -> Variant:
	if name.begins_with("workspace:"):
		return ctx.workspace(name.get_slice(":", 1))
	match name:
		"shell":
			return ctx.shell
		"editor":
			return ctx.editor
		"mission_controller":
			return ctx.mission()
		"game_session":
			return ctx.shell.get_game_run_session() \
					if ctx.shell != null and ctx.shell.has_method("get_game_run_session") else null
		"camera":
			return ctx.camera()
		"resource_root":
			return ctx.root()
	return null


func _tool_list_assets(args: Dictionary, ctx: McpToolContext) -> Variant:
	var shell := ctx.shell
	if shell == null or ctx.root() == null:
		return McpToolResult.error("No resource directory mounted — set one in the editor's Settings (gear) popup.")
	if shell.has_method("_ensure_resource_index"):
		shell._ensure_resource_index()
	var index: Variant = ctx.index()
	if index == null:
		return McpToolResult.error("Resource index unavailable.")
	var entries: Array = index.get_resource_files(String(args.get("kind", "")))
	var filter := String(args.get("filter", "")).to_lower()
	var matched: Array = []
	for entry: Dictionary in entries:
		if not filter.is_empty():
			var haystack := "%s %s" % [String(entry.get("display_name", "")), String(entry.get("relative_path", entry.get("path", "")))]
			if not haystack.to_lower().contains(filter):
				continue
		matched.append(entry)
	var offset := maxi(int(args.get("offset", 0)), 0)
	var limit := clampi(int(args.get("limit", 200)), 1, 1000)
	var page := matched.slice(offset, offset + limit)
	var assets: Array = []
	for entry: Dictionary in page:
		assets.append({
			"kind": entry.get("kind", ""),
			"name": entry.get("display_name", entry.get("logical_name", "")),
			"path": entry.get("path", ""),
			"relative_path": entry.get("relative_path", ""),
			"size_bytes": entry.get("size_bytes", -1),
			"source": entry.get("source_type", ""),
		})
	return { "assets": assets, "total": matched.size(), "truncated": offset + assets.size() < matched.size() }


func _tool_read_file(args: Dictionary, ctx: McpToolContext) -> Variant:
	var resolved := McpAssetDescribe.resolve(ctx, String(args.get("path", "")))
	if not resolved["ok"]:
		return McpToolResult.error(String(resolved["error"]))
	var bytes := McpAssetDescribe.read_bytes(ctx, resolved)
	var offset := maxi(int(args.get("offset", 0)), 0)
	var max_bytes := clampi(int(args.get("max_bytes", 65536)), 1, 1048576)
	var window := bytes.slice(offset, offset + max_bytes)
	var out := {
		"name": resolved["name"],
		"path": resolved["path"],
		"size_bytes": bytes.size(),
		"offset": offset,
		"returned_bytes": window.size(),
		"truncated": offset + window.size() < bytes.size(),
	}
	if String(args.get("as", "text")) == "base64":
		out["base64"] = Marshalls.raw_to_base64(window)
	else:
		var text := window.get_string_from_utf8()
		out["text"] = text if not (text.is_empty() and window.size() > 0) else window.get_string_from_ascii()
	return out


func _tool_describe_asset(args: Dictionary, ctx: McpToolContext) -> Variant:
	var described := McpAssetDescribe.describe(ctx, String(args.get("path", "")),
			String(args.get("depth", "summary")), int(args.get("limit", 100)))
	if described.has("error") and not described.has("data"):
		return McpToolResult.error(String(described["error"]))
	return described


func _tool_open_in_workspace(args: Dictionary, ctx: McpToolContext) -> Variant:
	var workspace_id := String(args.get("workspace", ""))
	if not WORKSPACE_TO_KIND.has(workspace_id):
		return McpToolResult.error("Unknown workspace '%s'. Ids: %s" % [workspace_id, ", ".join(WORKSPACE_TO_KIND.keys())])
	if ctx.shell == null:
		return McpToolResult.error("The editor shell is not bound yet.")
	var resolved := McpAssetDescribe.resolve(ctx, String(args.get("path", "")))
	if not resolved["ok"]:
		return McpToolResult.error(String(resolved["error"]))
	# Loose files open by absolute path; archived assets fall back to the bare
	# name so workspaces that resolve through the VFS still work.
	var target := String(resolved["path"]) if resolved.get("loose", false) else String(resolved["name"])
	var ws: Variant = ctx.workspace(workspace_id)
	if ws != null and ws.has_unsaved_changes() and String(ws.get_current_resource_path()) != target \
			and not bool(args.get("discard", false)):
		return McpToolResult.error("The %s workspace has unsaved changes — save in the editor first, or pass discard: true to replace them." % workspace_id)
	# The wire focus object is the transport encoding; decode it at this edge.
	var focus := FocusPayload.from_dict(args.get("focus", {}) if args.get("focus") is Dictionary else {})
	var err: Error = ctx.shell.open_in_workspace(WORKSPACE_TO_KIND[workspace_id], target, focus)
	await ctx.frames(1)
	if err != OK:
		return McpToolResult.error("Open failed (%s) for %s — check get_logs; the editor's status bar message is mirrored there." % [error_string(err), target])
	var active: Variant = ctx.workspace()
	return {
		"ok": true,
		"active_workspace": String(active.get_workspace_id()) if active != null else "",
		"open_path": String(ws.get_current_resource_path()) if ws != null else target,
		"dirty_workspaces": _dirty_workspaces(ctx),
	}


func _dirty_workspaces(ctx: McpToolContext) -> Array:
	var dirty: Array = []
	var table: Variant = ctx.shell.get("_workspaces") if ctx.shell != null else null
	if table is Dictionary:
		for ws in table.values():
			if ws != null and ws.has_unsaved_changes():
				dirty.append(String(ws.get_workspace_id()))
	return dirty


func _tool_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	var target := String(args.get("target", "viewport"))
	var viewport: Viewport = null
	var note := ""
	if target == "viewport":
		var camera := ctx.camera()
		if camera != null:
			viewport = camera.get_viewport()
		else:
			note = "No 3D camera in the active workspace — captured the whole window. "
	if viewport == null:
		var tree := ctx.main_tree()
		viewport = tree.root if tree != null else null
	var outcome: Dictionary = await McpScreenshot.capture(viewport, {
		"max_dim": int(args.get("max_dim", 1280)),
		"format": String(args.get("format", "webp")),
		"quality": float(args.get("quality", 0.8)),
	}, func() -> bool: return ctx.cancelled)
	if not outcome["ok"]:
		return McpToolResult.error(String(outcome["error"]))
	var caption := "%dx%d %s, %d KiB — %s" % [
		outcome["width"], outcome["height"], outcome["mime"],
		(outcome["bytes"] as PackedByteArray).size() / 1024,
		note + _screenshot_context(ctx)]
	if outcome.has("warning"):
		caption += " (%s)" % outcome["warning"]
	return McpToolResult.image(outcome["bytes"], outcome["mime"], caption)


func _screenshot_context(ctx: McpToolContext) -> String:
	var active: Variant = ctx.workspace()
	var parts := PackedStringArray()
	if active != null:
		parts.append("workspace=%s" % active.get_workspace_id())
		var path := String(active.get_current_resource_path())
		if not path.is_empty():
			parts.append("doc=%s" % path.get_file())
	var camera := ctx.camera()
	if camera != null:
		var p := camera.global_position
		var r := camera.rotation_degrees
		parts.append("cam=(%.0f, %.0f, %.0f) rot=(%.0f, %.0f)" % [p.x, p.y, p.z, r.x, r.y])
	return " ".join(parts)


func _tool_set_camera(args: Dictionary, ctx: McpToolContext) -> Variant:
	var camera := ctx.camera()
	if camera == null:
		return McpToolResult.error("No 3D camera in the active workspace — open terrain/mission/object first (open_in_workspace).")
	var note := ""
	if args.get("frame_entity") is Dictionary:
		var target: Dictionary = args["frame_entity"]
		var controller: Variant = ctx.mission()
		if controller == null or not controller.has_method("select_object"):
			return McpToolResult.error("frame_entity needs an open mission — open_in_workspace(workspace=\"mission\", ...) first.")
		controller.select_object(int(target.get("kind", -1)), int(target.get("index", -1)))
		if (controller.get_selection_summary() as Dictionary).is_empty():
			return McpToolResult.error("No entity at (kind=%s, index=%s) — get_mission_entities lists them." % [target.get("kind"), target.get("index")])
		controller.focus_selection_in_view()
	elif args.get("frame_point") is Dictionary:
		var point: Dictionary = args["frame_point"]
		var x := float(point.get("x", 0.0))
		var z := float(point.get("z", 0.0))
		var height := 0.0
		if ctx.editor != null and ctx.editor.has_method("sample_height_world"):
			var sampled: float = ctx.editor.sample_height_world(x, z)
			if is_nan(sampled):
				note = "point is off the terrain; framed at height 0. "
			else:
				height = sampled
		if camera.has_method("frame_bounds_custom"):
			camera.frame_bounds_custom(Vector3(x, height, z), float(point.get("radius", 60.0)), 1.35, 4000.0,
					deg_to_rad(float(point.get("yaw_deg", 0.0))), deg_to_rad(float(point.get("pitch_deg", -32.0))))
		else:
			var center := Vector3(x, height, z)
			camera.global_position = center + Vector3(0, 40, 60)
			camera.look_at(center)
	elif args.get("position") is Array and args.get("look_at") is Array:
		var pos: Array = args["position"]
		var aim: Array = args["look_at"]
		var pos_v := Vector3(float(pos[0]), float(pos[1]), float(pos[2]))
		var aim_v := Vector3(float(aim[0]), float(aim[1]), float(aim[2]))
		if pos_v.distance_to(aim_v) < 0.01:
			return McpToolResult.error("position and look_at coincide.")
		if camera.has_method("frame_bounds_custom"):
			# Route through the orbit state so subsequent human orbiting does not
			# snap: distance/yaw/pitch derived from the requested pose.
			var to_cam := pos_v - aim_v
			var yaw := atan2(to_cam.x, to_cam.z)
			var pitch := -asin(clampf(to_cam.normalized().y, -1.0, 1.0))
			camera.frame_bounds_custom(aim_v, to_cam.length(), 1.0, 100000.0, yaw, pitch)
		else:
			camera.global_position = pos_v
			camera.look_at(aim_v)
	else:
		return McpToolResult.error("Pass exactly one mode: frame_entity, frame_point, or position+look_at.")
	await ctx.frames(1)
	return {
		"position": camera.global_position,
		"rotation_deg": camera.rotation_degrees,
		"note": note,
		"hint": "screenshot(target=\"viewport\") shows this view.",
	}


func _tool_undo(args: Dictionary, ctx: McpToolContext) -> Variant:
	return await _undo_redo(args, ctx, true)


func _tool_redo(args: Dictionary, ctx: McpToolContext) -> Variant:
	return await _undo_redo(args, ctx, false)


func _undo_redo(args: Dictionary, ctx: McpToolContext, is_undo: bool) -> Variant:
	var ws: Variant = ctx.workspace(String(args.get("workspace", "")))
	if ws == null:
		return McpToolResult.error("Unknown workspace '%s' — get_editor_state lists ids; empty means the active one." % args.get("workspace", ""))
	var performed := 0
	for i in range(clampi(int(args.get("steps", 1)), 1, 50)):
		if is_undo:
			if not ws.can_undo():
				break
			ws.undo()
		else:
			if not ws.can_redo():
				break
			ws.redo()
		performed += 1
		await ctx.frames(1)
	return {
		"performed": performed,
		"can_undo": ws.can_undo(),
		"can_redo": ws.can_redo(),
		"workspace": String(ws.get_workspace_id()),
	}
