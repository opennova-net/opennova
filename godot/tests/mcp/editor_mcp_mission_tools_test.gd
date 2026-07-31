extends GutTest

# The curated mission MCP tools, end-to-end on a REAL terrain (dvxi5 fixture)
# through the real TerrainEditor scene — heights are live, so grounded
# placement is provable: place via the tool, then reground_mission must move
# NOTHING (the tool grounded exactly like the editor). The raw-position float
# is the recorded failure mode the reground tool must repair.
#
# Assertions read mission records (never MultiMesh readbacks — headless gotcha).

# The editor scene: an EditorApp root holding the TerrainEditor node, whose
# open_trn() loads live terrain heights.
const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
const MissionWorkspace := preload("res://modtools/mission/mission_workspace.gd")

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"
const ITEMS_PATH := "res://../fixtures/def/items.def"
const DSUV1_MODEL := "res://../fixtures/3dp/dsuv1/dsuv1.3di"
const SAVE_DIR := "user://mcp_mission_tools_test"

var editor: Node
var workspace: RefCounted
var controller: RefCounted
var service: EditorMcpService
var shell: Node
var mount_root_dir := ""


class ShellStub:
	extends Node

	var _workspaces := {}
	var root_dir := ""
	var root: NovaResourceRoot = null
	var editor_node: Node = null

	func get_resource_root_dir() -> String:
		return root_dir

	func get_resource_root() -> NovaResourceRoot:
		return root

	func get_editor_camera() -> Camera3D:
		return editor_node.get("camera") as Camera3D if editor_node != null else null

	func _get_active_workspace() -> Variant:
		return _workspaces.values()[0] if not _workspaces.is_empty() else null


func _abs(p: String) -> String:
	return ProjectSettings.globalize_path(p)


func before_each() -> void:
	editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	assert_eq(editor.open_trn(_abs(DVXI5_TRN)), OK, "the dvxi5 fixture terrain loads")
	workspace = MissionWorkspace.new(editor)
	assert_eq(int(workspace.new_current()), OK)
	controller = workspace._controller
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_abs(ITEMS_PATH)), OK)
	controller._placer.item_db = db
	shell = add_child_autofree(ShellStub.new())
	shell._workspaces = { 0: workspace }
	shell.editor_node = editor
	shell.root_dir = _abs(SAVE_DIR)
	DirAccess.make_dir_recursive_absolute(_abs(SAVE_DIR))
	service = add_child_autofree(EditorMcpService.new())
	service.setup(editor, shell)


func after_each() -> void:
	service.stop()
	McpLogHub.instance = null
	if not mount_root_dir.is_empty():
		_remove_dir_recursive(mount_root_dir)
		mount_root_dir = ""
	var dir := _abs(SAVE_DIR)
	if DirAccess.dir_exists_absolute(dir):
		var d := DirAccess.open(dir)
		if d != null:
			for f in d.get_files():
				DirAccess.remove_absolute(dir.path_join(f))
		DirAccess.remove_absolute(dir)


func _call(name: String, args := {}) -> McpToolResult:
	var ctx: McpToolContext = service._make_context(args)
	return await service.server.registry.call_tool(name, args, ctx)


func _ground(x: float, z: float) -> float:
	return editor.sample_height_world(x, z)


func _write_text_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "opened %s for writing" % path)
	file.store_string(text)
	file.close()


