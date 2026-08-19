extends GutTest

# The editor-wide tool handlers, exercised through the registry with stub
# editor/shell nodes: state shape, describe_api (classes, live objects,
# topics), discovery error paths, read_file windowing, describe_asset against
# repo fixtures, and the no-mission error paths of every mission tool (their
# happy paths live in editor_mcp_mission_tools_test.gd on a real terrain).

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var service: EditorMcpService


# The McpToolContext accessor contract (root/index/camera/active-workspace) is
# called directly on the shell now, so the stub carries the surface with empty
# answers instead of relying on probe fallbacks.
class ShellContractStub:
	extends Node

	func get_resource_root() -> ResourceRoot:
		return null

	func get_resource_index() -> Variant:
		return null

	func get_resource_root_dir() -> String:
		return ""

	func get_editor_camera() -> FlyCamera:
		return null

	func get_game_run_session() -> Variant:
		return null

	func _get_active_workspace() -> Variant:
		return null

	func _ensure_resource_index() -> void:
		pass


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	service = add_child_autofree(EditorMcpService.new())
	# A bare TerrainEditorBase serves the editor seam: every sampling default
	# answers "no terrain loaded".
	var editor_stub: TerrainEditorBase = add_child_autofree(TerrainEditorBase.new())
	editor_stub.name = "EditorStub"
	var shell_stub: ShellContractStub = add_child_autofree(ShellContractStub.new())
	shell_stub.name = "ShellStub"
	service.setup(editor_stub, shell_stub)


func after_each() -> void:
	service.stop()
	if _had_state_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_state_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	McpLogHub.instance = null


func _call(name: String, args := {}) -> McpToolResult:
	var ctx: McpToolContext = service._make_context(args)
	return await service.server.registry.call_tool(name, args, ctx)


func test_editor_state_shape_with_stub_shell() -> void:
	var result: McpToolResult = await _call("get_editor_state")
	assert_false(result.is_error)
	var state: Dictionary = result.structured
	assert_eq(state["app"]["name"], "ONED")
	assert_eq(state["app"]["version"], str(ProjectSettings.get_setting("application/config/version")))
	assert_false(bool(state["mission"]["loaded"]))
	assert_false(bool(state["camera"]["available"]))
	assert_eq(state["resource_root"]["mounted"], false)
	assert_true(state.has("log_cursor"))


func test_describe_api_index() -> void:
	var result: McpToolResult = await _call("describe_api")
	var index: Dictionary = result.structured
	assert_true((index["classes"] as Array).has("MissionData"), "GDExtension classes indexed.")
	assert_true((index["topics"] as Array).has("coordinates"))
	assert_false((index["topics"] as Array).has("ctx"), "the scripting topic is gone with the script surface")


func test_describe_api_native_class() -> void:
	var result: McpToolResult = await _call("describe_api", { "name": "MissionData" })
	var described: Dictionary = result.structured
	assert_eq(described["kind"], "native_class")
	var method_names: Array = described["methods"].map(func(m): return m["name"])
	assert_true(method_names.has("open_file"))
	assert_true(method_names.has("get_all_entities"))


func test_describe_api_topic_and_unknown() -> void:
	var topic: McpToolResult = await _call("describe_api", { "topic": "coordinates" })
	assert_true(String(topic.structured["markdown"]).contains("BMS"))
	var unknown: McpToolResult = await _call("describe_api", { "name": "NotAThing" })
	assert_true(unknown.is_error)


func test_list_assets_without_root_is_actionable() -> void:
	var result: McpToolResult = await _call("list_assets")
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("resource directory"))


func test_read_file_text_and_windowing() -> void:
	var path := ProjectSettings.globalize_path("user://mcp_read_probe.txt")
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string("0123456789")
	file.close()
	var result: McpToolResult = await _call("read_file", { "path": path, "offset": 2, "max_bytes": 4 })
	var out: Dictionary = result.structured
	assert_eq(out["text"], "2345")
	assert_eq(int(out["size_bytes"]), 10)
	assert_true(bool(out["truncated"]))
	var b64: McpToolResult = await _call("read_file", { "path": path, "as": "base64" })
	assert_eq(Marshalls.base64_to_raw(String(b64.structured["base64"])).get_string_from_utf8(), "0123456789")
	DirAccess.remove_absolute(path)


