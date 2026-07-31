class_name EditorMcpMissionTools
extends RefCounted

## The mission-authoring MCP tools: every mutation routes through the
## MissionController seams the editor UI itself uses — placement and moves bake
## the model's ground anchor against terrain heights the TOOL samples (callers
## never supply heights), waypoint markers ground the same way, properties go
## through the validated setters.
## Holding the API wrong (the raw-script era's floating objects) is impossible
## by construction.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")

## Watchdog budget for tools that (re)load the mission's terrain and place
## its objects — that path can take seconds on large maps.
const _MISSION_LOAD_TIMEOUT_MS := 120000
const ROW_CAP := 100
const INT_PROPS: Array[String] = [
	"group", "waypoint_id", "wp_number", "team", "ai_flags", "perception",
	"accuracy", "alert_state", "min_engagement_distance", "max_engagement_distance",
	"max_attack_distance", "spawn_count", "max_simultaneous", "no_less_than", "map_symbol",
]
const STRING_PROPS: Array[String] = ["name1", "name2"]
const HEADER_STRINGS: Array[String] = ["mission_name", "designer", "briefing", "terrain", "environment"]
const HEADER_INTS: Array[String] = [
	"climate", "weather", "mission_type", "attrib_flags", "start_time", "minutes_per_day",
	"player_health", "max_saves", "music", "reverb", "wind_speed", "wind_direction",
	"water_override", "fog_override",
]

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("new_mission",
			"Create a brand-new empty mission from scratch — no file until save_mission. A mission needs a terrain to place onto and reference: pass terrain (a .trn basename, e.g. \"Dvxi3\") to load it first, or omit to build on the already-loaded terrain. Replaces the open mission only when it has no unsaved changes (or discard=true). Switches focus to the Mission workspace. This is the fresh-authoring entry point — call it before list_items / place_entities when starting a new mission.",
			{
				"terrain": { "type": "string" },
				"discard": { "type": "boolean", "default": false },
			}, [], true, _MISSION_LOAD_TIMEOUT_MS), Callable(self, "_tool_new_mission"))
	registry.register(McpToolDef.make("list_items",
			"The placeable item palette (items.def) of the open mission: rows {id, name, type, kind, kind_label}. Pass id to place_entities. kind: 0=marker (player starts, gizmo markers), 1=item, 2=building, 3=person (AI organic). filter is a case-insensitive name substring. Requires an open mission (open_in_workspace workspace=\"mission\").",
			{
				"filter": { "type": "string", "default": "" },
				"kind": { "type": "integer", "enum": [0, 1, 2, 3] },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 2000 },
				"offset": { "type": "integer", "default": 0 },
			}), Callable(self, "_tool_list_items"))
	registry.register(McpToolDef.make("sample_terrain",
			"Terrain surface heights under world-space points: points=[[x,z],...] -> heights[i] = ground y, null when off the terrain. This is the live surface the editor grounds placements on — mostly diagnostic, since place_entities and edit_waypoint_path sample for you.",
			{
				"points": { "type": "array", "items": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 }, "minItems": 1, "maxItems": 1024 },
			}, ["points"]), Callable(self, "_tool_sample_terrain"))
	registry.register(McpToolDef.make("place_entities",
			"Place new entities standing ON the terrain, exactly as the editor's click-placement grounds them: the tool samples the surface at each (x, z) and bakes the model's ground anchor — never supply or guess heights. rows: [{item_id (from list_items), x, z (world-space), yaw_deg?, name1? (AI class), name2? (AI script), team?, group?, waypoint_id? (path the unit follows), properties? (int field -> value)}]. Off-terrain rows fail individually without aborting the batch; each placement is its own undo step, like hand-placing. Max 100 rows per call. Marks the mission dirty; never save unless asked.",
			{
				"rows": { "type": "array", "minItems": 1, "maxItems": 100, "items": { "type": "object",
					"properties": {
						"item_id": { "type": "integer" }, "x": { "type": "number" }, "z": { "type": "number" },
						"yaw_deg": { "type": "number" }, "name1": { "type": "string" }, "name2": { "type": "string" },
						"team": { "type": "integer" }, "group": { "type": "integer" }, "waypoint_id": { "type": "integer" },
						"properties": { "type": "object" },
					}, "required": ["item_id", "x", "z"] } },
			}, ["rows"], true, _MISSION_LOAD_TIMEOUT_MS), Callable(self, "_tool_place_entities"))
	registry.register(McpToolDef.make("get_mission_entities",
			"List or inspect the open mission's authored entities. Record positions are BMS mission-space ({x,y,z}, z up) plus a world [x,y,z] echo (y up). Filters: kind (0=marker 1=item 2=building 3=person), name (model-name substring), team, group, item_id. detail={kind,index} returns ONE entity's complete authored record instead.",
			{
				"kind": { "type": "integer", "enum": [0, 1, 2, 3] },
				"name": { "type": "string" },
				"team": { "type": "integer" },
				"group": { "type": "integer" },
				"item_id": { "type": "integer" },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"offset": { "type": "integer", "default": 0 },
				"detail": { "type": "object", "properties": { "kind": { "type": "integer" }, "index": { "type": "integer" } } },
			}), Callable(self, "_tool_get_entities"))
	registry.register(McpToolDef.make("edit_mission_entity",
			"Edit one existing entity by (kind, index) — exactly one op per call. move={x,z}: re-grounds it on the terrain at the new spot (the editor's drag bake; never pass y). rotate={yaw_deg?,pitch_deg?,roll_deg?}: omitted axes keep their value (stored as integer degrees). set={int property -> value} (group, waypoint_id, wp_number, team, ai_flags, perception, accuracy, alert_state, min/max_engagement_distance, max_attack_distance, spawn_count, max_simultaneous, no_less_than, map_symbol). set_strings={name1? (AI class), name2? (AI script)}. delete=true removes it (later indices of that kind shift — re-list before further edits). One undo step per call (set: one per field).",
			{
				"kind": { "type": "integer" }, "index": { "type": "integer" },
				"move": { "type": "object", "properties": { "x": { "type": "number" }, "z": { "type": "number" } } },
				"rotate": { "type": "object", "properties": { "yaw_deg": { "type": "number" }, "pitch_deg": { "type": "number" }, "roll_deg": { "type": "number" } } },
				"set": { "type": "object" },
				"set_strings": { "type": "object", "properties": { "name1": { "type": "string" }, "name2": { "type": "string" } } },
				"delete": { "type": "boolean" },
			}, ["kind", "index"]), Callable(self, "_tool_edit_entity"))
	registry.register(McpToolDef.make("edit_waypoint_path",
			"Author waypoint paths — the routes AI units follow (a unit follows its waypoint_id path). One op per call: new_path, select_path, add_markers, set_flags, assign_entity, delete_marker, or list. Marker points are grounded from world x/z.",
			{
				"op": { "type": "string", "enum": ["new_path", "select_path", "add_markers", "set_flags", "assign_entity", "delete_marker", "list"] },
				"path": { "type": "integer" },
				"points": { "type": "array", "items": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 }, "maxItems": 100 },
				"loop": { "type": "boolean", "default": true },
				"blue": { "type": "boolean", "default": false },
				"red": { "type": "boolean", "default": false },
				"kind": { "type": "integer" }, "index": { "type": "integer" },
				"marker_index": { "type": "integer" },
			}, ["op"]), Callable(self, "_tool_waypoints"))
	registry.register(McpToolDef.make("set_mission_header",
			"Set mission header fields; fields is a {name -> value} map, one undo step per field. Strings: mission_name, designer, briefing, environment (.env basename — the editor preview reloads to match the game, including the mission's fog/water overrides), terrain (terrain basename — does NOT reload the loaded terrain; avoid unless asked). Ints: climate, weather, mission_type, attrib_flags, start_time, minutes_per_day, player_health, max_saves, music, reverb, wind_speed, wind_direction, water_override (s16 half-units), fog_override (fog distance). The two *_override values only take effect when their attrib_flags bit is set (water 0x1, fog-distance 0x2) — set both, or the value is dormant (and an enabled bit with a 0 value fogs the map out). Unknown fields are rejected up front with this list.",
			{ "fields": { "type": "object" } }, ["fields"]), Callable(self, "_tool_set_header"))
	registry.register(McpToolDef.make("reground_mission",
			"Repair: snap EVERY entity back onto the terrain surface in one undo step. Use when objects float or sink (placed at guessed heights, or terrain edits moved the ground). Unlike the editor's drift prompt this does not skip rows whose terrain never changed, so it fixes mis-grounded missions — but entities deliberately authored off the ground get planted too; warn the user if that may apply.",
			{}, [], true, _MISSION_LOAD_TIMEOUT_MS), Callable(self, "_tool_reground"))
	registry.register(McpToolDef.make("analyze_mission",
			"Composition report for a mission — the open one (no args) or ANY .bms by name/path (read-only; nothing opens in the editor). Header info, entity counts by kind, the most-used items with names, the densest 64m world-space cells (centers you can set_camera at), waypoint path summaries with loop bits, and team / AI-class (name1) / alert distributions for AI persons. Use it to study a stock mission's patterns before authoring in its style.",
			{
				"path": { "type": "string" },
				"top": { "type": "integer", "default": 15, "minimum": 1, "maximum": 50 },
			}), Callable(self, "_tool_analyze"))
	registry.register(McpToolDef.make("analyze_mounts",
			"Read-only NPC mount/seat diagnostic for attach commands 123/124/125. Uses the same seat extraction rules as MissionRuntime: authored organic -> target SSN -> target model userpoints -> predicted seat. path can name any .bms through the mounted resource root; omit it for the open mission.",
			{
				"path": { "type": "string" },
				"command_ids": { "type": "array", "items": { "type": "integer" } },
				"ssn": { "type": "integer" },
				"target_ssn": { "type": "integer" },
				"only_problems": { "type": "boolean", "default": false },
				"limit": { "type": "integer", "default": 200, "minimum": 1, "maximum": 1000 },
				"offset": { "type": "integer", "default": 0, "minimum": 0 },
			}, [], false), Callable(self, "_tool_analyze_mounts"))
	registry.register(McpToolDef.make("save_mission",
			"Write the open mission to disk. ONLY call this when the user explicitly asked to save. No args: saves to its current path (errors if never saved — pass path). path: a filename (\"patrol.bms\", written into the mounted resource root) or an absolute path; must end in .bms and becomes the mission's current path. Clears the dirty flag; undo history survives.",
			{ "path": { "type": "string" } }), Callable(self, "_tool_save"))


