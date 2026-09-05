extends GutTest

# The runtime tool handlers over the abstract adapter seam (GameMcpAdapter,
# faked through its public verbs) and the REAL debug-control table over a
# faked DebugShellHost answering one real MissionRoot (ADR 0043 rule 11): the
# transport's per-caller authority refusals, argument marshalling and JSON
# shapes are observed on the engine owner, never on a stub table.


class AdapterStub:
	extends GameMcpAdapter

	var debug: DebugControlTable = null
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

	func get_debug_controls() -> DebugControlTable:
		return debug

	func runtime_status() -> Dictionary:
		return {"label": "stub"}

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
var _runtime: MissionRoot
var _sim: Simulation
var _host: DebugHostFixture


func before_each() -> void:
	# The real table over one real runtime: the engine rows have an owner to
	# reach, and the host's authority fact is the test's to flip.
	_runtime = WorldFixture.boot_mission_data(self, WorldFixture.default_mission(0))
	_sim = _runtime.get_sim()
	_host = DebugHostFixture.for_runtime(_runtime, self)
	adapter = add_child_autofree(AdapterStub.new())
	adapter.debug = DebugControlTable.new()
	adapter.debug.setup(_host)
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
		"id": "runtime_wac_paused",
		"value": true,
		"confirm_authority": true,
	})
	assert_false(result.is_error, "a confirmed gated write reaches the table")
	assert_eq(result.structured["id"], "runtime_wac_paused")
	assert_eq(result.structured["value"], true)
	assert_true(_sim.is_wac_paused(), "the write reached the engine owner")

	var refused := await _call("game_debug", {
		"op": "set",
		"id": "runtime_wac_paused",
		"value": false,
	})
	assert_true(refused.is_error, "an unconfirmed gated write is refused before the table")
	assert_true(_sim.is_wac_paused(), "the refusal left the owner untouched")


func test_state_rows_reflect_the_callers_confirmed_authority() -> void:
	# A confirm_authority caller's rows must answer for THAT caller: reporting
	# writable=false / "confirm_authority" after its own write succeeded
	# misleads MCP consumers into thinking the mutation was rejected.
	var locked := await _call("game_debug", {"op": "get", "id": "runtime_wac_paused"})
	assert_false(bool(locked.structured["writable"]),
			"an unconfirmed caller sees the locked policy view")
	assert_true(String(locked.structured["reason"]).contains("confirm_authority"))

	var open := await _call("game_debug", {
		"op": "get",
		"id": "runtime_wac_paused",
		"confirm_authority": true,
	})
	assert_true(bool(open.structured["writable"]),
			"op=get threads the caller's confirmed authority into the row")

	var written := await _call("game_debug", {
		"op": "set",
		"id": "runtime_wac_paused",
		"value": false,
		"confirm_authority": true,
	})
	assert_true(bool(written.structured["writable"]),
			"the row returned after a confirmed set reflects that authority")

	var listed := await _call("game_debug", {
		"op": "list",
		"filter": "Pause mission scripts",
		"confirm_authority": true,
	})
	var rows: Array = listed.structured["controls"]
	assert_eq(rows.size(), 1, "the filter narrows the list to the one row")
	assert_true(bool(rows[0]["state"]["writable"]))

	var snapshot := await _call("game_debug", {
		"op": "snapshot",
		"filter": "Pause mission scripts",
		"confirm_authority": true,
	})
	assert_true(bool((snapshot.structured["controls"] as Array)[0]["state"]["writable"]))
	assert_eq(snapshot.structured["runtime"]["label"], "stub",
			"the snapshot carries the shell's runtime block")
	assert_eq(snapshot.structured["edit_unlocked"], false)

	_host.authority = false
	var joiner := await _call("game_debug", {
		"op": "get",
		"id": "runtime_wac_paused",
		"confirm_authority": true,
	})
	assert_false(bool(joiner.structured["writable"]),
			"explicit confirmation cannot override joiner authority")


func test_authority_confirmation_requires_a_json_boolean() -> void:
	var result := await _call("game_debug", {
		"op": "set",
		"id": "runtime_wac_paused",
		"value": true,
		"confirm_authority": "true",
	})
	assert_true(result.is_error, "a truthy string cannot opt into authoritative mutation")
	assert_false(_sim.is_wac_paused())


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
	assert_false(result.is_error, "the by-name teleport marshals and reaches the sim")
	assert_eq(result.structured["error"], OK)
	assert_eq(result.structured["state"]["id"], "teleport_local_player")

	await _call("game_debug", {
		"op": "invoke",
		"id": "set_mission_variable",
		"args": {"index": 17, "value": -3},
		"confirm_authority": true,
	})
	assert_eq(_sim.get_mission_variable(17), -3, "the decoded arguments reached the engine")

	var paused := await _call("game_debug", {
		"op": "invoke",
		"id": "runtime_transport",
		"args": {"action": "pause"},
		"confirm_authority": true,
	})
	assert_false(paused.is_error)
	assert_false(_runtime.is_playing(), "the transport row paused the runtime")
	await _call("game_debug", {
		"op": "invoke",
		"id": "runtime_transport",
		"args": {"action": "resume"},
		"confirm_authority": true,
	})
	assert_true(_runtime.is_playing())

	var bus := AudioServer.get_bus_index("Master")
	var previous := AudioServer.is_bus_mute(bus)
	var muted := await _call("game_debug", {
		"op": "invoke",
		"id": "set_audio_bus_mute",
		"args": {"bus": "Master", "muted": previous},
	})
	assert_false(muted.is_error, "the ungated audio rows need no confirmation")
	assert_eq(AudioServer.is_bus_mute(bus), previous)


func test_gated_actions_require_per_call_confirmation_before_marshalling() -> void:
	# The transport's precheck refuses an unconfirmed gated call before the
	# table (and before marshalling its args).
	var result := await _call("game_debug", {
		"op": "invoke",
		"id": "set_mission_variable",
		"args": {"index": 17, "value": -3},
	})
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("confirm_authority"))
	assert_eq(_sim.get_mission_variable(17), 0, "the refused call never reached the engine")


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
		var result := await _call("game_debug", {
			"op": "invoke",
			"id": row["id"],
			"args": row["args"],
			"confirm_authority": true,
		})
		assert_true(result.is_error,
				"Bad numeric input errors for %s" % row["id"])
	assert_eq(_sim.get_mission_variable(17), 0, "bad numeric input never reaches the engine")


func test_debug_action_validation_is_actionable() -> void:
	var result := await _call("game_debug", {
		"op": "invoke",
		"id": "set_entity_position",
		"args": {"entity": 2},
		"confirm_authority": true,
	})
	assert_true(result.is_error)
	assert_true(String(result.content[0]["text"]).contains("position"))


func test_snapshot_filter_reaches_the_table() -> void:
	var result := await _call("game_debug", {
		"op": "snapshot",
		"filter": "terrain",
	})
	var rows: Array = result.structured["controls"]
	assert_gt(rows.size(), 0, "the filter keeps the terrain rows")
	for row in rows:
		var haystack := ("%s %s %s %s" % [
			row["id"], row["page"], row["label"], row["description"]]).to_lower()
		assert_true(haystack.contains("terrain"), "%s matches the filter" % row["id"])


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