func test_read_file_unresolvable_is_actionable() -> void:
	var result: McpToolResult = await _call("read_file", { "path": "definitely_not_a_real_file.xyz" })
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("list_assets"))


func test_describe_asset_strings_fixture() -> void:
	var path := ProjectSettings.globalize_path("res://../fixtures/rtxt/menu.bin")
	var summary: McpToolResult = await _call("describe_asset", { "path": path })
	assert_false(summary.is_error)
	assert_eq(summary.structured["kind"], "strings")
	assert_gt(int(summary.structured["data"]["entry_count"]), 0)
	var full: McpToolResult = await _call("describe_asset", { "path": path, "depth": "full", "limit": 5 })
	var entries: Dictionary = full.structured["data"]["entries"]
	assert_eq(entries.size(), 5, "Full depth respects the limit.")


func test_describe_asset_3di_fixture() -> void:
	var path := ProjectSettings.globalize_path("res://").path_join("../fixtures/3dp/Bird1/Bird1.3di")
	if not FileAccess.file_exists(path):
		pass_test("3di fixture not present in this checkout")
		return
	var result: McpToolResult = await _call("describe_asset", { "path": path })
	assert_false(result.is_error)
	assert_eq(result.structured["kind"], "object_model")
	assert_false(String(result.structured["data"]["object_name"]).is_empty())
	var full: McpToolResult = await _call("describe_asset", {
		"path": path,
		"depth": "full",
		"limit": 2,
	})
	assert_false(full.is_error)
	var data: Dictionary = full.structured["data"]
	assert_true(data.has("materials"))
	assert_true(data.has("lights"))
	assert_lte((data["materials"] as Array).size(), 2,
			"deep 3DI material summaries honor the shared limit")
	assert_lte((data["lights"] as Array).size(), 2,
			"deep 3DI LGHT summaries honor the shared limit")
	if not (data["materials"] as Array).is_empty():
		var material: Dictionary = data["materials"][0]
		for key in [
			"index", "shader", "is_glass", "emissive_type", "textures",
			"technique", "shadow_inputs", "glass", "emissive",
		]:
			assert_true(material.has(key), "material summary carries %s" % key)
	if not (data["lights"] as Array).is_empty():
		var light: Dictionary = data["lights"][0]
		for key in ["index", "offset", "attenuation_end", "affects"]:
			assert_true(light.has(key), "LGHT summary carries %s" % key)
	assert_eq(int(data["material_count"]), int(data["material_total"]))
	assert_eq(int(data["light_count"]), int(data["light_total"]))


func test_describe_asset_fire_fixture_distinguishes_emissive_from_lght() -> void:
	var path := ProjectSettings.globalize_path("res://").path_join(
			"../fixtures/3dp/CmpFireN/CmpFireN.3di")
	if not FileAccess.file_exists(path):
		pass_test("CmpFireN 3di fixture not present in this checkout")
		return
	var result: McpToolResult = await _call("describe_asset", {"path": path})
	assert_false(result.is_error)
	var data: Dictionary = result.structured["data"]
	assert_gt(int(data["material_features"]["emissive"]), 0,
			"the camp-fire fixture exposes its emissive material classification")
	assert_eq(int(data["light_count"]), 0,
			"emissive fire art is not misreported as an authored LGHT record")


func test_describe_asset_lght_fixture_exposes_authored_light_fields() -> void:
	var path := ProjectSettings.globalize_path("res://").path_join(
			"../fixtures/3dp/DLabA1/DLabA1.3di")
	if not FileAccess.file_exists(path):
		pass_test("DLabA1 3di fixture not present in this checkout")
		return
	var result: McpToolResult = await _call("describe_asset", {
		"path": path,
		"depth": "full",
		"limit": 1,
	})
	assert_false(result.is_error)
	var data: Dictionary = result.structured["data"]
	assert_gt(int(data["light_count"]), 0,
			"the DLabA1 fixture pins a real authored LGHT record")
	assert_eq((data["lights"] as Array).size(), 1)
	var light: Dictionary = data["lights"][0]
	assert_true(light.has("color_start"))
	assert_true(light.has("color_end"))
	assert_true(light.has("affects"))


