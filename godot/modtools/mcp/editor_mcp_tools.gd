class_name EditorMcpTools
extends RefCounted

## The built-in MCP tool catalog for ONED: thin handlers over the editor's
## existing surface (EditorWorkstation facade, EditorWorkspace capability
## hooks, the Nova* GDExtension classes) plus the scripting escape hatch.
## Descriptions are agent-facing prompt text — they carry the conventions
## (path resolution, dirty rules) so every tool teaches the protocol.

const INSTRUCTIONS := """ONED — the OpenNova editor (Godot-hosted). You are connected to a live editor a human may be watching.

Start with get_editor_state: it reports the mounted resource root, every workspace (open document, dirty, capabilities), the active workspace, and a log cursor.

Conventions:
- Paths: pass a bare resource name ("alpha.bms", resolved case-insensitively in the mounted resource root, including inside PFF archives) or an absolute path. Results carry the resolved path.
- After anything surprising, call get_logs — engine errors, script errors, and editor status messages land there.
- Edits mark documents dirty in the editor; NEVER save unless the user asked.

Typical flows:
- Inspect assets: list_assets(kind=...) -> describe_asset(path) -> read_file(path) for raw bytes.
- Look at something: open_in_workspace(workspace=..., path=...) -> screenshot(target="viewport"). Aim the camera first via execute_script (recipe: describe_api(topic="camera")).
- No tool fits? describe_api() indexes engine classes, live editor objects, and topic guides; execute_script runs GDScript inside the editor with ctx (shell, workspaces, mission, sim, resource root). Promote a repeated snippet into a named tool with define_tool — it persists across editor restarts.

Be a good guest: announce risky operations with show_status_message; the human's unsaved work matters."""

const TOPICS := {
	"ctx": """# ctx — the scripting context (execute_script and custom tools)

`func run(ctx):` for execute_script; `func run(ctx, args):` for define_tool code.

- ctx.shell — EditorWorkstation (the editor facade: open_in_workspace, show_status_message, get_resource_root, set_resource_root_dir, get_editor_camera, ...)
- ctx.editor — TerrainEditor, the app root node
- ctx.tree — the SceneTree; ctx.find_node("pattern") searches under its root
- ctx.root() — NovaResourceRoot (resolve_file/list_files/read_file through loose dirs + PFFs); ctx.index() — NovaResourceIndex
- ctx.workspace(id="") — an EditorWorkspace by string id (empty = active). Ids: terrain, object, mission, fonts, credits, strings, mnu, music, sound, environment. Hooks: open_file, save_current, can_undo/undo, has_unsaved_changes, get_current_resource_path, get_editor_document, get_viewport_camera.
- ctx.mission() — the MissionController (mission document + selection + sim transport) or null
- ctx.runtime() / ctx.sim() — the live MissionRuntime / NovaSimulation when simulating or playing-in-editor, else null
- ctx.camera() — the active 3D Camera3D or null
- ctx.log(value) — append to the result's logs; ctx.image(img) — attach an Image to the result; ctx.status(text) — editor status bar
- await ctx.frames(n) — yield n process frames; ctx.cancelled — poll between awaits in long loops
- ctx.args — the tool call's raw arguments

Return any JSON-able value; it is sanitized (Vector3 -> [x,y,z], Node -> path stub).""",
	"coordinates": """# Coordinate spaces

- BMS mission space (the .bms records, NovaMissionData entity dicts, MissionController.set_selected_position): {x, y, z} with z = up.
- Godot world space (cameras, Node3D transforms, terrain sampling): [x, y, z] with y = up.

get_all_entities()/get_entity() positions are BMS. Cameras and anything in the scene tree are Godot world. The mission scene's object container applies the mapping; when you need exact conversion, reflect the mission surface with describe_api(name="mission_controller") and work through its selection/placement methods, which all take BMS positions.""",
	"camera": """# Aiming the editor camera (then screenshot)

The terrain/mission 3D views share a FlyCamera with a public framing helper:

	func run(ctx):
		var cam := ctx.camera()
		if cam == null: return "no 3D camera — open terrain/mission/object first"
		# frame_bounds_custom(center, radius, distance_scale=1.35, max_distance=1200, yaw=0.0, pitch=-0.55)
		cam.frame_bounds_custom(Vector3(512, 30, 512), 60.0, 1.2, 2000.0, 0.6, -0.7)
		await ctx.frames(2)
		return { "position": cam.global_position, "rotation_deg": cam.rotation_degrees }

Then call screenshot(target="viewport"). Object-workspace preview cameras lack frame_bounds_custom — set global_position and call look_at() instead.""",
	"workspaces": """# Workspaces

Ids: terrain, object, mission, fonts, credits, strings, mnu, music, sound, environment (a popup over the active 3D view).

Every workspace implements the same capability contract (modtools/framework/editor_workspace.gd): can_new/can_open/can_save/can_save_as/can_export gate new_current/open_file(path)/save_current/save_as(dir)/begin_export; has_unsaved_changes(), is_busy(), get_current_resource_path(), can_undo/undo/can_redo/redo, get_editor_document() (the domain document/controller), get_viewport_camera().

open_in_workspace (the tool) is the supported open path. For everything else: execute_script with ctx.workspace(id). The mission document is a MissionController — describe_api(name="mission_controller") lists its sim transport (sim_play/sim_pause/sim_step/sim_stop), selection, and entity editing methods.""",
}

