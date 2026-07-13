extends GutTest

# McpServer over a real loopback socket: handshake, protocol methods, tool
# dispatch (serialized FIFO + serial=false bypass), cancellation, and the
# rejection paths (origin, batch, method, path, malformed JSON).

const McpClient := preload("res://tests/mcp/mcp_test_client.gd")


func _server() -> McpServer:
	var server: McpServer = add_child_autofree(McpServer.new())
	server.instructions = "test instructions"
	assert_eq(server.start(0), OK, "Ephemeral port binds.")
	assert_gt(server.get_port(), 0)
	return server


func _client(port: int) -> RefCounted:
	var client := McpClient.new()
	var ok: bool = await client.connect_to(get_tree(), port)
	assert_true(ok, "Client connects to 127.0.0.1:%d." % port)
	return client


func _tool_def(name: String, serial := true) -> McpToolDef:
	return McpToolDef.make(name, "test", {}, [], serial)


# Coroutine helper so tests can run two tool calls concurrently.
func _call_into(client: RefCounted, name: String, state: Dictionary, key: String, args := {}) -> void:
	state[key] = await client.call_tool(get_tree(), name, args)


func _await_keys(state: Dictionary, keys: Array, max_frames := 600) -> void:
	for i in range(max_frames):
		var done := true
		for key in keys:
			if not state.has(key):
				done = false
		if done:
			return
		await get_tree().process_frame


func test_initialize_handshake() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.rpc(get_tree(), "initialize", {
		"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": { "name": "t", "version": "0" },
	})
	assert_eq(client.last_status, 200)
	assert_false(client.session_id().is_empty(), "Server issues Mcp-Session-Id.")
	var result: Dictionary = envelope["result"]
	assert_eq(result["protocolVersion"], "2025-03-26", "Known older version echoes.")
	assert_eq(result["serverInfo"]["name"], "oned")
	assert_eq(result["instructions"], "test instructions")
	assert_eq(result["capabilities"]["tools"]["listChanged"], false)


func test_initialized_notification_gets_202() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var body: Variant = await client.post_json(get_tree(), { "jsonrpc": "2.0", "method": "notifications/initialized" })
	assert_eq(client.last_status, 202)
	assert_null(body, "Notifications get an empty body.")


func test_ping_and_keep_alive_reuse() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var first: Variant = await client.rpc(get_tree(), "ping")
	assert_eq(first["result"], {})
	var second: Variant = await client.rpc(get_tree(), "ping")
	assert_eq(second["result"], {}, "Same connection serves a second request (keep-alive).")


func test_tools_list_shape() -> void:
	var server := _server()
	server.registry.register(_tool_def("echo"), func(args, _ctx): return { "echo": args })
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.rpc(get_tree(), "tools/list")
	var tools: Array = envelope["result"]["tools"]
	assert_eq(tools.size(), 1)
	assert_eq(tools[0]["name"], "echo")
	assert_eq(tools[0]["inputSchema"], { "type": "object", "properties": {} })


func test_tool_call_roundtrip() -> void:
	var server := _server()
	server.registry.register(_tool_def("echo"), func(args, _ctx): return { "echo": args })
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.call_tool(get_tree(), "echo", { "x": 7 })
	var payload: Dictionary = envelope["result"]
	assert_eq(payload["isError"], false)
	assert_true(String(payload["content"][0]["text"]).contains('"x": 7'))
	assert_eq(payload["structuredContent"]["echo"]["x"], 7.0)


func test_unknown_tool_is_invalid_params() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.call_tool(get_tree(), "nope")
	assert_eq(int(envelope["error"]["code"]), -32602)


func test_serial_tools_run_fifo() -> void:
	var server := _server()
	var order: Array = []
	server.registry.register(_tool_def("slow"), func(_args, ctx):
		await ctx.frames(10)
		order.append("slow")
		return "ok")
	server.registry.register(_tool_def("quick"), func(_args, _ctx):
		order.append("quick")
		return "ok")
	var a: RefCounted = await _client(server.get_port())
	var b: RefCounted = await _client(server.get_port())
	var state := {}
	_call_into(a, "slow", state, "a")
	await get_tree().process_frame
	await get_tree().process_frame
	_call_into(b, "quick", state, "b")
	await _await_keys(state, ["a", "b"])
	assert_eq(order, ["slow", "quick"], "Serialized FIFO: quick waits for slow.")
	assert_eq(state["a"]["result"]["isError"], false)
	assert_eq(state["b"]["result"]["isError"], false)