# --- shared guards / helpers ---------------------------------------------------

# The open mission's controller, or an error result. Every mission tool funnels
# through here so the "open a mission first" guidance is uniform.
func _require_mission(ctx: McpToolContext) -> Dictionary:
	var controller: Variant = ctx.mission()
	if controller == null or not controller.has_method("is_loaded") or not controller.is_loaded():
		return { "error": "No mission open — open_in_workspace(workspace=\"mission\", path=...) first; list candidates with list_assets(kind=\"mission\")." }
	return { "controller": controller }


# Mission present and editable.
func _require_editable(ctx: McpToolContext) -> Dictionary:
	return _require_mission(ctx)


# Batch-sample terrain heights at world (x, z) pairs; null per off-terrain point.
func _sample(ctx: McpToolContext, points: PackedVector2Array) -> Array:
	var out: Array = []
	if ctx.editor == null or not ctx.editor.has_method("sample_heights_world"):
		return out
	var heights: PackedFloat32Array = ctx.editor.sample_heights_world(points)
	for h in heights:
		out.append(null if is_nan(h) else h)
	return out


static func _kind_label(kind: int) -> String:
	match kind:
		NovaMissionData.KIND_MARKER:
			return "marker"
		NovaMissionData.KIND_ITEM:
			return "item"
		NovaMissionData.KIND_BUILDING:
			return "building"
		NovaMissionData.KIND_ORGANIC:
			return "person"
	return str(kind)


