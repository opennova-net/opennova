extends GutTest

# The built-in tool handlers, exercised through the registry with stub
# editor/shell nodes: state shape, describe_api (classes, live objects,
# topics), discovery error paths, read_file windowing, describe_asset against
# repo fixtures, execute_script through the full service path, and the
# define/list/delete custom-tool flow.

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var service: EditorMcpService


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	service = add_child_autofree(EditorMcpService.new())
	var editor_stub: Node = add_child_autofree(Node.new())
	editor_stub.name = "EditorStub"
	var shell_stub: Node = add_child_autofree(Node.new())
	shell_stub.name = "ShellStub"
	service.setup(editor_stub, shell_stub)
	_clear_tools_dir()


func after_each() -> void:
	service.stop()
	_clear_tools_dir()
	if _had_state_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_state_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	McpLogHub.instance = null


func _clear_tools_dir() -> void:
	var dir := ProjectSettings.globalize_path(McpDynamicTools.TOOLS_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		return
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))


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
	assert_true((index["classes"] as Array).has("NovaMissionData"), "GDExtension classes indexed.")
	assert_true((index["topics"] as Array).has("ctx"))


func test_describe_api_native_class() -> void:
	var result: McpToolResult = await _call("describe_api", { "name": "NovaMissionData" })
	var described: Dictionary = result.structured
	assert_eq(described["kind"], "native_class")
	var method_names: Array = described["methods"].map(func(m): return m["name"])
	assert_true(method_names.has("open_file"))
	assert_true(method_names.has("get_all_entities"))


func test_describe_api_topic_and_unknown() -> void:
	var topic: McpToolResult = await _call("describe_api", { "topic": "ctx" })
	assert_true(String(topic.structured["markdown"]).contains("ctx.shell"))
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
	var path := ProjectSettings.globalize_path("res://fixtures/strings/menu.bin")
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


func test_execute_script_through_service() -> void:
	var result: McpToolResult = await _call("execute_script", { "code": "ctx.log(\"probing\")\nreturn ctx.editor.name" })
	assert_false(result.is_error)
	assert_eq(result.structured["result"], "EditorStub")
	assert_eq(result.structured["logs"], ["probing"])


func test_execute_script_logs_reach_hub() -> void:
	await _call("execute_script", { "code": "ctx.log(\"hub bound?\")\nreturn 1" })
	var page := service.log_hub.get_entries(0, 50, PackedStringArray(["script"]))
	assert_eq(page["entries"].size(), 1)
	assert_eq(String(page["entries"][0]["text"]), "hub bound?")


func test_define_then_call_then_delete_custom_tool() -> void:
	var defined: McpToolResult = await _call("define_tool", {
		"name": "stub_probe",
		"description": "Returns the stub editor's name.",
		"code": "return ctx.editor.name",
	})
	assert_false(defined.is_error)
	var called: McpToolResult = await _call("stub_probe")
	assert_eq(called.structured["result"], "EditorStub")
	var listed: McpToolResult = await _call("list_custom_tools")
	assert_eq(listed.structured["tools"][0]["name"], "stub_probe")
	var deleted: McpToolResult = await _call("delete_custom_tool", { "name": "stub_probe" })
	assert_false(deleted.is_error)
	assert_false(service.server.registry.has_tool("stub_probe"))


func test_open_in_workspace_validates_workspace_id() -> void:
	var result: McpToolResult = await _call("open_in_workspace", { "workspace": "bogus", "path": "x.bms" })
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("mission"), "Error lists valid ids.")


func test_screenshot_headless_errors_gracefully() -> void:
	var result: McpToolResult = await _call("screenshot")
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("headless"))
