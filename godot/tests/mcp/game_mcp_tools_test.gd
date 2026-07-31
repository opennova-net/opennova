extends GutTest


class DebugSessionStub:
	extends NovaDebugSession

	var invoked_id: StringName
	var invoked_args: Variant
	var invoked_authority := false
	var set_id: StringName
	var set_value: Variant
	var set_authority := false
	var list_authority := false
	var read_authority := false
	var snapshot_authority := false
	var presentation_acquires := 0
	var presentation_releases := 0

	func list_controls(
			_page: StringName = &"",
			_filter: String = "",
			allow_authority: bool = false) -> Array[Dictionary]:
		list_authority = allow_authority
		return [{"id": "probe"}]

	func get_control_state(
			id: StringName,
			allow_authority: bool = false) -> NovaDebugControlState:
		read_authority = allow_authority
		var state := NovaDebugControlState.new()
		state.id = id
		state.available = true
		# Writable like a session whose F3 Live-edits latch is open, so the
		# adapter's per-caller gating is observable.
		state.writable = true
		return state

	func definition(id: StringName) -> NovaDebugControlDef:
		var definition := NovaDebugControlDef.action_control(
				id, &"Test", String(id), "", &"test", &"test")
		if id in [
			&"teleport_local_player",
			&"set_entity_health",
			&"set_entity_position",
			&"runtime_transport",
			&"set_mission_variable",
		]:
			definition.requires_unlock = true
			definition.authority = NovaDebugControlDef.Authority.HOST_ONLY
		return definition

	func set_control_value(
			id: StringName,
			value: Variant,
			authority: bool = false) -> Error:
		set_id = id
		set_value = value
		set_authority = authority
		return OK

	func invoke_control(
			id: StringName,
			args: Variant = null,
			authority: bool = false) -> Dictionary:
		invoked_id = id
		invoked_args = args
		invoked_authority = authority
		return {
			"error": OK,
			"result": "done",
			"state": get_control_state(id).to_json_value(),
		}

	func capture_snapshot(
			filter: String = "",
			allow_authority: bool = false) -> Dictionary:
		snapshot_authority = allow_authority
		return {"filter": filter, "controls": []}

	func acquire_presentation_source(_source: StringName) -> void:
		presentation_acquires += 1

	func release_presentation_source(_source: StringName) -> void:
		presentation_releases += 1


class AdapterStub:
	extends GameMcpAdapter

	var debug := DebugSessionStub.new()
	var last_action := ""
	var entity_page := {
		"total": 2,
		"offset": 0,
		"limit": 64,
		"returned": 2,
		"truncated": false,
		"entities": [{"index": 0}, {"index": 1}],
	}
	var entity_card := {"index": 1, "name": "Guard"}

	func get_debug_session() -> NovaDebugSession:
		return debug

	func get_mcp_game_state() -> Variant:
		return {"shell": {"state": "world"}}

	func get_mcp_game_entities(_offset: int, _limit: int) -> Variant:
		return entity_page

	func get_mcp_game_entity(_index: int) -> Variant:
		return entity_card

	func mcp_game_control(action: String) -> Error:
		last_action = action
		return OK


class RuntimeServiceStub:
	extends Node

	var shutdown_requested := false

	func request_endpoint_shutdown() -> void:
		shutdown_requested = true


var adapter: AdapterStub
var tools: GameMcpTools
var registry: McpToolRegistry
var ctx := McpToolContext.new()


func before_each() -> void:
	adapter = add_child_autofree(AdapterStub.new())
	tools = GameMcpTools.new(null, adapter)
	registry = McpToolRegistry.new()
	tools.register_all(registry)
	ctx = McpToolContext.new()


func _call(name: String, args: Dictionary = {}) -> McpToolResult:
	return await registry.call_tool(name, args, ctx)


func test_shared_catalog_registers_all_runtime_tools() -> void:
	for name in [
		"game_state",
		"game_entities",
		"game_control",
		"game_debug",
		"game_screenshot",
		"game_logs",
	]:
		assert_true(registry.has_tool(name), "registered %s" % name)


func test_entity_listing_and_inspection_route_through_public_adapter_seams() -> void:
	var result := await _call("game_entities", {
		"op": "list",
		"offset": 0,
		"limit": 64,
	})
	var page: Dictionary = result.structured
	assert_eq(page["entities"], [{"index": 0}, {"index": 1}])

	result = await _call("game_entities", {
		"op": "inspect",
		"index": 1,
	})
	var detail: Dictionary = result.structured
	assert_eq(detail["entity"]["name"], "Guard")

	var invalid := await _call("game_entities", {
		"op": "list",
		"offset": "0",
	})
	assert_true(invalid.is_error)