static func _world_echo(bms_pos: Vector3) -> Array:
	var world := MissionObjectPlacer.bms_to_godot_position(bms_pos)
	return [world.x, world.y, world.z]


# --- discovery ------------------------------------------------------------------

# Create a fresh empty mission, optionally loading a terrain first. Mirrors the
# editor's New action (MissionController.new_mission) so authoring needs no manual
# click — the gap that made the MCP server require human interaction.
func _tool_new_mission(args: Dictionary, ctx: McpToolContext) -> Variant:
	var controller: Variant = ctx.mission()
	if controller == null or not controller.has_method("new_mission"):
		return McpToolResult.error("Mission workspace unavailable.")
	if controller.has_method("is_loaded") and controller.is_loaded() \
			and controller.has_method("is_dirty") and controller.is_dirty() \
			and not bool(args.get("discard", false)):
		return McpToolResult.error("The open mission has unsaved changes — save_mission first, or pass discard: true to replace it.")
	if ctx.shell == null:
		return McpToolResult.error("The editor shell is not bound yet.")
	var terrain := String(args.get("terrain", "")).strip_edges()
	if not terrain.is_empty():
		var resolved := McpAssetDescribe.resolve(ctx, terrain)
		if not resolved["ok"]:
			return McpToolResult.error("Terrain '%s' not found: %s. List with list_assets(kind=\"terrain\")." % [terrain, String(resolved.get("error", ""))])
		var target := String(resolved["path"]) if resolved.get("loose", false) else String(resolved["name"])
		var terr_err: Error = ctx.shell.open_in_workspace("terrain", target)
		if terr_err != OK:
			return McpToolResult.error("Could not load terrain '%s' (%s)." % [terrain, error_string(terr_err)])
		await ctx.frames(1)
	var err: Error = controller.new_mission()
	if err != OK:
		return McpToolResult.error("New mission failed: %s" % controller.get_last_status())
	# Bring the Mission workspace forward so the human sees the empty world.
	var mission_ws_id := int(ctx.shell._workspace_id_for_resource_kind("mission"))
	if mission_ws_id != -1 and ctx.shell.has_method("set_active_workspace"):
		ctx.shell.set_active_workspace(mission_ws_id)
		await ctx.frames(1)
	var info: Dictionary = controller.get_mission().get_info() if controller.has_method("get_mission") else {}
	return { "ok": true, "terrain": info.get("terrain", ""), "status": controller.get_last_status(), "dirty": controller.is_dirty() }