const WORKSPACE_TO_KIND := {
	"terrain": "terrain", "object": "object", "mission": "mission",
	"fonts": "font", "credits": "credits", "strings": "strings",
	"mnu": "menu", "music": "music", "sound": "sound", "environment": "environment",
}

const EXTRA_API_CLASSES: Array[String] = ["EnvFile", "RtxtStringFile", "CbinCreditsResource", "MnsStyleSheet", "PerfTimeline", "FlyCamera"]

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(_def("get_editor_state",
			"Deep snapshot of the ONED editor: version, mounted resource root, every workspace (open document, dirty, capabilities), the active workspace, mission/sim status, camera pose, recent perf lines, and a log cursor. Call this first in a session and after any surprising result.",
			{}), Callable(self, "_tool_editor_state"))
	registry.register(_def("get_logs",
			"Editor log since a cursor: server/script/status entries plus engine lines (print, push_warning, push_error, script errors) tailed from Godot's log file. Omit cursor to resume from this session's last read (first call: recent tail). Use after any failed or surprising operation.",
			{
				"cursor": { "type": "integer", "description": "Resume after this seq (from a previous next_cursor or get_editor_state.log_cursor). Omit to resume the session cursor." },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"sources": { "type": "array", "items": { "type": "string", "enum": ["server", "script", "status", "engine"] }, "description": "Filter by source; omit for all." },
			}, [], { "serial": false }), Callable(self, "_tool_get_logs"))
	registry.register(_def("show_status_message",
			"Show a transient message in the editor's status bar — visible to the human at the keyboard. Use it to narrate what you are about to do.",
			{
				"text": { "type": "string" },
				"duration_s": { "type": "number", "default": 4.0 },
			}, ["text"]), Callable(self, "_tool_show_status"))
	registry.register(_def("describe_api",
			"API reference for scripting. No args: lists topics, engine classes (Nova*), and live editor objects. name: methods/properties/constants of a class (\"NovaMissionData\") or live object (\"shell\", \"editor\", \"mission_controller\", \"runtime\", \"sim\", \"camera\", \"resource_root\", \"workspace:strings\"). topic: a guide (\"ctx\", \"coordinates\", \"camera\", \"workspaces\").",
			{
				"name": { "type": "string", "description": "Class or live-object name." },
				"topic": { "type": "string", "description": "Guide topic." },
			}), Callable(self, "_tool_describe_api"))
	registry.register(_def("list_assets",
			"List game assets in the mounted resource root by kind: mission, terrain, environment, object (3dp/3di/ase), font, credits, strings, menu, music (banks + scripts), sound — or \"\" for everything. filter is a case-insensitive substring over name and relative path.",
			{
				"kind": { "type": "string", "default": "" },
				"filter": { "type": "string", "default": "" },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"offset": { "type": "integer", "default": 0 },
			}), Callable(self, "_tool_list_assets"))
	registry.register(_def("read_file",
			"Read a file by resource name (resolved through loose dirs and PFF archives, with SCR/BFC1 decode) or absolute path. Returns text (lossy for binary) or base64; window large files with offset/max_bytes.",
			{
				"path": { "type": "string" },
				"as": { "type": "string", "enum": ["text", "base64"], "default": "text" },
				"offset": { "type": "integer", "default": 0 },
				"max_bytes": { "type": "integer", "default": 65536, "maximum": 1048576 },
			}, ["path"]), Callable(self, "_tool_read_file"))
	registry.register(_def("describe_asset",
			"Structured JSON summary of a game asset by name/path, dispatched on type: mission (header info, entity counts, waypoints, logic), strings (sections + texts), menu (screen/widget tree), font (pages, glyph metrics), environment (full property set + time-of-day samples), sound profile, 3di (shallow), pff (entry list). depth=\"full\" adds bounded detail.",
			{
				"path": { "type": "string" },
				"depth": { "type": "string", "enum": ["summary", "full"], "default": "summary" },
				"limit": { "type": "integer", "default": 100, "description": "Per-collection cap in full depth." },
			}, ["path"]), Callable(self, "_tool_describe_asset"))
	registry.register(_def("open_in_workspace",
			"Open an asset in its workspace and switch to it — the same path the editor's own cross-jumps use. workspace: terrain|object|mission|fonts|credits|strings|mnu|music|sound|environment. Fails if that workspace has unsaved changes unless discard=true. Opening a mission also loads its terrain and places its objects (can take seconds).",
			{
				"workspace": { "type": "string" },
				"path": { "type": "string" },
				"discard": { "type": "boolean", "default": false, "description": "Allow replacing an unsaved document." },
				"focus": { "type": "object", "description": "Workspace-defined focus target (e.g. {\"key\": ...} for strings)." },
			}, ["workspace", "path"], { "timeout_ms": 120000 }), Callable(self, "_tool_open_in_workspace"))
	registry.register(_def("screenshot",
			"Capture the editor as an image. target=\"viewport\": the 3D view (what the camera sees; falls back to the window in 2D workspaces). target=\"window\": the whole editor UI. Returns the image plus a caption (workspace, document, camera pose). Aim first — see describe_api(topic=\"camera\").",
			{
				"target": { "type": "string", "enum": ["viewport", "window"], "default": "viewport" },
				"max_dim": { "type": "integer", "default": 1280, "minimum": 64, "maximum": 4096 },
				"format": { "type": "string", "enum": ["webp", "png"], "default": "webp" },
				"quality": { "type": "number", "default": 0.8 },
			}, [], { "timeout_ms": 30000 }), Callable(self, "_tool_screenshot"))
	registry.register(_def("execute_script",
			"Run GDScript inside the live editor — the escape hatch when no tool fits. Send the body of `func run(ctx):` (or a full script defining run). ctx exposes the editor (describe_api(topic=\"ctx\")); code may await (e.g. `await ctx.frames(2)`). Returns the run's value, ctx.log lines, and any captured script errors. Mutations bypass tool-level guards — prefer dedicated tools when they exist. A loop that never awaits blocks the editor until it ends; mind timeout_ms.",
			{
				"code": { "type": "string" },
				"timeout_ms": { "type": "integer", "default": 10000, "minimum": 100, "maximum": 300000 },
			}, ["code"], { "timeout_ms": 310000 }), Callable(self, "_tool_execute_script"))
	registry.register(_def("define_tool",
			"Register a persistent custom tool from GDScript, so a repeated probe becomes a one-call tool that survives editor restarts. code defines `func run(ctx, args):` (or is a bare body, wrapped). The new tool is callable immediately via tools/call; it appears in tools/list when your client next refreshes (this server cannot push notifications). description and input_schema flow verbatim into the catalog — write them as prompt text.",
			{
				"name": { "type": "string", "description": "lowercase snake_case, 3-48 chars" },
				"description": { "type": "string" },
				"input_schema": { "type": "object", "description": "JSON Schema for the tool's arguments (documentation; not enforced)." },
				"code": { "type": "string" },
				"overwrite": { "type": "boolean", "default": false },
			}, ["name", "description", "code"]), Callable(self, "_tool_define_tool"))
	registry.register(_def("list_custom_tools",
			"List agent-defined tools: name, description, backing file, load state, and call count.",
			{}, [], { "serial": false }), Callable(self, "_tool_list_custom_tools"))
	registry.register(_def("delete_custom_tool",
			"Unregister an agent-defined tool and delete its backing file.",
			{ "name": { "type": "string" } }, ["name"]), Callable(self, "_tool_delete_custom_tool"))


