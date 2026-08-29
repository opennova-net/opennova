extends GutTest


class DebugControlsStub:
	extends DebugControls

	var invoked_id: StringName
	var invoked_args: Variant
	var invoked_authority := false
	var set_id: StringName
	var set_value: Variant
	var set_authority := false
	var list_authority := false
	var read_authority := false
	var snapshot_authority := false

	func list_controls(
			_page: StringName = &"",
			_filter: String = "",
			allow_authority: bool = false) -> Array[Dictionary]:
		list_authority = allow_authority
		return [{"id": "probe"}]

	func get_control_state(
			id: StringName,
			allow_authority: bool = false) -> DebugControlState:
		read_authority = allow_authority
		var state := DebugControlState.new()
		state.id = id
		state.available = true
		# Writable no matter the caller, so the transport's own per-caller
		# refusals stay observable against an always-accepting table.
		state.writable = true
		return state

	func control(id: StringName) -> DebugControls.Row:
		var row := DebugControls.Row.new()
		row.id = id
		row.page = &"Test"
		row.label = String(id)
		row.kind = DebugControls.Kind.ACTION
		if id in [
			&"teleport_local_player",
			&"set_entity_health",
			&"set_entity_position",
			&"runtime_transport",
			&"set_mission_variable",
		]:
			row.requires_confirm = true
			row.authority = DebugControls.Authority.HOST_ONLY
		return row

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


class AdapterStub:
	extends GameMcpAdapter

	var debug := DebugControlsStub.new()
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
	var capture_args := {}
	var render_diagnostics := {
		"schema": "OpenNovaRenderDiagnosticsV1",
		"camera": {"available": true, "fov_deg": 50.534},
		"passes": {"root": {"shadow_draw_calls": 17}},
	}

	func get_debug_controls() -> DebugControls:
		return debug

	func get_mcp_game_state() -> Variant:
		return {"shell": {"state": "world"}}

	func get_mcp_game_entities(_offset: int, _limit: int) -> Variant:
		return entity_page

	func get_mcp_game_entity(_index: int) -> Variant:
		return entity_card

	func get_mcp_render_diagnostics() -> Variant:
		return render_diagnostics

	func capture_mcp_render_bundle(
			args: Dictionary,
			_cancel_requested: Callable = Callable()) -> Variant:
		capture_args = args.duplicate(true)
		return {
			"schema": "OpenNovaRenderCaptureV1",
			"capture_id": "probe-17",
			"artifact": {
				"png_path": "C:/capture/probe-17.png",
				"state_path": "C:/capture/probe-17.json",
				"width": 1600,
				"height": 900,
				"sha256": "abc123",
			},
			"diagnostics": render_diagnostics,
			"image_bytes": PackedByteArray([0x89, 0x50, 0x4e, 0x47]),
		}

	func mcp_game_control(action: String) -> Error:
		last_action = action
		return OK


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
	# The catalog IS the list: a definition dropped from register_all (or a
	# registration whose handler stopped resolving) fails here, game_menu and
	# game_probe included.
	var definitions: Array[McpToolDef] = GameMcpCatalog.definitions()
	assert_gt(definitions.size(), 0, "the shared catalog carries the runtime tools")
	for def in definitions:
		assert_true(registry.has_tool(def.name), "registered %s" % def.name)


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


func test_gated_actions_require_per_call_confirmation_before_marshalling() -> void:
	# The stub table deliberately accepts any write. The transport's precheck
	# must refuse an unconfirmed gated call before reaching it (and before
	# marshalling its args).
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


func test_cancelled_screenshot_reports_the_cancellation() -> void:
	ctx.cancelled = true
	var result := await _call("game_screenshot")
	assert_true(result.is_error)


func test_wrong_typed_screenshot_args_error_cleanly() -> void:
	# int()/float() on a non-numeric Variant is a script error; each wrong
	# shape must be rejected by validation instead.
	for args in [
		{"max_dim": [1280]},
		{"max_dim": "wide"},
		{"quality": {"value": 0.8}},
		{"format": 3},
	]:
		var result := await _call("game_screenshot", args)
		assert_true(result.is_error, "wrong-typed %s errors" % [args])


func test_render_diagnostics_routes_exact_state_through_the_adapter_seam() -> void:
	var result := await _call("game_render_diagnostics")
	assert_false(result.is_error)
	assert_eq(result.structured["schema"], "OpenNovaRenderDiagnosticsV1")
	assert_eq(result.structured["camera"]["fov_deg"], 50.534)
	assert_eq(result.structured["passes"]["root"]["shadow_draw_calls"], 17)


func test_capture_bundle_returns_lossless_artifact_metadata_and_optional_image() -> void:
	var result := await _call("game_capture_bundle", {
		"label": "00TRa-courtyard",
		"settle_frames": 3,
		"world_only": true,
		"include_image": true,
	})
	assert_false(result.is_error)
	assert_eq(adapter.capture_args["label"], "00TRa-courtyard")
	assert_eq(result.structured["schema"], "OpenNovaRenderCaptureV1")
	assert_eq(result.structured["artifact"]["width"], 1600)
	assert_eq(result.structured["artifact"]["height"], 900)
	assert_false(result.structured.has("image_bytes"),
			"transport-only PNG bytes stay out of structuredContent")
	assert_eq(result.content[0]["type"], "image")
	assert_eq(result.content[0]["mimeType"], "image/png")

	result = await _call("game_capture_bundle", {"include_image": false})
	assert_false(result.is_error)
	for block in result.content:
		assert_ne(block.get("type", ""), "image")


func test_render_capture_arguments_reject_coercible_values_before_capture() -> void:
	for args in [
		{"label": ["courtyard"]},
		{"settle_frames": "3"},
		{"world_only": 1},
		{"include_image": "yes"},
	]:
		adapter.capture_args = {}
		var result := await _call("game_capture_bundle", args)
		assert_true(result.is_error, "wrong-typed %s errors" % [args])
		assert_true(adapter.capture_args.is_empty(),
				"invalid arguments never reach the capture adapter")