func test_game_control_routes_through_public_adapter_seam() -> void:
	var result := await _call("game_control", {"action": "pause"})
	assert_eq(adapter.last_action, "pause")
	assert_eq(result.structured["shell"]["state"], "world")


func test_internal_debug_shutdown_stops_endpoint_without_quitting_game() -> void:
	var runtime_service: RuntimeServiceStub = add_child_autofree(
			RuntimeServiceStub.new())
	var runtime_tools := GameMcpTools.new(runtime_service, adapter)
	var runtime_registry := McpToolRegistry.new()
	runtime_tools.register_all(runtime_registry)

	var result := await runtime_registry.call_tool("game_control", {
		"action": GameMcpTools.INTERNAL_SHUTDOWN_ACTION,
	}, ctx)

	assert_true(runtime_service.shutdown_requested)
	assert_eq(adapter.last_action, "",
			"the reserved endpoint control never reaches MainGame quit")
	assert_eq(result.structured["debug_endpoint"], "stopping")


func test_debug_set_forwards_per_call_authority_confirmation() -> void:
	var result := await _call("game_debug", {
		"op": "set",
		"id": "terrain_lod_quality",
		"value": 2.0,
		"confirm_authority": true,
	})
	assert_eq(adapter.debug.set_id, &"terrain_lod_quality")
	assert_eq(adapter.debug.set_value, 2.0)
	assert_true(adapter.debug.set_authority)
	assert_eq(result.structured["id"], "terrain_lod_quality")


func test_state_rows_reflect_the_callers_confirmed_authority() -> void:
	# A confirm_authority caller's rows must answer for THAT caller: reporting
	# writable=false / "Unlock edits" after its own write succeeded misleads
	# MCP consumers into thinking the mutation was rejected.
	await _call("game_debug", {
		"op": "get",
		"id": "terrain_lod_quality",
		"confirm_authority": true,
	})
	assert_true(adapter.debug.read_authority,
			"op=get threads the caller's confirmed authority into the row")

	adapter.debug.read_authority = false
	await _call("game_debug", {
		"op": "set",
		"id": "terrain_lod_quality",
		"value": 2.0,
		"confirm_authority": true,
	})
	assert_true(adapter.debug.read_authority,
			"the row returned after a confirmed set reflects that authority")

	await _call("game_debug", {"op": "list", "confirm_authority": true})
	assert_true(adapter.debug.list_authority)
	await _call("game_debug", {"op": "snapshot", "confirm_authority": true})
	assert_true(adapter.debug.snapshot_authority)

	adapter.debug.read_authority = true
	await _call("game_debug", {"op": "get", "id": "terrain_lod_quality"})
	assert_false(adapter.debug.read_authority,
			"an unconfirmed caller still sees the locked F3 policy view")


func test_unconfirmed_rows_for_gated_controls_mirror_the_write_precheck() -> void:
	# The session can report writable while F3's Live-edits latch is open, but
	# an unconfirmed automation caller's set/invoke is refused by the
	# confirm_authority precheck — its rows must tell the same story.
	var result := await _call("game_debug", {
		"op": "get",
		"id": "teleport_local_player",
	})
	assert_false(bool(result.structured["writable"]),
			"an unconfirmed caller cannot be shown a write it will be refused")
	assert_true(String(result.structured["reason"]).contains("confirm_authority"))

	result = await _call("game_debug", {
		"op": "get",
		"id": "teleport_local_player",
		"confirm_authority": true,
	})
	assert_true(bool(result.structured["writable"]),
			"a confirmed caller keeps the session's writable view")

	result = await _call("game_debug", {"op": "get", "id": "plain_control"})
	assert_true(bool(result.structured["writable"]),
			"ungated controls stay writable for unconfirmed callers")


func test_authority_confirmation_requires_a_json_boolean() -> void:
	await _call("game_debug", {
		"op": "set",
		"id": "terrain_lod_quality",
		"value": 2.0,
		"confirm_authority": "true",
	})
	assert_false(adapter.debug.set_authority,
			"a truthy string cannot opt into authoritative mutation")