func test_script_surface_is_gone() -> void:
	for name in ["execute_script", "define_tool", "list_custom_tools", "delete_custom_tool"]:
		assert_false(service.server.registry.has_tool(name), "%s must not exist — the surface is curated tools only" % name)


func test_mission_tools_error_actionably_without_a_mission() -> void:
	# The stub shell has no mission workspace; every mission tool must point the
	# agent at open_in_workspace instead of failing obscurely.
	for name in ["list_items", "place_entities", "get_mission_entities", "edit_mission_entity",
			"edit_waypoint_path", "set_mission_header", "reground_mission", "save_mission",
			"analyze_mission"]:
		var result: McpToolResult = await _call(name, _minimal_args(name))
		assert_true(result.is_error, "%s without a mission is an error" % name)
		assert_true(String(result.content[0]["text"]).contains("open_in_workspace"),
				"%s names the fixing tool" % name)


func _minimal_args(name: String) -> Dictionary:
	match name:
		"place_entities":
			return { "rows": [{ "item_id": 1, "x": 0, "z": 0 }] }
		"edit_mission_entity":
			return { "kind": 2, "index": 0, "delete": true }
		"edit_waypoint_path":
			return { "op": "new_path" }
		"set_mission_header":
			return { "fields": { "mission_name": "x" } }
		"sample_terrain":
			return { "points": [[0, 0]] }
	return {}


func test_menu_tools_error_actionably_without_a_workspace() -> void:
	# The stub shell has no Menus workspace; every menu tool must point the
	# agent at open_in_workspace instead of failing obscurely.
	for name in ["get_menu", "menu_tabs", "edit_menu_screen", "add_menu_widgets",
			"edit_menu_widget", "set_widget_actions", "edit_widget_items",
			"preview_menu", "menu_screenshot", "save_menu"]:
		var result: McpToolResult = await _call(name, _minimal_menu_args(name))
		assert_true(result.is_error, "%s without the workspace is an error" % name)
	var analyzed: McpToolResult = await _call("analyze_menu", { "path": "no_such.mnu" })
	assert_true(analyzed.is_error, "analyze_menu with an unresolvable path errors")


func _minimal_menu_args(name: String) -> Dictionary:
	match name:
		"menu_tabs":
			return { "op": "new" }
		"edit_menu_screen":
			return { "add": { "name": "X" } }
		"add_menu_widgets":
			return { "rows": [{ "parent": 0, "type": "BUTTON", "rect": [0, 0, 1, 1] }] }
		"edit_menu_widget":
			return { "delete": 1 }
		"set_widget_actions":
			return { "id": 1, "actions": [] }
		"edit_widget_items":
			return { "id": 1, "op": "item_add", "row": {} }
		"preview_menu":
			return { "op": "status" }
	return {}


func test_game_state_without_managed_session_is_actionable() -> void:
	var result: McpToolResult = await _call("game_state")
	assert_false(result.is_error)
	assert_false(bool(result.structured["run"]["running"]))
	assert_eq(result.structured["run"]["state"], "unavailable")


func test_set_camera_without_camera_errors() -> void:
	var result: McpToolResult = await _call("set_camera", { "frame_point": { "x": 0, "z": 0 } })
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("open_in_workspace"))


func test_undo_unknown_workspace_errors() -> void:
	var result: McpToolResult = await _call("undo", { "workspace": "bogus" })
	assert_true(result.is_error)


func test_open_in_workspace_validates_workspace_id() -> void:
	var result: McpToolResult = await _call("open_in_workspace", { "workspace": "bogus", "path": "x.bms" })
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("mission"), "Error lists valid ids.")


func test_screenshot_headless_errors_gracefully() -> void:
	var result: McpToolResult = await _call("screenshot")
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("headless"))