func _tool_list_items(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_mission(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var filter := String(args.get("filter", "")).to_lower()
	var want_kind := int(args.get("kind", -1)) if args.has("kind") else -1
	var matched: Array = []
	for item: Dictionary in controller.get_placeable_items():
		var kind := int(NovaMissionData.kind_for_item_type(int(item["type"])))
		if want_kind >= 0 and kind != want_kind:
			continue
		if not filter.is_empty() and not String(item["display_name"]).to_lower().contains(filter):
			continue
		matched.append({
			"id": item["id"], "name": item["display_name"], "type": item["type"],
			"kind": kind, "kind_label": _kind_label(kind),
		})
	var offset := maxi(int(args.get("offset", 0)), 0)
	var limit := clampi(int(args.get("limit", 200)), 1, 2000)
	var page := matched.slice(offset, offset + limit)
	return { "items": page, "total": matched.size(), "truncated": offset + page.size() < matched.size() }


func _tool_sample_terrain(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_mission(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	if ctx.editor == null or not ctx.editor.has_method("sample_heights_world"):
		return McpToolResult.error("No terrain loaded — open a terrain or mission first.")
	var points := PackedVector2Array()
	for pair in args.get("points", []):
		if pair is Array and pair.size() >= 2:
			points.append(Vector2(float(pair[0]), float(pair[1])))
	if points.is_empty():
		return McpToolResult.error("points must be [[x, z], ...] in world space.")
	var heights := _sample(ctx, points)
	var off := 0
	for h in heights:
		if h == null:
			off += 1
	return { "heights": heights, "off_terrain": off }


# --- placement / editing ---------------------------------------------------------

func _tool_place_entities(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var rows: Array = args.get("rows", []) if args.get("rows") is Array else []
	if rows.is_empty():
		return McpToolResult.error("rows is required: [{item_id, x, z, ...}].")
	if rows.size() > ROW_CAP:
		return McpToolResult.error("Too many rows (%d) — max %d per call; batch across calls." % [rows.size(), ROW_CAP])
	var points := PackedVector2Array()
	for row: Dictionary in rows:
		points.append(Vector2(float(row.get("x", 0.0)), float(row.get("z", 0.0))))
	var heights := _sample(ctx, points)
	if heights.is_empty():
		return McpToolResult.error("No terrain loaded to ground placements on.")
	var results: Array = []
	var placed := 0
	for i in range(rows.size()):
		var row: Dictionary = rows[i]
		if heights[i] == null:
			results.append({ "ok": false, "error": "off the terrain at (%.0f, %.0f)" % [points[i].x, points[i].y] })
			continue
		var hit := Vector3(points[i].x, float(heights[i]), points[i].y)
		if not controller.place_entity_at_world(int(row.get("item_id", 0)), hit):
			results.append({ "ok": false, "error": String(controller.get_last_status()) })
			continue
		var sel: Dictionary = controller.get_selection_summary()
		var row_out := {
			"ok": true,
			"kind": sel.get("kind", -1), "kind_label": _kind_label(int(sel.get("kind", -1))),
			"index": sel.get("index", -1), "grounded_y": heights[i],
		}
		var issue := _apply_row_extras(controller, row)
		if not issue.is_empty():
			row_out["warning"] = issue
		results.append(row_out)
		placed += 1
		if placed % 10 == 0:
			await ctx.frames(1)
	return { "placed": placed, "failed": rows.size() - placed, "rows": results, "dirty": controller.is_dirty() }


# Post-place extras for one row (rotation, AI fields, properties) on the
# still-selected new entity. Returns a warning string for rejected keys.
func _apply_row_extras(controller: Variant, row: Dictionary) -> String:
	var rejected := PackedStringArray()
	var yaw := float(row.get("yaw_deg", 0.0))
	if absf(yaw) > 0.01:
		controller.set_selected_rotation(Vector3(0, yaw, 0))
		controller.commit_edit()
	if row.has("team"):
		controller.set_selected_team(int(row["team"]))
	if row.has("group"):
		controller.set_selected_group(int(row["group"]))
	if row.has("waypoint_id"):
		controller.set_selected_property("waypoint_id", int(row["waypoint_id"]))
	for key in ["name1", "name2"]:
		if row.has(key):
			controller.set_selected_string_property(key, String(row[key]))
	var properties: Dictionary = row.get("properties", {}) if row.get("properties") is Dictionary else {}
	for key in properties:
		if INT_PROPS.has(String(key)):
			controller.set_selected_property(String(key), int(properties[key]))
		else:
			rejected.append(String(key))
	if rejected.is_empty():
		return ""
	return "unknown properties ignored: %s (valid: %s)" % [", ".join(rejected), ", ".join(INT_PROPS)]


func _tool_get_entities(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_mission(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var mission: Variant = controller.get_mission()
	if args.get("detail") is Dictionary:
		var detail: Dictionary = args["detail"]
		var record: Dictionary = mission.get_entity(int(detail.get("kind", -1)), int(detail.get("index", -1)))
		if record.is_empty():
			return McpToolResult.error("No entity at (kind=%s, index=%s) — list with get_mission_entities." % [detail.get("kind"), detail.get("index")])
		record["kind_label"] = _kind_label(int(detail.get("kind", -1)))
		if record.get("position") is Vector3:
			record["world_position"] = _world_echo(record["position"])
		return record
	var matched: Array = []
	for row: Dictionary in controller.get_object_list():
		if args.has("kind") and int(row.get("kind", -1)) != int(args["kind"]):
			continue
		if args.has("item_id") and int(row.get("item_id", -1)) != int(args["item_id"]):
			continue
		if args.has("name") and not String(row.get("name", "")).to_lower().contains(String(args["name"]).to_lower()):
			continue
		matched.append(row)
	var offset := maxi(int(args.get("offset", 0)), 0)
	var limit := clampi(int(args.get("limit", 200)), 1, 1000)
	var out: Array = []
	var skipped := 0
	for row: Dictionary in matched:
		var record: Dictionary = mission.get_entity(int(row["kind"]), int(row["index"]))
		if args.has("team") and int(record.get("team", -1)) != int(args["team"]):
			skipped += 1
			continue
		if args.has("group") and int(record.get("group", record.get("group_id", -1))) != int(args["group"]):
			skipped += 1
			continue
		var entry := {
			"kind": row["kind"], "kind_label": _kind_label(int(row["kind"])),
			"index": row["index"], "item_id": row["item_id"], "name": row["name"],
			"position": record.get("position"),
			"rotation_deg": record.get("rotation_deg"),
			"team": record.get("team"), "waypoint_id": record.get("waypoint_id"),
		}
		if record.get("position") is Vector3:
			entry["world_position"] = _world_echo(record["position"])
		out.append(entry)
	var page := out.slice(offset, offset + limit)
	return { "entities": page, "total": out.size(), "truncated": offset + page.size() < out.size() }


func _tool_edit_entity(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var ops := PackedStringArray()
	for op in ["move", "rotate", "set", "set_strings", "delete"]:
		if args.has(op):
			ops.append(op)
	if ops.size() != 1:
		return McpToolResult.error("Exactly one op per call (move | rotate | set | set_strings | delete); got: %s" % [ops])
	var kind := int(args.get("kind", -1))
	var index := int(args.get("index", -1))
	controller.select_object(kind, index)
	if (controller.get_selection_summary() as Dictionary).is_empty():
		return McpToolResult.error("No entity at (kind=%d, index=%d) — get_mission_entities lists them." % [kind, index])
	var op := String(ops[0])
	match op:
		"move":
			var move: Dictionary = args["move"]
			var heights := _sample(ctx, PackedVector2Array([Vector2(float(move.get("x", 0)), float(move.get("z", 0)))]))
			if heights.is_empty() or heights[0] == null:
				return McpToolResult.error("(%.0f, %.0f) is off the terrain." % [float(move.get("x", 0)), float(move.get("z", 0))])
			if not controller.move_selected_to_world_grounded(Vector3(float(move["x"]), float(heights[0]), float(move["z"]))):
				return McpToolResult.error("Move rejected: %s" % controller.get_last_status())
		"rotate":
			var rotate: Dictionary = args["rotate"]
			var current: Vector3 = controller.get_selected_rotation()
			var merged := Vector3(
				float(rotate.get("pitch_deg", current.x)),
				float(rotate.get("yaw_deg", current.y)),
				float(rotate.get("roll_deg", current.z)))
			controller.set_selected_rotation(merged)
			controller.commit_edit()
		"set":
			var fields: Dictionary = args["set"]
			for key in fields:
				if not INT_PROPS.has(String(key)):
					return McpToolResult.error("Unknown property '%s'. Valid: %s" % [key, ", ".join(INT_PROPS)])
			for key in fields:
				match String(key):
					"team":
						controller.set_selected_team(int(fields[key]))
					"group":
						controller.set_selected_group(int(fields[key]))
					_:
						controller.set_selected_property(String(key), int(fields[key]))
		"set_strings":
			var strings: Dictionary = args["set_strings"]
			for key in strings:
				if not STRING_PROPS.has(String(key)):
					return McpToolResult.error("Unknown string property '%s'. Valid: %s" % [key, ", ".join(STRING_PROPS)])
			for key in strings:
				controller.set_selected_string_property(String(key), String(strings[key]))
		"delete":
			if not bool(args["delete"]):
				return McpToolResult.error("delete must be true to remove the entity.")
			if not controller.delete_selected():
				return McpToolResult.error("Delete rejected: %s" % controller.get_last_status())
			return { "ok": true, "op": "delete", "dirty": controller.is_dirty() }
	var record: Dictionary = controller.get_mission().get_entity(kind, index)
	if record.get("position") is Vector3:
		record["world_position"] = _world_echo(record["position"])
	return { "ok": true, "op": op, "entity": record, "dirty": controller.is_dirty() }


# --- waypoints -------------------------------------------------------------------

func _tool_waypoints(args: Dictionary, ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	var gate := _require_mission(ctx) if op == "list" else _require_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	match op:
		"new_path":
			controller.set_waypoint_mode(true)
			var index := int(controller.select_new_waypoint_path())
			if index < 0:
				return McpToolResult.error("All 128 waypoint paths are in use.")
			return { "ok": true, "path": index, "active": controller.get_active_waypoint_path() }
		"select_path":
			controller.set_waypoint_mode(true)
			controller.select_waypoint_path(int(args.get("path", -1)))
			var active: Dictionary = controller.get_active_waypoint_path()
			if active.is_empty():
				return McpToolResult.error("No path %s — op=\"list\" shows them; op=\"new_path\" creates one." % args.get("path"))
			return { "ok": true, "active": active }
		"add_markers":
			if int(controller.get_selected_waypoint_path_index()) < 0:
				return McpToolResult.error("No active path — op=\"new_path\" or op=\"select_path\" first.")
			var raw: Array = args.get("points", []) if args.get("points") is Array else []
			if raw.is_empty():
				return McpToolResult.error("points is required: [[x, z], ...] in world space.")
			var points := PackedVector2Array()
			for pair in raw:
				points.append(Vector2(float(pair[0]), float(pair[1])))
			var heights := _sample(ctx, points)
			var added := 0
			var results: Array = []
			for i in range(points.size()):
				if heights[i] == null:
					results.append({ "ok": false, "error": "off the terrain" })
					continue
				if controller.add_marker_to_active_path_at_world(Vector3(points[i].x, float(heights[i]), points[i].y)):
					added += 1
					results.append({ "ok": true, "marker": controller.get_selected_marker() })
				else:
					results.append({ "ok": false, "error": String(controller.get_last_status()) })
				if added % 25 == 0:
					await ctx.frames(1)
			return { "added": added, "rows": results, "active": controller.get_active_waypoint_path(), "dirty": controller.is_dirty() }
		"set_flags":
			if int(controller.get_selected_waypoint_path_index()) < 0:
				return McpToolResult.error("No active path — select one first.")
			controller.set_waypoint_flags(bool(args.get("loop", true)), bool(args.get("blue", false)), bool(args.get("red", false)))
			return { "ok": true, "active": controller.get_active_waypoint_path(), "dirty": controller.is_dirty() }
		"assign_entity":
			controller.select_object(int(args.get("kind", -1)), int(args.get("index", -1)))
			if (controller.get_selection_summary() as Dictionary).is_empty():
				return McpToolResult.error("No entity at (kind=%s, index=%s)." % [args.get("kind"), args.get("index")])
			controller.set_selected_property("waypoint_id", int(args.get("path", 0)))
			return { "ok": true, "dirty": controller.is_dirty() }
		"delete_marker":
			if int(controller.get_selected_waypoint_path_index()) < 0:
				return McpToolResult.error("No active path — select one first.")
			controller.select_waypoint_marker(int(args.get("marker_index", -1)))
			if (controller.get_selected_marker() as Dictionary).is_empty():
				return McpToolResult.error("No marker %s on the active path." % args.get("marker_index"))
			if not controller.delete_selected_marker():
				return McpToolResult.error("Delete rejected: %s" % controller.get_last_status())
			return { "ok": true, "active": controller.get_active_waypoint_path(), "dirty": controller.is_dirty() }
		"list":
			return {
				"paths": controller.get_waypoint_summaries(),
				"active": controller.get_active_waypoint_path(),
			}
	return McpToolResult.error("Unknown op '%s'." % op)


# --- header / repair / analysis ----------------------------------------------------

func _tool_set_header(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var fields: Dictionary = args.get("fields", {}) if args.get("fields") is Dictionary else {}
	if fields.is_empty():
		return McpToolResult.error("fields is required: {name -> value}.")
	for key in fields:
		var name := String(key)
		if not HEADER_STRINGS.has(name) and not HEADER_INTS.has(name):
			return McpToolResult.error("Unknown header field '%s'. Strings: %s. Ints: %s." % [name, ", ".join(HEADER_STRINGS), ", ".join(HEADER_INTS)])
	var applied := PackedStringArray()
	var env_changed := false
	for key in fields:
		var name := String(key)
		if HEADER_STRINGS.has(name):
			controller.set_header_string(name, String(fields[key]))
			env_changed = env_changed or name == "environment"
		else:
			controller.set_header_int(name, int(fields[key]))
		applied.append(name)
	var note := ""
	if env_changed and controller.has_method("reload_environment"):
		note = String(controller.reload_environment())
	var out := { "applied": applied, "dirty": controller.is_dirty() }
	if not note.is_empty():
		out["environment_note"] = note
	return out


func _tool_reground(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var out: Dictionary = controller.reground_all()
	out["dirty"] = controller.is_dirty()
	out["status"] = controller.get_last_status()
	return out


func _tool_analyze(args: Dictionary, ctx: McpToolContext) -> Variant:
	var mission: Variant = null
	var names := {}
	var source := ""
	if args.has("path") and not String(args["path"]).is_empty():
		mission = NovaMissionData.new()
		var opened := McpAssetDescribe.open_data(mission, ctx, String(args["path"]))
		if not opened["ok"]:
			return McpToolResult.error(String(opened["error"]))
		source = String(opened["path"])
		var db := NovaItemDatabase.new()
		if ctx.root() != null and db.load_from_resource_root(ctx.root(), "items.def") == OK:
			for item: Dictionary in db.get_items():
				names[int(item["id"])] = item["display_name"]
	else:
		var gate := _require_mission(ctx)
		if gate.has("error"):
			return McpToolResult.error(gate["error"])
		var controller: Variant = gate["controller"]
		mission = controller.get_mission()
		source = String(controller.get_current_path())
		for item: Dictionary in controller.get_placeable_items():
			names[int(item["id"])] = item["display_name"]
	var top := clampi(int(args.get("top", 15)), 1, 50)
	var entities: Array = mission.get_all_entities()
	var by_kind := {}
	var item_freq := {}
	var cells := {}
	var teams := {}
	var classes := {}
	for entity: Dictionary in entities:
		var kind := int(entity.get("kind", -1))
		var label := _kind_label(kind)
		by_kind[label] = int(by_kind.get(label, 0)) + 1
		var item_id := int(entity.get("item_id", 0))
		item_freq[item_id] = int(item_freq.get(item_id, 0)) + 1
		if entity.get("position") is Vector3:
			var world := MissionObjectPlacer.bms_to_godot_position(entity["position"])
			var cell := Vector2i(floori(world.x / 64.0), floori(world.z / 64.0))
			cells[cell] = int(cells.get(cell, 0)) + 1
		if kind == NovaMissionData.KIND_ORGANIC:
			var team := str(entity.get("team", "?"))
			teams[team] = int(teams.get(team, 0)) + 1
			var name1 := String(entity.get("name1", ""))
			if not name1.is_empty():
				classes[name1] = int(classes.get(name1, 0)) + 1
	var top_items: Array = []
	for id in item_freq:
		top_items.append([item_freq[id], id, names.get(id, "?")])
	top_items.sort_custom(func(a, b): return a[0] > b[0])
	var top_cells: Array = []
	for cell in cells:
		top_cells.append([cells[cell], cell])
	top_cells.sort_custom(func(a, b): return a[0] > b[0])
	var dense: Array = []
	for row in top_cells.slice(0, top):
		var cell: Vector2i = row[1]
		dense.append({ "count": row[0], "world_center": [cell.x * 64 + 32, cell.y * 64 + 32] })
	var paths: Array = []
	for summary: Dictionary in mission.get_waypoint_summaries():
		if int(summary.get("marker_count", 0)) > 0:
			paths.append(summary)
	return {
		"source": source,
		"info": mission.get_info(),
		"total_entities": entities.size(),
		"by_kind": by_kind,
		"top_items": top_items.slice(0, top).map(func(row): return { "count": row[0], "id": row[1], "name": row[2] }),
		"densest_cells_64m": dense,
		"waypoint_paths": paths,
		"persons": { "teams": teams, "ai_classes": classes },
	}


func _tool_analyze_mounts(args: Dictionary, ctx: McpToolContext) -> Variant:
	var loaded := _mission_for_readonly_analysis(args, ctx)
	if loaded.has("error"):
		return McpToolResult.error(loaded["error"])
	var mission: Variant = loaded["mission"]
	var source := String(loaded.get("source", ""))
	var item_db: Variant = _item_db_for_mount_analysis(ctx, loaded.get("controller"))
	var root: Variant = ctx.root()
	var command_ids := _mount_command_filter(args.get("command_ids", []))
	var target_by_ssn := _entities_by_bms_id(mission.get_all_entities())
	var seat_cache := {}
	var rows: Array = []
	for raw in mission.get_all_entities():
		var organic: Dictionary = raw
		if int(organic.get("kind", -1)) != NovaMissionData.KIND_ORGANIC:
			continue
		var command_id := int(organic.get("waypoint_id", 0))
		if not command_ids.has(command_id):
			continue
		var ssn := int(organic.get("bms_id", organic.get("id", 0)))
		if args.has("ssn") and ssn != int(args["ssn"]):
			continue
		var target_ssn := int(organic.get("wp_number", 0))
		if args.has("target_ssn") and target_ssn != int(args["target_ssn"]):
			continue
		var target: Dictionary = target_by_ssn.get(target_ssn, {})
		var row := _mount_analysis_row(organic, target, command_id, root, item_db, seat_cache)
		if bool(args.get("only_problems", false)) and not _mount_row_has_problem(row):
			continue
		rows.append(row)
	var offset := maxi(int(args.get("offset", 0)), 0)
	var limit := clampi(int(args.get("limit", 200)), 1, 1000)
	var page := rows.slice(offset, offset + limit)
	return {
		"source": source,
		"runtime_parity": {
			"shared_rules": "MissionRuntime",
			"editor_mode": "static_analysis",
			"live_runtime": "GameWorld",
			"source": "static",
		},
		"mounts": page,
		"total": rows.size(),
		"truncated": offset + page.size() < rows.size(),
	}


func _mission_for_readonly_analysis(args: Dictionary, ctx: McpToolContext) -> Dictionary:
	if args.has("path") and not String(args["path"]).is_empty():
		var mission := NovaMissionData.new()
		var opened := McpAssetDescribe.open_data(mission, ctx, String(args["path"]))
		if not opened["ok"]:
			return { "error": String(opened["error"]) }
		return { "mission": mission, "source": String(opened["path"]) }
	var gate := _require_mission(ctx)
	if gate.has("error"):
		return gate
	var controller: Variant = gate["controller"]
	return {
		"mission": controller.get_mission(),
		"source": String(controller.get_current_path()),
		"controller": controller,
	}


func _item_db_for_mount_analysis(ctx: McpToolContext, controller: Variant) -> Variant:
	if ctx.root() != null:
		var db := NovaItemDatabase.new()
		if db.load_from_resource_root(ctx.root(), "items.def") == OK:
			return db
	if controller != null and controller.has_method("_item_db"):
		return controller._placement._item_db()
	return null


func _mount_command_filter(raw: Variant) -> Dictionary:
	var out := {}
	if raw is Array and not (raw as Array).is_empty():
		for value in raw:
			out[int(value)] = true
	else:
		out[ItemSeatSpecs.COMMAND_PASSENGER_ONLY] = true
		out[ItemSeatSpecs.COMMAND_SKIP_CONTROLLER] = true
		out[ItemSeatSpecs.COMMAND_ANY_SEAT] = true
	return out


func _entities_by_bms_id(entities: Array) -> Dictionary:
	var out := {}
	for raw in entities:
		var entity: Dictionary = raw
		var ssn := int(entity.get("bms_id", entity.get("id", 0)))
		if ssn != 0:
			out[ssn] = entity
	return out


func _mount_analysis_row(organic: Dictionary, target: Dictionary, command_id: int, root: Variant,
		item_db: Variant, seat_cache: Dictionary) -> Dictionary:
	var diagnostics: Array = []
	var target_ssn := int(organic.get("wp_number", 0))
	var seats: Array = []
	var target_card := { "bms_id": target_ssn, "found": false, "seat_count": 0 }
	if target.is_empty():
		diagnostics.append("target_missing")
	else:
		target_card = _mount_entity_card(target, item_db)
		target_card["found"] = true
		var type_id := int(target.get("type_id", 0))
		if not seat_cache.has(type_id):
			seat_cache[type_id] = ItemSeatSpecs.seat_specs_for_item(
					root, item_db, int(target.get("item_id", 0)), type_id, true)
		var spec: Dictionary = seat_cache[type_id]
		seats = spec.get("seats", [])
		target_card["display_name"] = spec.get("display_name", target_card.get("display_name", ""))
		target_card["graphic"] = spec.get("graphic", "")
		target_card["model"] = spec.get("model", "")
		target_card["seat_count"] = seats.size()
		if not String(spec.get("error", "")).is_empty():
			target_card["seat_error"] = String(spec["error"])
			diagnostics.append(String(spec["error"]))
		if seats.is_empty():
			diagnostics.append("no_target_seats")
	var prediction := ItemSeatSpecs.predict_best_seat(seats, command_id)
	if int(prediction.get("seat_index", -1)) < 0 and not seats.is_empty():
		diagnostics.append("no_eligible_seat")
	var row := {
		"organic": _mount_entity_card(organic, item_db),
		"command": ItemSeatSpecs.command_rule(command_id),
		"target": target_card,
		"seat_candidates": prediction.get("candidates", []),
		"prediction": {
			"seat_index": prediction.get("seat_index", -1),
			"seat": prediction.get("seat", {}),
		},
		"diagnostics": diagnostics,
	}
	diagnostics.append("static_only")
	return row


func _mount_entity_card(entity: Dictionary, item_db: Variant) -> Dictionary:
	var item_id := int(entity.get("item_id", 0))
	var out := {
		"kind": int(entity.get("kind", -1)),
		"kind_label": _kind_label(int(entity.get("kind", -1))),
		"index": int(entity.get("index", -1)),
		"bms_id": int(entity.get("bms_id", entity.get("id", 0))),
		"item_id": item_id,
		"type_id": int(entity.get("type_id", 0)),
		"position": entity.get("position", Vector3.ZERO),
		"rotation_deg": entity.get("rotation_deg", Vector3.ZERO),
		"team": entity.get("team"),
		"waypoint_id": entity.get("waypoint_id"),
		"wp_number": entity.get("wp_number"),
	}
	if entity.get("position") is Vector3:
		out["world_position"] = _world_echo(entity["position"])
	if item_db != null and item_id != 0 and item_db.has_method("has_item") and item_db.has_item(item_id):
		out["display_name"] = String(item_db.get_display_name(item_id))
		out["graphic"] = String(item_db.get_graphic(item_id))
	return out


func _mount_row_has_problem(row: Dictionary) -> bool:
	for item in row.get("diagnostics", []):
		if String(item) != "static_only":
			return true
	return false


# --- save -----------------------------------------------------------------------

func _tool_save(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_mission(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var controller: Variant = gate["controller"]
	var path := String(args.get("path", "")).strip_edges()
	var err: Error
	if path.is_empty():
		err = controller.save_current()
		if err == ERR_INVALID_PARAMETER:
			return McpToolResult.error("This mission has never been saved — pass path (a .bms filename or absolute path).")
	else:
		if path.get_extension().to_lower() != "bms":
			return McpToolResult.error("path must end in .bms.")
		if path.is_relative_path():
			var root_dir := String(ctx.shell.get_resource_root_dir()) if ctx.shell != null and ctx.shell.has_method("get_resource_root_dir") else ""
			if root_dir.is_empty():
				return McpToolResult.error("No resource root mounted to resolve a relative filename — pass an absolute path.")
			path = root_dir.path_join(path)
		err = controller.save_as_path(path)
	if err != OK:
		return McpToolResult.error("Save failed (%s): %s" % [error_string(err), controller.get_last_status()])
	return { "ok": true, "path": controller.get_current_path(), "status": controller.get_last_status(), "dirty": controller.is_dirty() }