func _copy_fixture(src: String, dst: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(_abs(src))
	assert_gt(bytes.size(), 0, "fixture %s loaded" % src)
	var file := FileAccess.open(dst, FileAccess.WRITE)
	assert_not_null(file, "opened %s for writing" % dst)
	file.store_buffer(bytes)
	file.close()


func _remove_dir_recursive(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for child in dir.get_files():
		DirAccess.remove_absolute(path.path_join(child))
	for child_dir in dir.get_directories():
		_remove_dir_recursive(path.path_join(child_dir))
	DirAccess.remove_absolute(path)


func _seed_mount_resource_root() -> void:
	mount_root_dir = OS.get_cache_dir().path_join("opennova_mcp_mount_test_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(mount_root_dir), OK)
	_write_text_file(mount_root_dir.path_join("items.def"), """
begin "Debug Seat SUV"
  id 101294
  type vehicle
  graphic dsuv1
  sid dsuv1
  anim_def dsuv1
end

begin "Debug Soldier"
  id 102072
  type person
  graphic Fsldr03
  sid fsldr03
  anim_def E_STAND
end
""")
	_copy_fixture(DSUV1_MODEL, mount_root_dir.path_join("dsuv1.3di"))
	var resources := NovaResourceRoot.new()
	assert_eq(resources.set_root_dir(mount_root_dir), OK)
	shell.root = resources


func test_sample_terrain_matches_editor_surface() -> void:
	var result: McpToolResult = await _call("sample_terrain", { "points": [[64.0, -64.0], [99999.0, 99999.0]] })
	assert_false(result.is_error)
	var out: Dictionary = result.structured
	assert_almost_eq(float(out["heights"][0]), _ground(64.0, -64.0), 0.001, "on-mesh heights match the live surface")
	assert_null(out["heights"][1], "off-mesh points are null")
	assert_eq(int(out["off_terrain"]), 1)


func test_place_entities_grounds_like_the_editor() -> void:
	var result: McpToolResult = await _call("place_entities", { "rows": [
		{ "item_id": 102001, "x": 64.0, "z": -64.0, "yaw_deg": 90.0 },
		{ "item_id": 100001, "x": 72.0, "z": -72.0 },
		{ "item_id": 102001, "x": 99999.0, "z": 99999.0 },
	] })
	assert_false(result.is_error)
	var out: Dictionary = result.structured
	assert_eq(int(out["placed"]), 2)
	assert_eq(int(out["failed"]), 1)
	assert_almost_eq(float(out["rows"][0]["grounded_y"]), _ground(64.0, -64.0), 0.001,
		"the tool grounded at the sampled surface")
	assert_false(bool(out["rows"][2]["ok"]), "the off-terrain row failed individually")
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 1)
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_MARKER), 1)
	var record: Dictionary = controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)
	assert_almost_eq((record["rotation_deg"] as Vector3).y, 90.0, 0.5, "yaw persisted in the record")

	# The headline guarantee: a follow-up bulk re-ground finds NOTHING to move —
	# tool placements are grounded exactly as the editor grounds them.
	var reground: McpToolResult = await _call("reground_mission")
	assert_false(reground.is_error)
	assert_eq(int(reground.structured["moved"]), 0, "tool placements need no repair")


func test_place_entities_applies_ai_extras() -> void:
	var organics: McpToolResult = await _call("list_items", { "kind": NovaMissionData.KIND_ORGANIC, "limit": 1 })
	assert_false(organics.is_error)
	var items: Array = organics.structured["items"]
	if items.is_empty():
		pass_test("fixture items.def has no organics")
		return
	var organic_id := int(items[0]["id"])
	var result: McpToolResult = await _call("place_entities", { "rows": [
		{ "item_id": organic_id, "x": 80.0, "z": -80.0, "name1": "Eindo06", "team": 2, "waypoint_id": 3,
			"properties": { "alert_state": 1, "bogus_field": 9 } },
	] })
	assert_false(result.is_error)
	var out: Dictionary = result.structured
	assert_eq(int(out["placed"]), 1)
	assert_true(String(out["rows"][0].get("warning", "")).contains("bogus_field"),
		"unknown property keys are reported, not silently dropped")
	var record: Dictionary = controller.get_mission().get_entity(NovaMissionData.KIND_ORGANIC, 0)
	assert_eq(String(record["name1"]), "Eindo06")
	assert_eq(int(record["team"]), 2)
	assert_eq(int(record["waypoint_id"]), 3)
	assert_eq(int(record["alert_state"]), 1)


