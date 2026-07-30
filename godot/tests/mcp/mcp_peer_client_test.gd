extends GutTest

# The production loopback peer used by ONED's game proxy: initialization,
# structured/error/image result preservation, and endpoint validation.


func _server() -> McpServer:
	var server: McpServer = add_child_autofree(McpServer.new())
	assert_eq(server.start(0), OK)
	return server


func _client(server: McpServer) -> McpPeerClient:
	var client := McpPeerClient.new()
	var err: Error = await client.connect_to_url(server.get_url(), get_tree())
	assert_eq(err, OK, client.get_last_error())
	return client


func test_forwards_structured_result_without_flattening() -> void:
	var server := _server()
	server.registry.register(
		McpToolDef.make("echo", "test", {"value": {"type": "integer"}}),
		func(args, _ctx): return {"echo": int(args.get("value", 0))})
	var client := await _client(server)
	var result: McpToolResult = await client.call_tool("echo", {"value": 42})
	assert_false(result.is_error)
	assert_eq(int(result.structured.get("echo", -1)), 42)
	assert_eq(result.content[0]["type"], "text")


func test_forwards_error_and_image_content_blocks() -> void:
	var server := _server()
	server.registry.register(McpToolDef.make("failure", "test"),
		func(_args, _ctx): return McpToolResult.error("expected failure"))
	server.registry.register(McpToolDef.make("pixel", "test"),
		func(_args, _ctx):
			return McpToolResult.image(
					PackedByteArray([1, 2, 3]), "application/octet-stream"))
	var client := await _client(server)
	var failure: McpToolResult = await client.call_tool("failure")
	assert_true(failure.is_error)
	assert_eq(failure.content[0]["text"], "expected failure")
	var pixel: McpToolResult = await client.call_tool("pixel")
	assert_false(pixel.is_error)
	assert_eq(pixel.content[0]["type"], "image")
	assert_eq(pixel.content[0]["mimeType"], "application/octet-stream")
	assert_eq(pixel.content[0]["data"],
			Marshalls.raw_to_base64(PackedByteArray([1, 2, 3])))


func test_rejects_non_loopback_endpoint() -> void:
	var client := McpPeerClient.new()
	var err: Error = await client.connect_to_url(
			"http://example.com:8975/mcp", get_tree())
	assert_eq(err, ERR_INVALID_PARAMETER)
	assert_true(client.get_last_error().contains("Invalid runtime MCP URL"))


func test_no_wait_call_reaches_child_for_graceful_stop() -> void:
	var server := _server()
	var calls: Array = []
	server.registry.register(McpToolDef.make("quit_probe", "test"),
		func(args, _ctx):
			calls.append(args)
			return {"ok": true})
	var client := await _client(server)
	assert_eq(client.send_tool_no_wait("quit_probe", {"action": "quit"}), OK)
	for _i in 8:
		await get_tree().process_frame
	assert_eq(calls.size(), 1)
	assert_eq(calls[0].get("action"), "quit")
