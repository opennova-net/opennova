extends GutTest

# DebugControlTable is the typed debug-control table (ADR 0043 d12) the F3
# windows and MCP's game_debug share. These tests drive the real C++ table
# over a faked shell seam (DebugHostFixture, rule 11's interface fake)
# answering REAL MissionRoot runtimes (each over an in-memory default
# mission, so its Simulation is the engine row owner): rows resolve their
# owners live per call, typed argument checks refuse bad input before any
# engine call, and nothing replays across an owner swap (a fresh mission gets
# fresh debug state).

# The registration order IS the op=list wire order (debug_control_ids.h).
const EXPECTED_IDS: Array[StringName] = [
	&"hide_foliage",
	&"hide_particles",
	&"force_fp_arms",
	&"body_in_first_person",
	&"third_person_on_foot",
	&"terrain_draw_mode",
	&"terrain_lod_quality",
	&"terrain_no_frustum",
	&"terrain_no_nearfar",
	&"terrain_no_sideplanes",
	&"terrain_no_partial_subdiv",
	&"terrain_force_leaves",
	&"terrain_force_lod0",
	&"viewport_debug_draw",
	&"occlusion_culling",
	&"teleport_local_player",
	&"cycle_map_mode",
	&"set_entity_health",
	&"set_entity_position",
	&"set_entity_item_attrib",
	&"set_audio_bus_volume",
	&"set_audio_bus_mute",
	&"set_audio_bus_solo",
	&"set_audio_bus_bypass",
	&"runtime_transport",
	&"runtime_return_to_menu",
	&"runtime_wac_paused",
	&"set_mission_variable",
	&"environment_time_of_day",
	&"environment_wind_strength",
	&"environment_lightning_short",
	&"environment_lightning_long",
	&"environment_rain",
	&"environment_snow",
	&"environment_overcast",
	&"environment_fog_distance",
	&"environment_move_fog",
	&"environment_sky_speed",
	&"environment_quake",
	&"environment_fog_type",
	&"environment_sky_height",
	&"environment_time_of_day_minutes",
	&"environment_sun_fade",
	&"environment_color_fade",
	&"environment_wind_scale",
	&"environment_block_color",
	&"environment_lightning_color",
	&"environment_weather_snapshot",
	&"deploy_pick",
	&"set_viewmodel_weapon",
	&"clear_viewmodel_weapon",
	&"net_joiner_diagnostics",
	&"kill_group",
	&"crew_vehicle",
	&"crew_local_player",
	&"local_player_look",
	&"local_spectator",
]

const WIRE_ROW_KEYS := ["id", "page", "label", "description", "kind", "target",
		"minimum", "maximum", "step", "choices", "requires_unlock", "authority", "args"]
const WIRE_STATE_KEYS := ["id", "kind", "value", "desired_value", "available",
		"writable", "authoritative", "reason"]
# Engine rows end in a Simulation / Terrain / Weather / environment call;
# device rows are viewport / audio / shell state.
const ENGINE_ROWS := 43
const DEVICE_ROWS := 14


var _sim: Simulation
var _runtime: MissionRoot
var _host: DebugHostFixture
var _controls: DebugControlTable


func before_each() -> void:
	# A real MissionRoot over an in-memory default mission: its Simulation is
	# the engine row owner the table resolves per call through the host.
	_runtime = WorldFixture.boot_mission_data(self, WorldFixture.default_mission(0))
	_sim = _runtime.get_sim()
	assert_not_null(_sim, "the runtime owns a live Simulation")
	_host = DebugHostFixture.for_runtime(_runtime, self)
	_controls = DebugControlTable.new()
	_controls.setup(_host)


func test_the_table_registers_the_wire_catalog() -> void:
	var ids := _controls.row_ids()
	assert_eq(ids.size(), EXPECTED_IDS.size())
	for i in range(EXPECTED_IDS.size()):
		assert_eq(ids[i], EXPECTED_IDS[i], "row %d keeps its wire id" % i)
	var owner_counts := {DebugControlRow.OWNER_ENGINE: 0, DebugControlRow.OWNER_DEVICE: 0}
	for id in ids:
		var row := _controls.control(id)
		assert_false(row.label.is_empty(), "'%s' carries a label" % id)
		assert_false(row.tooltip.is_empty(), "'%s' carries a tooltip" % id)
		assert_true(owner_counts.has(row.owner), "'%s' has a known owner" % id)
		owner_counts[row.owner] += 1
		var json: Dictionary = row.to_json_value()
		assert_eq(json.keys(), WIRE_ROW_KEYS,
				"'%s' keeps the legacy wire row keys" % id)
		assert_null(row.state, "a catalog row carries no caller state")
	assert_eq(owner_counts[DebugControlRow.OWNER_ENGINE], ENGINE_ROWS,
			"engine rows end in a Simulation/Terrain/Weather/environment call")
	assert_eq(owner_counts[DebugControlRow.OWNER_DEVICE], DEVICE_ROWS,
			"device rows are viewport/audio/shell state")
	assert_ne(JSON.stringify(_controls.capture_snapshot()), "",
			"the entire MCP snapshot is JSON-safe")