func test_get_mission_entities_lists_filters_and_details() -> void:
	await _call("place_entities", { "rows": [
		{ "item_id": 102001, "x": 64.0, "z": -64.0 },
		{ "item_id": 100001, "x": 72.0, "z": -72.0 },
	] })
	var all: McpToolResult = await _call("get_mission_entities")
	assert_eq(int(all.structured["total"]), 2)
	var buildings: McpToolResult = await _call("get_mission_entities", { "kind": NovaMissionData.KIND_BUILDING })
	assert_eq(int(buildings.structured["total"]), 1)
	assert_eq(String(buildings.structured["entities"][0]["kind_label"]), "building")
	assert_true(buildings.structured["entities"][0].has("world_position"), "records carry a world echo")
	var detail: McpToolResult = await _call("get_mission_entities",
			{ "detail": { "kind": NovaMissionData.KIND_BUILDING, "index": 0 } })
	assert_false(detail.is_error)
	assert_true(detail.structured.has("position"))
	var missing: McpToolResult = await _call("get_mission_entities", { "detail": { "kind": 2, "index": 99 } })
	assert_true(missing.is_error)


func test_edit_mission_entity_move_stays_grounded() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	var moved: McpToolResult = await _call("edit_mission_entity",
			{ "kind": NovaMissionData.KIND_BUILDING, "index": 0, "move": { "x": 96.0, "z": -96.0 } })
	assert_false(moved.is_error)
	var reground: McpToolResult = await _call("reground_mission")
	assert_eq(int(reground.structured["moved"]), 0, "a tool move is grounded; nothing to repair")
	var world: Array = moved.structured["entity"]["world_position"]
	assert_almost_eq(float(world[0]), 96.0, 0.5, "the entity sits at the requested world x")
	assert_almost_eq(float(world[2]), -96.0, 0.5, "and world z")


func test_edit_mission_entity_set_validates_and_applies() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	var bogus: McpToolResult = await _call("edit_mission_entity",
			{ "kind": NovaMissionData.KIND_BUILDING, "index": 0, "set": { "nonsense": 1 } })
	assert_true(bogus.is_error)
	assert_true(String(bogus.content[0]["text"]).contains("waypoint_id"), "the error lists the vocabulary")
	var ok: McpToolResult = await _call("edit_mission_entity",
			{ "kind": NovaMissionData.KIND_BUILDING, "index": 0, "set": { "team": 2, "group": 4 } })
	assert_false(ok.is_error)
	var record: Dictionary = controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)
	assert_eq(int(record["team"]), 2)
	var deleted: McpToolResult = await _call("edit_mission_entity",
			{ "kind": NovaMissionData.KIND_BUILDING, "index": 0, "delete": true })
	assert_false(deleted.is_error)
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 0)


func test_edit_mission_entity_requires_exactly_one_op() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	var both: McpToolResult = await _call("edit_mission_entity",
			{ "kind": 2, "index": 0, "move": { "x": 1, "z": 1 }, "delete": true })
	assert_true(both.is_error)
	var neither: McpToolResult = await _call("edit_mission_entity", { "kind": 2, "index": 0 })
	assert_true(neither.is_error)


