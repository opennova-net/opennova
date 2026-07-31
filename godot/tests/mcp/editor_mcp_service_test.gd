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
	"analyze_mounts",
	"get_menu", "menu_tabs", "edit_menu_screen", "add_menu_widgets",
	"edit_menu_widget", "set_widget_actions", "edit_widget_items",
	"preview_menu", "menu_screenshot", "analyze_menu", "save_menu",
	"run_game", "game_state", "game_entities", "game_control", "game_debug",
	"game_screenshot", "game_logs",
]


class SessionGateStub:
	extends ShellGameSession

	var debug_enabled := false
	var hooks_bound := false

	func set_runtime_debug_enabled(value: bool) -> void:
		debug_enabled = value

	func set_runtime_control_hooks(
			forwarder: Callable,
			quit_requester: Callable = Callable()) -> void:
		hooks_bound = forwarder.is_valid() or quit_requester.is_valid()

	func get_state() -> Dictionary:
		return {"state": "stopped", "running": false}


class ShellStub:
	extends Node

	var session := SessionGateStub.new()

	func get_game_run_session() -> Variant:
		return session


class DisableFailureGameTools:
	extends EditorMcpGameTools

	func _init() -> void:
		super(null)

	func disable_runtime_debug() -> Dictionary:
		return {
			"error": ERR_UNAVAILABLE,
			"fallback_stopped": false,
			"message": "Agent server stopped, but the runtime debug endpoint could not be retired.",
		}


class DelayedDisableGameTools:
	extends EditorMcpGameTools

	signal release_disable

	var disable_started := false
	var delay_next := true
	var enable_calls := 0
	var needs_relaunch := false

	func _init() -> void:
		super(null)

	func enable_runtime_debug() -> void:
		enable_calls += 1

	func current_runtime_needs_relaunch() -> bool:
		return needs_relaunch

	func disable_runtime_debug() -> Dictionary:
		disable_started = true
		if delay_next:
			delay_next = false
			await release_disable
		return {
			"error": ERR_UNAVAILABLE,
			"fallback_stopped": false,
			"message": "stale disable result",
		}


var _saved_state_config := PackedByteArray()
var _had_state_config := false
var service: EditorMcpService
var shell_stub: ShellStub


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	service = add_child_autofree(EditorMcpService.new())
	var editor_stub: Node = add_child_autofree(Node.new())
	editor_stub.name = "EditorStub"
	shell_stub = add_child_autofree(ShellStub.new())
	shell_stub.name = "ShellStub"
	service.setup(editor_stub, shell_stub, shell_stub.session)


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
	for name in [
		"game_state",
		"game_entities",
		"game_control",
		"game_debug",
		"game_screenshot",
		"game_logs",
	]:
		assert_true(service.server.registry.is_serial(name),
				"the one runtime peer connection is protected by ONED's FIFO")


func test_headless_autostart_suppressed() -> void:
	assert_false(service.is_running(), "Headless runs never open the socket implicitly.")
	assert_false(McpSettings.resolve_enabled())
	assert_true(McpSettings.get_enabled(), "...even though the persisted toggle defaults ON.")
	assert_false(shell_stub.session.debug_enabled,
			"setup does not authorize runtime debug before the Agent server starts")
	assert_false(shell_stub.session.hooks_bound,
			"setup does not bind runtime hooks while the Agent server is disabled")


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
	assert_true(shell_stub.session.debug_enabled)
	assert_true(shell_stub.session.hooks_bound)
	service.set_enabled(false)
	await get_tree().process_frame
	assert_false(McpSettings.get_enabled())
	assert_false(service.is_running())
	assert_false(shell_stub.session.debug_enabled)
	assert_false(shell_stub.session.hooks_bound)


func test_runtime_disable_failure_is_visible_in_settings_status() -> void:
	service.game_tools = DisableFailureGameTools.new()

	service.stop()
	await get_tree().process_frame

	assert_string_contains(service.get_status_text(),
			"runtime debug endpoint could not be retired")


func test_reenable_ignores_stale_disable_completion() -> void:
	var transition := DelayedDisableGameTools.new()
	service.game_tools = transition

	service.stop()
	await get_tree().process_frame
	assert_true(transition.disable_started)
	assert_string_contains(service.get_status_text(), "Stopping Agent server")

	assert_eq(service.start(0), OK)
	transition.release_disable.emit()
	await get_tree().process_frame
	await get_tree().process_frame

	assert_true(service.is_running())
	assert_eq(transition.enable_calls, 1)
	assert_true(service.get_status_text().begins_with("Running at "))
	assert_false(service.get_status_text().contains("stale disable result"))


func test_running_status_discloses_current_game_relaunch_requirement() -> void:
	var transition := DelayedDisableGameTools.new()
	transition.needs_relaunch = true
	service.game_tools = transition

	assert_eq(service.start(0), OK)

	assert_string_contains(service.get_status_text(), "Running at ")
	assert_string_contains(service.get_status_text(), "relaunch the current game")


func test_port_settings_round_trip() -> void:
	McpSettings.set_port(9123)
	assert_eq(McpSettings.get_port(), 9123)
	McpSettings.set_port(80)
	assert_eq(McpSettings.get_port(), 1024, "Clamped to the unprivileged range.")