func test_clear_drops_every_row_and_the_host() -> void:
	# The shell's adapter builds the shipping table over its host and drops
	# both before its script teardown.
	var adapter: GameDebugAdapter = add_child_autofree(GameDebugAdapter.new())
	adapter.configure(autofree(GameShell.new()))
	var retained: DebugControlTable = adapter.get_debug_controls()
	assert_not_null(retained.control(&"hide_foliage"))
	assert_not_null(retained.get_host(), "the adapter's host answers the rows")

	adapter.release_shell()

	assert_null(adapter.get_debug_controls())
	assert_true(retained.row_ids().is_empty())
	assert_null(retained.control(&"hide_foliage"))
	assert_null(retained.get_host())


func test_list_pairs_definitions_with_live_state_and_filters() -> void:
	var state := _controls.get_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_true(state.writable)
	assert_true(state.authoritative)
	assert_eq(state.value, false)
	assert_eq(state.to_json_value().keys(), WIRE_STATE_KEYS)

	var rows := _controls.list_controls(&"Terrain")
	assert_eq(rows.size(), 9,
			"the Terrain page pairs hide_foliage with the 8 terrain rows")
	for row in rows:
		var listed := row as DebugControlRow
		assert_not_null(listed.state, "listed rows carry their live state")
		assert_true(listed.to_json_value().has("state"),
				"a listed row's JSON carries the state key")

	assert_eq(_controls.list_controls(&"", "vegetation").size(), 1,
			"the catalog filter searches descriptions as well as labels")
	var snapshot: Dictionary = _controls.capture_snapshot()
	assert_eq(snapshot["edit_unlocked"], false,
			"the wire-stable latch key reports false forever")
	assert_eq((snapshot["controls"] as Array).size(), EXPECTED_IDS.size())


func test_values_validate_and_normalize_before_the_owner() -> void:
	assert_eq(_controls.set_value(&"net_joiner_diagnostics", 1),
			ERR_INVALID_PARAMETER, "checks take exactly a bool")
	for invalid in [INF, -INF, NAN, "wide"]:
		assert_eq(_controls.set_value(&"terrain_lod_quality", invalid),
				ERR_INVALID_PARAMETER,
				"non-finite slider values are rejected before owner resolution")
	var lod := _controls.control(&"terrain_lod_quality")
	var snapped: Variant = DebugControlTable.normalize_value(lod, 3.13)
	assert_almost_eq(float(snapped), 3.1, 0.001,
			"sliders snap to their public step")
	var clamped: Variant = DebugControlTable.normalize_value(lod, 99.0)
	assert_almost_eq(float(clamped), 4.0, 0.001,
			"sliders clamp to their public domain")
	var mode := _controls.control(&"viewport_debug_draw")
	assert_null(DebugControlTable.normalize_value(mode, 99))
	assert_null(DebugControlTable.normalize_value(mode, -1))
	assert_null(DebugControlTable.normalize_value(mode, true))
	assert_null(DebugControlTable.normalize_value(mode, 1.5))
	assert_eq(DebugControlTable.normalize_value(mode, 4.0), 4,
			"an integral float (a JSON number over MCP) is an enum index")

	# The viewport row is a live device row in this in-tree harness.
	var viewport := get_viewport()
	var previous := int(viewport.debug_draw)
	assert_eq(_controls.set_value(&"viewport_debug_draw", 1), OK)
	assert_eq(int(viewport.debug_draw), 1)
	assert_eq(_controls.get_state(&"viewport_debug_draw").value, 1)
	assert_eq(_controls.set_value(&"viewport_debug_draw", previous), OK)