func test_waypoint_path_lifecycle() -> void:
	var created: McpToolResult = await _call("edit_waypoint_path", { "op": "new_path" })
	assert_false(created.is_error)
	var path := int(created.structured["path"])
	var added: McpToolResult = await _call("edit_waypoint_path",
			{ "op": "add_markers", "points": [[64.0, -64.0], [80.0, -64.0], [80.0, -80.0]] })
	assert_false(added.is_error)
	assert_eq(int(added.structured["added"]), 3)
	assert_eq(int(added.structured["active"]["marker_count"]), 3)

	var no_loop: McpToolResult = await _call("edit_waypoint_path", { "op": "set_flags", "loop": false })
	assert_true(int(no_loop.structured["active"]["flags"]) & NovaMissionData.WP_FLAG_DOES_NOT_LOOP != 0,
		"loop=false sets the inverted on-disk bit")
	var loop: McpToolResult = await _call("edit_waypoint_path", { "op": "set_flags", "loop": true })
	assert_eq(int(loop.structured["active"]["flags"]) & NovaMissionData.WP_FLAG_DOES_NOT_LOOP, 0,
		"loop=true clears it (paths loop by default)")

	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 100.0, "z": -100.0 }] })
	var assigned: McpToolResult = await _call("edit_waypoint_path",
			{ "op": "assign_entity", "kind": NovaMissionData.KIND_BUILDING, "index": 0, "path": path })
	assert_false(assigned.is_error)
	assert_eq(int(controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)["waypoint_id"]), path)

	var removed: McpToolResult = await _call("edit_waypoint_path", { "op": "delete_marker", "marker_index": 1 })
	assert_false(removed.is_error)
	assert_eq(int(removed.structured["active"]["marker_count"]), 2)

	var listing: McpToolResult = await _call("edit_waypoint_path", { "op": "list" })
	assert_false(listing.is_error)
	assert_true(listing.structured["paths"] is Array)


func test_set_mission_header_validates_and_round_trips() -> void:
	var bogus: McpToolResult = await _call("set_mission_header", { "fields": { "nope": 1 } })
	assert_true(bogus.is_error)
	assert_true(String(bogus.content[0]["text"]).contains("mission_name"), "the error lists valid fields")
	var ok: McpToolResult = await _call("set_mission_header", { "fields": {
		"mission_name": "Tool Authored", "designer": "MCP", "start_time": 1200 } })
	assert_false(ok.is_error)
	var info: Dictionary = controller.get_mission().get_info()
	assert_eq(String(info["mission_name"]), "Tool Authored")
	assert_eq(String(info["designer"]), "MCP")


func test_reground_mission_repairs_raw_position_float() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	await _call("reground_mission")  # adopt the placement into the baseline
	var grounded_z := (controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)["position"] as Vector3).z
	# The recorded failure: a raw position write floats the entity 5m up.
	controller.select_object(NovaMissionData.KIND_BUILDING, 0)
	controller.set_selected_position(
			(controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)["position"] as Vector3) + Vector3(0, 0, 5.0))
	var repair: McpToolResult = await _call("reground_mission")
	assert_false(repair.is_error)
	assert_eq(int(repair.structured["moved"]), 1, "the floated entity is planted")
	assert_almost_eq((controller.get_mission().get_entity(NovaMissionData.KIND_BUILDING, 0)["position"] as Vector3).z,
			grounded_z, 0.01)


func test_save_mission_filename_and_current() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	var never_saved: McpToolResult = await _call("save_mission")
	assert_true(never_saved.is_error, "save without a path on a never-saved mission is an actionable error")
	var bad_ext: McpToolResult = await _call("save_mission", { "path": "thing.txt" })
	assert_true(bad_ext.is_error)
	var saved: McpToolResult = await _call("save_mission", { "path": "tool_authored.bms" })
	assert_false(saved.is_error)
	var path := String(saved.structured["path"])
	assert_true(FileAccess.file_exists(path))
	assert_true(path.begins_with(_abs(SAVE_DIR)), "relative filenames land in the mounted root")
	assert_false(bool(saved.structured["dirty"]))
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 96.0, "z": -96.0 }] })
	var resaved: McpToolResult = await _call("save_mission")
	assert_false(resaved.is_error, "save_current works once a path exists")


func test_undo_redo_tools_route_to_the_workspace() -> void:
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 1)
	var undone: McpToolResult = await _call("undo")
	assert_false(undone.is_error)
	assert_eq(int(undone.structured["performed"]), 1)
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 0)
	var redone: McpToolResult = await _call("redo")
	assert_eq(int(redone.structured["performed"]), 1)
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 1)


