extends GutTest

# McpToolRegistry: registration rules, tools/list shape and order, and the
# call_tool watchdog (sync handlers, coroutine handlers, wrapping, timeouts).


func _ctx() -> McpToolContext:
	return McpToolContext.new()


func _def(name: String, serial := true, timeout_ms := McpToolDef.DEFAULT_TIMEOUT_MS) -> McpToolDef:
	return McpToolDef.make(name, "test tool", {}, [], serial, timeout_ms)


func test_register_validates() -> void:
	var registry := McpToolRegistry.new()
	assert_eq(registry.register(_def(""), func(_a, _c): return 1), ERR_INVALID_PARAMETER, "Empty name rejected.")
	assert_eq(registry.register(_def("ok"), func(_a, _c): return 1), OK)
	assert_true(registry.has_tool("ok"))


func test_custom_cannot_shadow_builtin() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("core"), func(_a, _c): return 1, "builtin")
	assert_eq(registry.register(_def("core"), func(_a, _c): return 2, "custom"), ERR_ALREADY_EXISTS)
	assert_eq(registry.source_of("core"), "builtin")


func test_list_tools_builtins_first_in_registration_order() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("c_one"), func(_a, _c): return 0, "custom")
	registry.register(_def("b_one"), func(_a, _c): return 0, "builtin")
	registry.register(_def("b_two"), func(_a, _c): return 0, "builtin")
	var names := registry.list_tools().map(func(t): return t["name"])
	assert_eq(names, ["b_one", "b_two", "c_one"])


func test_list_tools_descriptor_shape() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("shaped"), func(_a, _c): return 0)
	var tool: Dictionary = registry.list_tools()[0]
	assert_eq(tool["name"], "shaped")
	assert_eq(tool["description"], "test tool")
	assert_eq(tool["inputSchema"], { "type": "object", "properties": {} })


func test_is_serial_default_and_override() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("locked"), func(_a, _c): return 0)
	registry.register(_def("free", false), func(_a, _c): return 0)
	assert_true(registry.is_serial("locked"))
	assert_false(registry.is_serial("free"))
	assert_true(registry.is_serial("unknown"), "Unknown names default to serialized.")


func test_call_unknown_tool_is_error_result() -> void:
	var registry := McpToolRegistry.new()
	var result: McpToolResult = await registry.call_tool("missing", {}, _ctx())
	assert_true(result.is_error)


func test_call_sync_handler_wraps_value() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("adder"), func(args, _c): return { "sum": int(args["a"]) + int(args["b"]) })
	var result: McpToolResult = await registry.call_tool("adder", { "a": 2, "b": 3 }, _ctx())
	assert_false(result.is_error)
	assert_eq(result.structured, { "sum": 5 }, "Plain returns are json-wrapped with structuredContent.")


func test_call_passes_through_tool_result() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("direct"), func(_a, _c): return McpToolResult.text("done"))
	var result: McpToolResult = await registry.call_tool("direct", {}, _ctx())
	assert_eq(result.content[0]["text"], "done")


func test_call_coroutine_handler() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("slow"), func(_a, ctx):
		await ctx.frames(2)
		return "after frames")
	var result: McpToolResult = await registry.call_tool("slow", {}, _ctx())
	assert_false(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("after frames"))


func test_call_timeout_returns_error_not_hang() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("stuck", true, 60), func(_a, ctx):
		await ctx.frames(100000)
		return "never")
	var ctx := _ctx()
	var result: McpToolResult = await registry.call_tool("stuck", {}, ctx)
	assert_true(result.is_error, "A runaway handler degrades to an error result.")
	assert_true(ctx.cancelled, "Timeout flags ctx.cancelled for cooperative aborts.")


func test_ctx_image_attachment_added() -> void:
	var registry := McpToolRegistry.new()
	registry.register(_def("snap"), func(_a, ctx):
		ctx.image(Image.create(2, 2, false, Image.FORMAT_RGBA8))
		return { "ok": true })
	var result: McpToolResult = await registry.call_tool("snap", {}, _ctx())
	var kinds := result.content.map(func(block): return block["type"])
	assert_true(kinds.has("image"), "ctx.image() attaches an image content block.")


func test_ctx_log_capture() -> void:
	var ctx := _ctx()
	ctx.log("first")
	ctx.log(42)
	assert_eq(ctx.logs, ["first", "42"])