func test_actions_validate_typed_arguments_before_the_engine() -> void:
	var refused: Array = [
		[&"teleport_local_player", [Vector3.ZERO, 0.0, 91.0]],
		[&"teleport_local_player", [Vector3.ZERO, "0", 0.0]],
		[&"teleport_local_player", [Vector3(32768.0, 0.0, 0.0), 0.0, 0.0]],
		[&"set_entity_health", [0, 32768]],
		[&"set_entity_health", ["0", 25]],
		[&"set_entity_position", [0, Vector3(32768.0, 0.0, 0.0)]],
		[&"set_mission_variable", [17.5, -3]],
		[&"deploy_pick", [-1]],
		[&"kill_group", [0]],
		[&"crew_vehicle", [3]],
		[&"crew_local_player", ["11"]],
		[&"local_player_look", ["3", 1.0]],
		[&"runtime_transport", ["warp"]],
		[&"set_entity_item_attrib", [0xFFFF, 0, 0]],
		[&"set_entity_item_attrib", [7, 0x100000000, 0]],
		[&"set_entity_item_attrib", [7, 0, -1]],
		[&"set_entity_item_attrib", [7, 0]],
		[&"environment_block_color", [9, 0]],
		[&"environment_lightning_color", [0x1000000]],
	]
	for case in refused:
		assert_eq(int(_controls.invoke(case[0], case[1], true).error),
				ERR_INVALID_PARAMETER,
				"typed argument checks refuse %s before the engine call" % case[0])

	assert_eq(int(_controls.invoke(&"set_mission_variable", [17, -3], true).error), OK)
	var killed := _controls.invoke(&"kill_group", [14], true)
	assert_eq(int(killed.error), OK)
	assert_eq(int(killed.result), 0,
			"the engine's kill count passes through as the action result")
	# The engine's verdict: the host's own client queues the pick on its
	# loopback like a joiner's (case 12 has no authority test; D-NET-339).
	var picked := _controls.invoke(&"deploy_pick", [0], true)
	assert_eq(int(picked.error), OK)
	assert_eq(picked.result, true,
			"in-domain arguments reach the engine's own verdict")

	# Over the real runtime the local player exists, so the seat refusal is the
	# mount command's own verdict (no vehicle carries SSN 11 in the default
	# mission: ERR_INVALID_PARAMETER), not the typed-argument gate (11 is in
	# the argument's domain) and not the "no local player" ERR_UNAVAILABLE of a
	# mission-less Simulation. The engine's direct answer to the same call is
	# the action result.
	var seat := _controls.invoke(&"crew_local_player", [11], true)
	assert_eq(int(seat.error), int(_sim.debug_crew_local_player(11)),
			"the engine's seat refusal propagates as the action result")
	assert_eq(int(seat.error), int(ERR_INVALID_PARAMETER),
			"the refusal is the mount command's, with a live local player to seat")


func test_action_args_marshal_by_name_and_publish_their_schema() -> void:
	assert_eq(_controls.marshal_invoke_args(&"teleport_local_player",
			{"position": [1.0, 2.0, 3.0]}).args, [Vector3(1.0, 2.0, 3.0), 0.0, 0.0],
			"a by-name object marshals; optional yaw/pitch default to 0")
	assert_eq(_controls.marshal_invoke_args(&"set_entity_position",
			{"entity": 2, "position": {"x": 1, "y": 2, "z": 3}}).args,
			[2, Vector3(1.0, 2.0, 3.0)], "an {x, y, z} position marshals")
	assert_eq(_controls.marshal_invoke_args(&"runtime_transport", "pause").args, ["pause"],
			"one scalar is the single positional argument")
	assert_eq(_controls.marshal_invoke_args(&"environment_rain",
			{"percent": 50, "seconds": 10}).args, [50, 10],
			"the weather rows marshal by their WAC argument names")
	assert_eq(_controls.marshal_invoke_args(&"runtime_wac_paused", true).args, [true],
			"a check row passes its value through as the one argument op=set reads")

	var missing := _controls.marshal_invoke_args(&"set_entity_position", {"entity": 2})
	assert_true(missing.refused and missing.reason.contains("position"),
			"a refusal names the missing argument: %s" % missing.reason)
	var wrong := _controls.marshal_invoke_args(&"set_entity_health",
			{"entity": 7, "health": "banana"})
	assert_true(wrong.refused and wrong.reason.contains("health"),
			"a refusal names the argument outside its domain: %s" % wrong.reason)
	var extra := _controls.marshal_invoke_args(&"kill_group", [14, 15])
	assert_true(extra.refused, "surplus positional arguments are refused: %s" % extra.reason)
	assert_true(_controls.marshal_invoke_args(&"nonexistent_action", null).refused)

	var published: Array = _controls.control(&"crew_vehicle").to_json_value()["args"]
	assert_eq(published.map(func(spec: Dictionary) -> String: return spec["name"]),
			["occupant_ssn", "vehicle_ssn"], "op=list publishes the arg names in order")
	assert_eq(published[0]["kind"], "int")
	assert_eq(published[0]["minimum"], 1.0)
	assert_false(published[0].has("maximum"), "an open upper bound is not published")
	var zone: Dictionary = _controls.control(&"deploy_pick").to_json_value()["args"][0]
	assert_eq([zone["required"], zone["default"]], [false, 0])
	var transport: Dictionary = _controls.control(&"runtime_transport").to_json_value()["args"][0]
	assert_eq(transport["choices"], ["resume", "pause", "step"])
	# The schema is readable as typed records on the row too.
	var specs: Array = _controls.control(&"crew_vehicle").args
	assert_eq((specs[0] as DebugArgSpec).name, "occupant_ssn")
	assert_eq((specs[0] as DebugArgSpec).kind, DebugArgSpec.INT)
	assert_true((specs[0] as DebugArgSpec).required)


