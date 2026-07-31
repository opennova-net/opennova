extends GutTest


func test_runtime_descriptor_is_confined_to_user_data() -> void:
	var valid := ProjectSettings.globalize_path(
			"user://oned-run-run-17.json")
	assert_true(GameMcpService.is_launch_descriptor_safe("run-17", valid))
	assert_false(GameMcpService.is_launch_descriptor_safe(
			"run-17",
			ProjectSettings.globalize_path("res://oned-run-run-17.json")))
	assert_false(GameMcpService.is_launch_descriptor_safe(
			"run-17",
			ProjectSettings.globalize_path("user://wrong-name.json")))


func test_runtime_descriptor_rejects_path_like_run_ids() -> void:
	var path := ProjectSettings.globalize_path(
			"user://oned-run-run-17.json")
	assert_false(GameMcpService.is_launch_descriptor_safe("../run-17", path))


func test_endpoint_shutdown_removes_handshake_without_stopping_the_game() -> void:
	var service: GameMcpService = add_child_autofree(GameMcpService.new())
	var game_adapter: GameMcpAdapter = add_child_autofree(GameMcpAdapter.new())
	var descriptor_path := ProjectSettings.globalize_path(
			"user://oned-run-shutdown-test.json")
	assert_eq(service.setup_from_metadata(game_adapter, {
		"run_id": "shutdown-test",
		"descriptor_path": descriptor_path,
		"log_path": "",
	}), OK)
	assert_true(FileAccess.file_exists(descriptor_path))

	service.request_endpoint_shutdown()
	await get_tree().process_frame

	assert_false(service.server.is_running())
	assert_false(FileAccess.file_exists(descriptor_path))
	assert_true(is_instance_valid(game_adapter),
			"retiring runtime MCP does not stop or free MainGame")
	assert_null(McpLogHub.instance)