func test_analyze_mission_open_and_by_path() -> void:
	await _call("place_entities", { "rows": [
		{ "item_id": 102001, "x": 64.0, "z": -64.0 },
		{ "item_id": 100001, "x": 72.0, "z": -72.0 },
	] })
	var live: McpToolResult = await _call("analyze_mission")
	assert_false(live.is_error)
	var out: Dictionary = live.structured
	assert_eq(int(out["total_entities"]), 2)
	assert_eq(int(out["by_kind"]["building"]), 1)
	assert_gt((out["densest_cells_64m"] as Array).size(), 0)
	assert_ne(String(out["top_items"][0]["name"]), "?", "item names resolve through the palette")

	await _call("save_mission", { "path": "analyzed.bms" })
	var by_path: McpToolResult = await _call("analyze_mission", { "path": _abs(SAVE_DIR).path_join("analyzed.bms") })
	assert_false(by_path.is_error)
	assert_eq(int(by_path.structured["total_entities"]), 2, "read-only analysis re-parses from disk")


func test_analyze_mounts_reports_static_prediction_from_shared_runtime_rules() -> void:
	_seed_mount_resource_root()
	var mission: NovaMissionData = controller.get_mission()
	var vehicle: Dictionary = mission.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier: Dictionary = mission.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_true(mission.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(mission.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))

	var result: McpToolResult = await _call("analyze_mounts", { "include_live": false })
	assert_false(result.is_error, str(result.content))
	var out: Dictionary = result.structured
	assert_eq(String(out["runtime_parity"]["shared_rules"]), "MissionRuntime")
	assert_eq(String(out["runtime_parity"]["editor_mode"]), "static_analysis",
		"the editor predicts from authored data instead of running gameplay")
	assert_eq(String(out["runtime_parity"]["live_runtime"]), "GameWorld",
		"only the standalone GameWorld runs the live mission")
	var mounts: Array = out["mounts"]
	assert_eq(mounts.size(), 1)
	var row: Dictionary = mounts[0]
	assert_eq(int(row["command"]["id"]), 125)
	assert_eq(String(row["command"]["mode"]), "any_seat")
	assert_eq(int(row["target"]["bms_id"]), int(vehicle["bms_id"]))
	assert_gt(int(row["target"]["seat_count"]), 0, "target seats came from the real 3DI userpoints")
	assert_eq(String(row["prediction"]["seat"]["type_label"]), "controller",
		"command 125 can select ctrlx by original priority")
	assert_true(String(row["prediction"]["seat"]["source_name"]).to_lower().contains("ctrlx"))
	assert_eq(String(row["diagnostics"][0]), "static_only")

	var saved: McpToolResult = await _call("save_mission", { "path": "mounts_path_mode.bms" })
	assert_false(saved.is_error)
	var by_path: McpToolResult = await _call("analyze_mounts", {
		"path": _abs(SAVE_DIR).path_join("mounts_path_mode.bms"),
		"include_live": false,
	})
	assert_false(by_path.is_error)
	assert_eq(int(by_path.structured["total"]), 1, "path mode re-parses a BMS without opening it")




func test_set_camera_frame_point_and_entity() -> void:
	var framed: McpToolResult = await _call("set_camera", { "frame_point": { "x": 64.0, "z": -64.0, "radius": 40.0 } })
	assert_false(framed.is_error)
	assert_true(framed.structured.has("position"))
	await _call("place_entities", { "rows": [{ "item_id": 102001, "x": 64.0, "z": -64.0 }] })
	var entity: McpToolResult = await _call("set_camera", { "frame_entity": { "kind": NovaMissionData.KIND_BUILDING, "index": 0 } })
	assert_false(entity.is_error)
	var modeless: McpToolResult = await _call("set_camera", {})
	assert_true(modeless.is_error)