func test_debug_actions_decode_json_arguments_for_public_engine_methods() -> void:
	var result := await _call("game_debug", {
		"op": "invoke",
		"id": "teleport_local_player",
		"args": {
			"position": [12.0, 34.0, 56.0],
			"yaw_deg": 90.0,
			"pitch_deg": -10.0,
		},
		"confirm_authority": true,
	})
	assert_eq(result.structured["error"], OK)
	assert_eq(adapter.debug.invoked_id, &"teleport_local_player")
	assert_eq(adapter.debug.invoked_args, [Vector3(12.0, 34.0, 56.0), 90.0, -10.0])
	assert_true(adapter.debug.invoked_authority)

	await _call("game_debug", {
		"op": "invoke",
		"id": "set_entity_health",
		"args": {"entity": 7, "health": 25},
		"confirm_authority": true,
	})
	assert_eq(adapter.debug.invoked_args, [7, 25])

	await _call("game_debug", {
		"op": "invoke",
		"id": "runtime_transport",
		"args": {"action": "pause"},
		"confirm_authority": true,
	})
	assert_eq(adapter.debug.invoked_args, "pause")

	await _call("game_debug", {
		"op": "invoke",
		"id": "set_mission_variable",
		"args": {"index": 17, "value": -3},
		"confirm_authority": true,
	})
	assert_eq(adapter.debug.invoked_args, [17, -3])

	await _call("game_debug", {
		"op": "invoke",
		"id": "set_audio_bus_volume",
		"args": {"bus": "SFX", "volume_db": -12.5},
	})
	assert_eq(adapter.debug.invoked_args, ["SFX", -12.5])

	await _call("game_debug", {
		"op": "invoke",
		"id": "set_audio_bus_mute",
		"args": {"bus": "SFX", "muted": true},
	})
	assert_eq(adapter.debug.invoked_args, ["SFX", true])


func test_f3_unlock_cannot_substitute_for_mcp_per_call_confirmation() -> void:
	# The stub deliberately accepts any write, like a shared session whose F3
	# edit latch is already open. The MCP adapter must reject before reaching it.
	adapter.debug.invoked_id = &""
	var result := await _call("game_debug", {
		"op": "invoke",
		"id": "set_entity_health",
		"args": {"entity": 0, "health": 25},
	})
	assert_true(result.is_error)
	assert_eq(adapter.debug.invoked_id, &"")
	assert_true(String(result.content[0]["text"]).contains(
			"confirm_authority"))


func test_debug_actions_reject_coercible_or_nonfinite_numeric_input() -> void:
	var cases: Array[Dictionary] = [
		{
			"id": "set_entity_health",
			"args": {"entity": "banana", "health": 25},
		},
		{
			"id": "set_entity_health",
			"args": {"entity": 7, "health": "banana"},
		},
		{
			"id": "set_entity_position",
			"args": {"entity": 2, "position": ["12", 34.0, 56.0]},
		},
		{
			"id": "teleport_local_player",
			"args": {"position": [12.0, 34.0, INF]},
		},
		{
			"id": "set_mission_variable",
			"args": {"index": 17.5, "value": -3},
		},
		{
			"id": "set_audio_bus_volume",
			"args": {"bus": "SFX", "volume_db": "quiet"},
		},
		{
			"id": "set_audio_bus_mute",
			"args": {"bus": "SFX", "muted": 1},
		},
	]
	for row in cases:
		adapter.debug.invoked_id = &""
		var result := await _call("game_debug", {
			"op": "invoke",
			"id": row["id"],
			"args": row["args"],
			"confirm_authority": true,
		})
		assert_true(result.is_error,
				"Bad numeric input errors for %s" % row["id"])
		assert_eq(adapter.debug.invoked_id, &"",
				"Bad numeric input never reaches %s" % row["id"])


func test_debug_action_validation_is_actionable() -> void:
	var result := await _call("game_debug", {
		"op": "invoke",
		"id": "set_entity_position",
		"args": {"entity": 2},
		"confirm_authority": true,
	})
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("position"))


func test_snapshot_filter_reaches_shared_session() -> void:
	var result := await _call("game_debug", {
		"op": "snapshot",
		"filter": "terrain",
	})
	assert_eq(result.structured["filter"], "terrain")


func test_cancelled_screenshot_releases_its_presentation_lease() -> void:
	ctx.cancelled = true
	var result := await _call("game_screenshot")
	assert_true(result.is_error)
	assert_eq(adapter.debug.presentation_acquires, 1)
	assert_eq(adapter.debug.presentation_releases, 1,
			"cooperative cancellation drops expensive debug views immediately")


func test_wrong_typed_screenshot_args_error_before_the_lease_is_acquired() -> void:
	# int()/float() on a non-numeric Variant is a script error; raised after
	# acquire it would abort the handler and leak the presentation lease.
	for args in [
		{"max_dim": [1280]},
		{"max_dim": "wide"},
		{"quality": {"value": 0.8}},
		{"format": 3},
	]:
		var result := await _call("game_screenshot", args)
		assert_true(result.is_error, "wrong-typed %s errors" % [args])
	assert_eq(adapter.debug.presentation_acquires, 0,
			"invalid args never touch the presentation lease")
	assert_eq(adapter.debug.presentation_releases, 0)
