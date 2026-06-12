extends GutTest

# EditorMcpService: registry assembly, headless auto-start suppression,
# explicit start over loopback (initialize carries instructions; tools/list
# carries the catalog), server.json lifecycle, and settings persistence.

const McpClient := preload("res://tests/mcp/mcp_test_client.gd")
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"

const BUILTIN_TOOLS := [
	"get_editor_state", "get_logs", "show_status_message", "describe_api",
	"list_assets", "read_file", "describe_asset", "open_in_workspace",
	"screenshot", "set_camera", "undo", "redo",
	"list_items", "sample_terrain", "place_entities", "get_mission_entities",
	"edit_mission_entity", "edit_waypoint_path", "set_mission_header",
	"reground_mission", "analyze_mission", "save_mission",
	"sim_control", "get_sim_state",
]

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var service: EditorMcpService


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	service = add_child_autofree(EditorMcpService.new())
	var editor_stub: Node = add_child_autofree(Node.new())
	editor_stub.name = "EditorStub"
	var shell_stub: Node = add_child_autofree(Node.new())
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


func test_builtins_registered() -> void:
	for name in BUILTIN_TOOLS:
		assert_true(service.server.registry.has_tool(name), "Registered: %s" % name)
		assert_eq(service.server.registry.source_of(name), "builtin")


func test_headless_autostart_suppressed() -> void:
	assert_false(service.is_running(), "Headless runs never open the socket implicitly.")
	assert_false(McpSettings.resolve_enabled())
	assert_true(McpSettings.get_enabled(), "...even though the persisted toggle defaults ON.")


func test_explicit_start_serves_initialize_and_tools() -> void:
	assert_eq(service.start(0), OK)
	assert_true(service.is_running())
	var client := McpClient.new()
	assert_true(await client.connect_to(get_tree(), service.server.get_port()))
	var envelope: Variant = await client.initialize(get_tree())
	assert_eq(envelope["result"]["serverInfo"]["name"], "oned")
	assert_true(String(envelope["result"]["instructions"]).contains("get_editor_state"),
			"initialize carries the agent instructions.")
	var listing: Variant = await client.rpc(get_tree(), "tools/list")
	var names: Array = listing["result"]["tools"].map(func(tool): return tool["name"])
	for name in BUILTIN_TOOLS:
		assert_true(names.has(name), "tools/list carries %s" % name)
	assert_eq(names[0], "get_editor_state", "State tool leads the catalog.")


func test_server_json_lifecycle() -> void:
	service.start(0)
	assert_true(FileAccess.file_exists(EditorMcpService.SERVER_JSON_PATH))
	var info: Variant = JSON.parse_string(FileAccess.get_file_as_string(EditorMcpService.SERVER_JSON_PATH))
	assert_eq(int(info["port"]), service.server.get_port())
	assert_true(String(info["url"]).begins_with("http://127.0.0.1:"))
	service.stop()
	assert_false(FileAccess.file_exists(EditorMcpService.SERVER_JSON_PATH), "Removed on stop.")


func test_status_text_states() -> void:
	assert_eq(service.get_status_text(), "Stopped")
	service.start(0)
	assert_true(service.get_status_text().begins_with("Running at http://127.0.0.1:"))


func test_set_enabled_persists_and_starts() -> void:
	McpSettings.set_port(39751)  # off the default so tests never fight a live ONED
	service.set_enabled(true)
	assert_true(McpSettings.get_enabled())
	# set_enabled(true) starts the server live even though resolve_enabled()
	# would say no in headless — the explicit toggle is the user's call.
	assert_true(service.is_running())
	service.set_enabled(false)
	assert_false(McpSettings.get_enabled())
	assert_false(service.is_running())


func test_port_settings_round_trip() -> void:
	McpSettings.set_port(9123)
	assert_eq(McpSettings.get_port(), 9123)
	McpSettings.set_port(80)
	assert_eq(McpSettings.get_port(), 1024, "Clamped to the unprivileged range.")
