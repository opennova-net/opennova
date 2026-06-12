extends GutTest

# McpDynamicTools: define/list/delete, compile-first persistence, name and
# collision rules, disk round-trip via load_all, and escaping of metadata.


var registry: McpToolRegistry
var dynamic: McpDynamicTools


func before_each() -> void:
	McpLogHub.instance = McpLogHub.new()
	registry = McpToolRegistry.new()
	dynamic = McpDynamicTools.new(registry)
	_clear_tools_dir()


func after_each() -> void:
	McpLogHub.instance = null
	_clear_tools_dir()


func _clear_tools_dir() -> void:
	var dir := ProjectSettings.globalize_path(McpDynamicTools.TOOLS_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		return
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))


func _ctx() -> McpToolContext:
	return McpToolContext.new()


func test_define_persists_registers_and_runs() -> void:
	var outcome := dynamic.define("double_it", "Doubles args.n", { "type": "object" }, "return args[\"n\"] * 2")
	assert_true(outcome["ok"], str(outcome.get("errors", [])))
	assert_true(FileAccess.file_exists(String(outcome["path"])), "Tool file persisted.")
	assert_true(registry.has_tool("double_it"))
	assert_eq(registry.source_of("double_it"), "custom")
	var result: McpToolResult = await registry.call_tool("double_it", { "n": 21 }, _ctx())
	assert_false(result.is_error)
	assert_eq(result.structured["result"], 42)


func test_defined_tool_appears_in_tools_list() -> void:
	dynamic.define("my_probe", "A probe.", {}, "return 1")
	var names := registry.list_tools().map(func(tool): return tool["name"])
	assert_true(names.has("my_probe"))


func test_duplicate_requires_overwrite() -> void:
	dynamic.define("dupe", "First.", {}, "return 1")
	var rejected := dynamic.define("dupe", "Second.", {}, "return 2")
	assert_false(rejected["ok"])
	var replaced := dynamic.define("dupe", "Second.", {}, "return 2", true)
	assert_true(replaced["ok"])
	var result: McpToolResult = await registry.call_tool("dupe", {}, _ctx())
	assert_eq(result.structured["result"], 2, "Overwrite replaced the behavior.")


func test_builtin_name_collision_rejected() -> void:
	registry.register({ "name": "core_tool", "description": "x", "input_schema": {} }, func(_a, _c): return 0, "builtin")
	var outcome := dynamic.define("core_tool", "Shadow attempt.", {}, "return 1")
	assert_false(outcome["ok"])
	assert_true(String(outcome["errors"][0]).contains("built-in"))


func test_bad_names_rejected() -> void:
	for bad in ["Bad", "x", "has space", "has-dash", "1leading", "x".repeat(49)]:
		var outcome := dynamic.define(bad, "d", {}, "return 1")
		assert_false(outcome["ok"], "Rejected: %s" % bad)


func test_compile_error_persists_nothing() -> void:
	var outcome := dynamic.define("broken_tool", "Won't compile.", {}, "func broken(:")
	assert_false(outcome["ok"])
	assert_gt((outcome["errors"] as Array).size(), 0)
	assert_false(registry.has_tool("broken_tool"))
	var dir := ProjectSettings.globalize_path(McpDynamicTools.TOOLS_DIR)
	var persisted := DirAccess.dir_exists_absolute(dir) and DirAccess.get_files_at(dir).has("broken_tool.gd")
	assert_false(persisted, "Nothing written on compile failure.")
	assert_engine_error("Parse Error")


func test_delete_removes_file_and_registration() -> void:
	var outcome := dynamic.define("doomed", "Bye.", {}, "return 1")
	assert_eq(dynamic.delete("doomed"), OK)
	assert_false(registry.has_tool("doomed"))
	assert_false(FileAccess.file_exists(String(outcome["path"])))
	assert_eq(dynamic.delete("doomed"), ERR_DOES_NOT_EXIST)


func test_load_all_round_trips_from_disk() -> void:
	dynamic.define("keeper", "Survives restarts.", { "type": "object", "properties": { "n": { "type": "integer" } } },
			"return args.get(\"n\", 0) + 100")
	# Fresh registry + loader simulates an editor restart.
	var registry2 := McpToolRegistry.new()
	var dynamic2 := McpDynamicTools.new(registry2)
	var report := dynamic2.load_all()
	assert_eq(Array(report["loaded"]), ["keeper"])
	assert_eq((report["failed"] as Array).size(), 0)
	var listed: Array = dynamic2.list()
	assert_eq(listed[0]["name"], "keeper")
	assert_eq(listed[0]["description"], "Survives restarts.")
	var tools := registry2.list_tools()
	assert_eq(tools[0]["inputSchema"]["properties"]["n"]["type"], "integer", "Schema round-trips.")
	var result: McpToolResult = await registry2.call_tool("keeper", { "n": 1 }, _ctx())
	assert_eq(result.structured["result"], 101)


func test_load_all_reports_corrupt_file_and_continues() -> void:
	dynamic.define("healthy", "Fine.", {}, "return 1")
	var dir := ProjectSettings.globalize_path(McpDynamicTools.TOOLS_DIR)
	var corrupt := FileAccess.open(dir.path_join("corrupt.gd"), FileAccess.WRITE)
	corrupt.store_string("this is not gdscript ((")
	corrupt.close()
	var registry2 := McpToolRegistry.new()
	var report := McpDynamicTools.new(registry2).load_all()
	assert_eq(Array(report["loaded"]), ["healthy"])
	assert_eq((report["failed"] as Array).size(), 1)
	assert_eq(String(report["failed"][0]["file"]), "corrupt.gd")
	assert_engine_error("Parse Error")


func test_metadata_escaping_round_trips() -> void:
	var tricky := "Quotes \" and \\\"escapes\\\", newlines\nand\ttabs — even \"\"\" triples."
	var outcome := dynamic.define("tricky_meta", tricky, { "type": "object", "properties": { "s": { "type": "string", "description": "a \"quoted\" thing" } } }, "return true")
	assert_true(outcome["ok"], str(outcome.get("errors", [])))
	var registry2 := McpToolRegistry.new()
	var dynamic2 := McpDynamicTools.new(registry2)
	dynamic2.load_all()
	assert_eq(dynamic2.list()[0]["description"], tricky, "Any description content survives the file round-trip.")


func test_calls_counter_increments() -> void:
	dynamic.define("counted", "Counts.", {}, "return 1")
	await registry.call_tool("counted", {}, _ctx())
	await registry.call_tool("counted", {}, _ctx())
	assert_eq(dynamic.list()[0]["calls"], 2)
