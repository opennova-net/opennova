extends GutTest

# GameMcpService: the runtime MCP endpoint the game serves under --mcp-port.
# It binds loopback, keeps polling while a probe freezes the shell, and tears
# its server and log hub down with the node.

const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")


func after_each() -> void:
	McpLogHub.instance = null


func test_setup_binds_loopback_and_publishes_the_tool_catalog() -> void:
	var service: GameMcpService = add_child_autofree(GameMcpService.new())
	var game_adapter: GameMcpAdapter = add_child_autofree(GameMcpAdapter.new())
	assert_eq(service.setup(game_adapter, 0), OK)
	assert_true(service.is_running())
	assert_gt(service.server.get_port(), 0)
	assert_true(service.server.get_url().begins_with("http://127.0.0.1:"))
	assert_eq(service.process_mode, Node.PROCESS_MODE_ALWAYS,
			"a frozen shell must not stall the transport")
	assert_eq(McpLogHub.instance, service.log_hub)
	assert_true(service.probe_runner != null and service.probe_runner.get_parent() == service,
			"the probe runner rides the service (ADR 0041)")
	assert_eq(service.probe_runner.process_mode, Node.PROCESS_MODE_ALWAYS,
			"a frozen shell must not stall the probe watchdog")

	var client := McpTestClient.new()
	assert_true(await client.connect_to(get_tree(), service.server.get_port()))
	assert_true(await client.initialize(get_tree()) != null)
	var listed: Variant = await client.rpc(get_tree(), "tools/list")
	client.close()
	assert_true(listed is Dictionary)
	var names: Array = []
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		names.append(String(tool["name"]))
	assert_has(names, "game_state")
	assert_has(names, "game_control")
	assert_has(names, "game_capture_bundle")
	assert_has(names, "game_probe")


func test_setup_rejects_a_missing_adapter_and_bad_ports() -> void:
	var service: GameMcpService = add_child_autofree(GameMcpService.new())
	var game_adapter: GameMcpAdapter = add_child_autofree(GameMcpAdapter.new())
	assert_eq(service.setup(null, 0), ERR_INVALID_PARAMETER)
	assert_eq(service.setup(game_adapter, -1), ERR_INVALID_PARAMETER)
	assert_eq(service.setup(game_adapter, 70000), ERR_INVALID_PARAMETER)
	assert_false(service.is_running())


func test_leaving_the_tree_stops_the_server_and_releases_the_log_hub() -> void:
	var service := GameMcpService.new()
	add_child(service)
	var game_adapter: GameMcpAdapter = add_child_autofree(GameMcpAdapter.new())
	assert_eq(service.setup(game_adapter, 0), OK)
	var server := service.server
	remove_child(service)
	assert_false(server.is_running())
	assert_null(McpLogHub.instance)
	assert_true(is_instance_valid(game_adapter),
			"retiring the endpoint does not stop or free the game")
	service.free()