func test_confirmation_and_authority_are_distinct_gates() -> void:
	var unconfirmed := _controls.invoke(&"teleport_local_player", [Vector3.ZERO, 0.0, 0.0])
	assert_eq(int(unconfirmed.error), ERR_UNAUTHORIZED,
			"a confirmation-gated action refuses an unconfirmed caller")
	assert_eq(_controls.set_value(&"runtime_wac_paused", true), ERR_UNAUTHORIZED)
	assert_eq(_controls.set_value(&"local_spectator", true), ERR_UNAUTHORIZED,
			"spectator mode cannot mutate authority without per-call confirmation")

	var locked := _controls.get_state(&"teleport_local_player")
	assert_false(locked.writable)
	assert_string_contains(locked.reason, "confirm_authority")
	var confirmed := _controls.get_state(&"teleport_local_player", true)
	assert_true(confirmed.writable,
			"a per-call authority confirmation sees the write it may make")
	assert_eq(confirmed.reason, "")
	assert_true(_controls.get_state(&"local_spectator", true).writable,
			"confirmed authority tooling may drive the real spectator state")
	var rows := _controls.list_controls(&"", "Teleport", true)
	assert_eq(rows.size(), 1)
	assert_true((rows[0] as DebugControlRow).state.writable,
			"listed rows reflect the same caller authority")

	_host.authority = false
	assert_eq(int(_controls.invoke(&"teleport_local_player",
			[Vector3.ZERO, 0.0, 0.0], true).error), ERR_UNAUTHORIZED,
			"explicit confirmation cannot override joiner authority")
	var joiner := _controls.get_state(&"teleport_local_player", true)
	assert_false(joiner.writable)
	assert_string_contains(joiner.reason, "host")
	assert_false(_controls.get_state(&"local_spectator", true).writable,
			"a joiner cannot use tooling to manufacture spectator authority")
	_host.authority = true

	assert_true(_controls.get_state(&"net_joiner_diagnostics").writable,
			"ungated rows stay writable for unconfirmed callers")
	assert_eq(_controls.set_value(&"net_joiner_diagnostics", false), OK)


func test_reads_are_live_and_nothing_replays_across_an_owner_swap() -> void:
	assert_eq(_controls.set_value(&"net_joiner_diagnostics", true), OK)
	assert_true(_sim.is_joiner_network_diagnostics_enabled())
	assert_eq(_controls.get_state(&"net_joiner_diagnostics").value, true)

	# A fresh mission: a second real runtime (its own Simulation) behind the
	# same host.
	var replacement_runtime: MissionRoot = WorldFixture.boot_mission_data(
			self, WorldFixture.default_mission(0))
	var replacement: Simulation = replacement_runtime.get_sim()
	_host.runtime = replacement_runtime
	assert_eq(_controls.get_state(&"net_joiner_diagnostics").value, false,
			"reads re-resolve the replacement owner on the next call")
	assert_false(replacement.is_joiner_network_diagnostics_enabled(),
			"nothing replays prior debug intent into a fresh mission")
	assert_true(_sim.is_joiner_network_diagnostics_enabled(),
			"the departed owner keeps its own state")

	# invoke on a non-action row routes through the same set path.
	var outcome := _controls.invoke(&"net_joiner_diagnostics", [true])
	assert_eq(int(outcome.error), OK)
	assert_true(replacement.is_joiner_network_diagnostics_enabled())

	_host.runtime = null
	var gone := _controls.get_state(&"net_joiner_diagnostics")
	assert_false(gone.available)
	assert_string_contains(gone.reason, "No simulation is active")
	assert_eq(_controls.set_value(&"net_joiner_diagnostics", true), ERR_UNAVAILABLE)