func test_non_serial_tool_bypasses_queue() -> void:
	var server := _server()
	var order: Array = []
	server.registry.register(_tool_def("slow"), func(_args, ctx):
		await ctx.frames(10)
		order.append("slow")
		return "ok")
	server.registry.register(_tool_def("monitor", false), func(_args, _ctx):
		order.append("monitor")
		return "ok")
	var a: RefCounted = await _client(server.get_port())
	var b: RefCounted = await _client(server.get_port())
	var state := {}
	_call_into(a, "slow", state, "a")
	await get_tree().process_frame
	await get_tree().process_frame
	_call_into(b, "monitor", state, "b")
	await _await_keys(state, ["a", "b"])
	assert_eq(order, ["monitor", "slow"], "serial=false runs alongside the queued job.")


func test_cancelled_queued_job_gets_error() -> void:
	var server := _server()
	server.registry.register(_tool_def("slow"), func(_args, ctx):
		await ctx.frames(10)
		return "ok")
	server.registry.register(_tool_def("quick"), func(_args, _ctx): return "ok")
	var a: RefCounted = await _client(server.get_port())
	var b: RefCounted = await _client(server.get_port())
	var c: RefCounted = await _client(server.get_port())
	var state := {}
	_call_into(a, "slow", state, "a")
	await get_tree().process_frame
	await get_tree().process_frame
	# B's call with a distinctive id, fired as a raw post so we control it.
	var post_b := func() -> void:
		state["b"] = await b.post_json(get_tree(), {
			"jsonrpc": "2.0", "id": 99, "method": "tools/call",
			"params": { "name": "quick", "arguments": {} },
		})
	post_b.call()
	await get_tree().process_frame
	await get_tree().process_frame
	await c.post_json(get_tree(), {
		"jsonrpc": "2.0", "method": "notifications/cancelled",
		"params": { "requestId": 99 },
	})
	await _await_keys(state, ["a", "b"])
	assert_eq(int(state["b"]["error"]["code"]), -32800, "Queued job cancelled before running.")
	assert_eq(state["a"]["result"]["isError"], false, "Running job unaffected.")


func test_non_local_origin_rejected_403() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	await client.post_json(get_tree(), { "jsonrpc": "2.0", "id": 1, "method": "ping" },
			{ "Origin": "http://evil.example" })
	assert_eq(client.last_status, 403)


func test_localhost_origin_allowed() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.post_json(get_tree(), { "jsonrpc": "2.0", "id": 1, "method": "ping" },
			{ "Origin": "http://localhost:5173" })
	assert_eq(client.last_status, 200)
	assert_eq(envelope["result"], {})


func test_batch_rejected() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var body: Variant = await client.post_json(get_tree(), [{ "jsonrpc": "2.0", "id": 1, "method": "ping" }])
	assert_eq(client.last_status, 400)
	assert_eq(int(body["error"]["code"]), -32600)


func test_malformed_json_body_rejected() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var ok: bool = await client.request(get_tree(), "POST", "/mcp",
			{ "Content-Type": "application/json" }, "{not json".to_utf8_buffer())
	assert_true(ok)
	assert_eq(client.last_status, 400)
	var body: Variant = JSON.parse_string(client.last_body.get_string_from_utf8())
	assert_eq(int(body["error"]["code"]), -32700)


func test_get_is_405_with_allow() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var ok: bool = await client.request(get_tree(), "GET", "/mcp")
	assert_true(ok)
	assert_eq(client.last_status, 405)
	assert_true(String(client.last_headers.get("allow", "")).contains("POST"))


func test_unknown_path_is_404() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	await client.request(get_tree(), "POST", "/other", {}, "{}".to_utf8_buffer())
	assert_eq(client.last_status, 404)


func test_delete_clears_session() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	await client.initialize(get_tree())
	var sid: String = client.session_id()
	assert_false(server.session(sid).is_empty(), "Session exists after initialize.")
	await client.request(get_tree(), "DELETE", "/mcp", { "Mcp-Session-Id": sid })
	assert_eq(client.last_status, 200)
	assert_true(server.session(sid).is_empty(), "DELETE drops the session.")


func test_unknown_method_is_32601() -> void:
	var server := _server()
	var client: RefCounted = await _client(server.get_port())
	var envelope: Variant = await client.rpc(get_tree(), "resources/list")
	assert_eq(int(envelope["error"]["code"]), -32601)


func test_stop_closes_listener() -> void:
	var server := _server()
	var port := server.get_port()
	server.stop()
	assert_false(server.is_running())
	var client := McpClient.new()
	var connected: bool = await client.connect_to(get_tree(), port)
	assert_false(connected, "No connections after stop().")