static func _def(name: String, description: String, properties := {}, required: Array = [], extra := {}) -> Dictionary:
	var schema := { "type": "object", "properties": properties }
	if not required.is_empty():
		schema["required"] = required
	var def := { "name": name, "description": description, "input_schema": schema }
	def.merge(extra)
	return def


func _tool_editor_state(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var shell := ctx.shell
	if shell == null:
		return McpToolResult.error("The editor shell is not bound yet — ONED is still booting.")
	var workspaces: Array = []
	var table: Variant = shell.get("_workspaces")
	var instances: Array = table.values() if table is Dictionary else []
	var popup: Variant = shell.get("_environment_workspace")
	if popup != null:
		instances.append(popup)
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
		"camera": _camera_state(ctx),
		"perf_recent": _perf_lines(),
		"log_cursor": service.log_hub.latest_cursor() if service != null and service.get("log_hub") != null else 0,
	}
	return out


func _workspace_state(ws: Variant) -> Dictionary:
	if ws == null:
		return {}
	return {
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


func _mission_state(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("mission")
	var controller: Variant = ctx.mission()
	var out := { "loaded": false }
	if controller != null and controller.has_method("is_loaded"):
		out["loaded"] = bool(controller.is_loaded())
	if controller != null and controller.has_method("get_current_path"):
		out["path"] = controller.get_current_path()
	if controller != null and controller.has_method("get_stats"):
		out["stats"] = controller.get_stats()
	if controller != null and controller.has_method("is_simulating"):
		out["simulating"] = bool(controller.is_simulating())
		if controller.has_method("is_sim_playing"):
			out["sim_playing"] = bool(controller.is_sim_playing())
	if ws != null and ws.has_method("is_playing_mission"):
		out["pie_active"] = bool(ws.is_playing_mission())
	var sim: Variant = ctx.sim()
	if sim != null and sim.has_method("get_logic_tick"):
		out["tick"] = sim.get_logic_tick()
	return out


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
			"objects": ["shell", "editor", "mission_controller", "runtime", "sim", "camera", "resource_root",
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
		"runtime":
			return ctx.runtime()
		"sim":
			return ctx.sim()
		"camera":
			return ctx.camera()
		"resource_root":
			return ctx.root()
	return null


func _tool_list_assets(args: Dictionary, ctx: McpToolContext) -> Variant:
	var shell := ctx.shell
	if shell == null or ctx.root() == null:
		return McpToolResult.error("No resource directory mounted — set one in the editor's Settings (gear) popup, or via execute_script: ctx.shell.set_resource_root_dir(\"D:/Games/JO\").")
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
	var focus: Dictionary = args.get("focus", {}) if args.get("focus") is Dictionary else {}
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
	})
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


func _tool_execute_script(args: Dictionary, ctx: McpToolContext) -> Variant:
	var compiled := McpScriptRunner.compile(String(args.get("code", "")))
	if not compiled["ok"]:
		return McpToolResult.error("Script failed to compile:\n%s" % "\n".join(PackedStringArray(
				compiled["compile_errors"].map(func(e): return String(e)))))
	var timeout := clampi(int(args.get("timeout_ms", McpScriptRunner.DEFAULT_TIMEOUT_MS)),
			McpScriptRunner.MIN_TIMEOUT_MS, McpScriptRunner.MAX_TIMEOUT_MS)
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], ctx, timeout)
	return McpScriptRunner.result_from_outcome(outcome)