func test_unavailable_owners_report_their_reason_rows() -> void:
	var world_row := _controls.get_state(&"hide_foliage")
	assert_false(world_row.available)
	assert_string_contains(world_row.reason, "No game world is loaded")
	assert_eq(_controls.set_value(&"hide_foliage", true), ERR_UNAVAILABLE)

	assert_string_contains(_controls.get_state(&"force_fp_arms").reason,
			"No local player presenter is active")
	assert_string_contains(_controls.get_state(&"terrain_lod_quality").reason,
			"The current world has no terrain")
	assert_string_contains(
			_controls.get_state(&"environment_wind_strength", true).reason,
			"The current world has no weather controller")
	assert_string_contains(
			_controls.get_state(&"environment_block_color", true).reason,
			"The current world has no weather controller")
	assert_eq(int(_controls.invoke(&"set_viewmodel_weapon", ["WPN_M4"], true).error),
			ERR_UNAVAILABLE, "world actions refuse while no world is loaded")
	assert_eq(int(_controls.invoke(&"environment_sun_fade", [50, 5], true).error),
			ERR_UNAVAILABLE, "weather actions refuse while no world is loaded")


func test_audio_actions_validate_their_wire_args_and_mutate_the_mixer() -> void:
	var refused: Array = [
		[&"set_audio_bus_volume", ["SFX"]],
		[&"set_audio_bus_volume", ["SFX", 99.0]],
		[&"set_audio_bus_volume", [1, -10.0]],
		[&"set_audio_bus_volume", ["", -10.0]],
		[&"set_audio_bus_mute", ["SFX", 1]],
		[&"set_audio_bus_solo", [true, true]],
	]
	for case in refused:
		assert_eq(int(_controls.invoke(case[0], case[1]).error),
				ERR_INVALID_PARAMETER,
				"audio argument checks refuse %s" % [case[1]])
	assert_eq(int(_controls.invoke(&"set_audio_bus_mute", ["__missing_bus__", true]).error),
			ERR_INVALID_PARAMETER, "an unknown bus name is the row's own refusal")

	var bus := AudioServer.get_bus_index("Master")
	var previous := AudioServer.is_bus_mute(bus)
	assert_eq(int(_controls.invoke(&"set_audio_bus_mute", ["Master", true]).error), OK)
	assert_true(AudioServer.is_bus_mute(bus))
	assert_eq(int(_controls.invoke(&"set_audio_bus_mute", ["Master", previous]).error), OK)
	assert_eq(AudioServer.is_bus_mute(bus), previous)


func test_the_shell_verbs_reach_the_host() -> void:
	var leave := _controls.invoke(&"runtime_return_to_menu", [], true)
	assert_eq(int(leave.error), OK)
	assert_eq(_host.return_to_menu_calls, 1, "the row runs the host's return-to-menu leg")
	_host.return_to_menu_result = ERR_BUSY
	assert_eq(int(_controls.invoke(&"runtime_return_to_menu", []).error), ERR_BUSY,
			"the host's verdict is the row's verdict")
	var paused := _controls.invoke(&"runtime_transport", ["pause"], true)
	assert_eq(int(paused.error), OK, "the transport row pauses the runtime")
	assert_false(_runtime.is_playing())
	assert_eq(int(_controls.invoke(&"runtime_transport", ["resume"], true).error), OK)
	assert_eq(_host.resume_calls, 1, "resume runs the host's resume leg")
	assert_true(_runtime.is_playing())


func test_unknown_controls_and_kind_mismatches_error_cleanly() -> void:
	assert_eq(_controls.set_value(&"nonexistent_option", true), ERR_DOES_NOT_EXIST)
	var unknown := _controls.get_state(&"nonexistent_option")
	assert_string_contains(unknown.reason, "Unknown debug control")
	assert_eq(int(_controls.invoke(&"nonexistent_option").error), ERR_DOES_NOT_EXIST)
	assert_eq(_controls.set_value(&"kill_group", 3),
			ERR_INVALID_PARAMETER, "set_value on an action row is refused")