func _tool_define_tool(args: Dictionary, ctx: McpToolContext) -> Variant:
	var schema: Dictionary = args.get("input_schema", {}) if args.get("input_schema") is Dictionary else {}
	var outcome: Dictionary = service.dynamic_tools.define(
			String(args.get("name", "")), String(args.get("description", "")),
			schema, String(args.get("code", "")), bool(args.get("overwrite", false)))
	if not outcome["ok"]:
		return McpToolResult.error("define_tool failed:\n%s" % "\n".join(PackedStringArray(
				outcome["errors"].map(func(e): return String(e)))))
	ctx.log("custom tool '%s' registered" % args.get("name", ""))
	return {
		"ok": true,
		"tool": args.get("name", ""),
		"path": outcome["path"],
		"note": "Callable right now via tools/call. It appears in tools/list when your client next refreshes the list (this server has no push notifications). Persisted across editor restarts; manage with list_custom_tools / delete_custom_tool.",
	}


func _tool_list_custom_tools(_args: Dictionary, _ctx: McpToolContext) -> Variant:
	return { "tools": service.dynamic_tools.list() }


func _tool_delete_custom_tool(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var name := String(args.get("name", ""))
	var err: Error = service.dynamic_tools.delete(name)
	if err == ERR_DOES_NOT_EXIST:
		return McpToolResult.error("No custom tool named '%s' — see list_custom_tools." % name)
	if err != OK:
		return McpToolResult.error("Delete failed (%s)." % error_string(err))
	return { "ok": true, "deleted": name }
