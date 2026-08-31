extends GutTest

# DebugControls is the typed debug-control table (ADR 0042 d5) shared by the
# MCP game_debug plane. These tests drive the real table over stub seams with
# REAL Simulation instances: rows resolve their owners live per call, typed
# argument checks refuse bad input before any engine call, and nothing
# replays across an owner swap (a fresh mission gets fresh debug state).

# The registration order IS the op=list wire order.
const EXPECTED_IDS: Array[StringName] = [
	&"show_skeletons",
	&"show_user_points",
	&"show_collision",
	&"hide_foliage",
	&"hide_particles",
	&"show_effect_boxes",
	&"show_portal_faces",
	&"show_round_trails",
	&"show_rays",
	&"show_hit_meshes",
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
		"minimum", "maximum", "step", "choices", "requires_unlock", "authority"]
const WIRE_STATE_KEYS := ["id", "kind", "value", "desired_value", "available",
		"writable", "authoritative", "reason"]


class RuntimeStub:
	extends MissionPresentation

	var stub_sim: Simulation = null

	func get_sim() -> Simulation:
		return stub_sim


class AuthorityAdapter:
	extends GameDebugAdapter

	var host_authority := true

	func has_debug_authority() -> bool:
		return host_authority


var _sim: Simulation
var _runtime: RuntimeStub
var _adapter: AuthorityAdapter
var _controls: DebugControls


func before_each() -> void:
	_sim = autofree(Simulation.new())
	_runtime = autofree(RuntimeStub.new())
	_runtime.stub_sim = _sim
	var seams := GameShellSeams.new()
	seams.runtime_source = func(): return _runtime
	seams.world_source = func(): return null
	seams.presenter_source = func(): return null
	seams.shell_state_source = func(): return "world"
	seams.world_loading_source = func(): return false
	seams.dev_tools_open_source = func(): return false
	seams.resume_action = func(): pass
	seams.quit_action = func(): pass
	_adapter = add_child_autofree(AuthorityAdapter.new())
	_adapter.configure(seams)
	_controls = _adapter.get_debug_controls()


func test_the_table_registers_the_wire_catalog() -> void:
	var ids := _controls.row_ids()
	assert_eq(ids.size(), EXPECTED_IDS.size())
	for i in range(EXPECTED_IDS.size()):
		assert_eq(ids[i], EXPECTED_IDS[i], "row %d keeps its wire id" % i)
	var owner_counts := {DebugControls.OWNER_ENGINE: 0, DebugControls.OWNER_DEVICE: 0}
	for id in ids:
		var row := _controls.control(id)
		assert_false(row.label.is_empty(), "'%s' carries a label" % id)
		assert_false(row.tooltip.is_empty(), "'%s' carries a tooltip" % id)
		assert_true(owner_counts.has(row.owner), "'%s' has a known owner" % id)
		owner_counts[row.owner] += 1
		if row.kind == DebugControls.Kind.ACTION:
			assert_true(row.invoke.is_valid(), "action '%s' carries invoke" % id)
		else:
			assert_true(row.read.is_valid(), "'%s' carries read" % id)
			assert_true(row.write.is_valid(), "'%s' carries write" % id)
		var json: Dictionary = row.to_json_value()
		assert_eq(json.keys(), WIRE_ROW_KEYS,
				"'%s' keeps the legacy wire row keys" % id)
	assert_eq(owner_counts[DebugControls.OWNER_ENGINE], 35,
			"engine rows end in a Simulation/Terrain/Weather/environment call")
	assert_eq(owner_counts[DebugControls.OWNER_DEVICE], 22,
			"device rows are viewport/overlay/audio/shell state")
	assert_ne(JSON.stringify(_controls.capture_snapshot()), "",
			"the entire MCP snapshot is JSON-safe")


func test_release_breaks_every_row_callable_cycle() -> void:
	var retained_controls := _controls
	var retained_row := _controls.control(&"show_skeletons")
	assert_true(retained_row.availability.is_valid())
	assert_true(retained_row.read.is_valid())
	assert_true(retained_row.write.is_valid())

	_adapter.release_shell_seams()

	assert_null(_adapter.get_debug_controls())
	assert_true(retained_controls.row_ids().is_empty())
	assert_null(retained_controls.control(&"show_skeletons"))
	assert_false(retained_row.availability.is_valid())
	assert_false(retained_row.read.is_valid())
	assert_false(retained_row.write.is_valid())
	assert_false(retained_row.invoke.is_valid())


func test_list_pairs_definitions_with_live_state_and_filters() -> void:
	var state := _controls.get_control_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_true(state.writable)
	assert_true(state.authoritative)
	assert_eq(state.value, false)
	assert_eq(state.to_json_value().keys(), WIRE_STATE_KEYS)

	var rows := _controls.list_controls(&"Terrain")
	assert_eq(rows.size(), 9,
			"the Terrain page pairs hide_foliage with the 8 terrain rows")
	for row in rows:
		assert_true(row.has("state"), "listed rows carry their live state")

	assert_eq(_controls.list_controls(&"", "capsule").size(), 1,
			"the catalog filter searches descriptions as well as labels")
	var snapshot: Dictionary = _controls.capture_snapshot()
	assert_eq(snapshot["edit_unlocked"], false,
			"the wire-stable latch key reports false forever")
	assert_eq((snapshot["controls"] as Array).size(), EXPECTED_IDS.size())


func test_values_validate_and_normalize_before_the_owner() -> void:
	assert_eq(_controls.set_control_value(&"net_joiner_diagnostics", 1),
			ERR_INVALID_PARAMETER, "checks take exactly a bool")
	for invalid in [INF, -INF, NAN, "wide"]:
		assert_eq(_controls.set_control_value(&"terrain_lod_quality", invalid),
				ERR_INVALID_PARAMETER,
				"non-finite slider values are rejected before owner resolution")
	var lod := _controls.control(&"terrain_lod_quality")
	var snapped: Dictionary = DebugControls.normalize_value(lod, 3.13)
	assert_true(bool(snapped["ok"]))
	assert_almost_eq(float(snapped["value"]), 3.1, 0.001,
			"sliders snap to their public step")
	var clamped: Dictionary = DebugControls.normalize_value(lod, 99.0)
	assert_almost_eq(float(clamped["value"]), 4.0, 0.001,
			"sliders clamp to their public domain")
	var mode := _controls.control(&"viewport_debug_draw")
	assert_false(bool(DebugControls.normalize_value(mode, 99)["ok"]))
	assert_false(bool(DebugControls.normalize_value(mode, -1)["ok"]))
	assert_false(bool(DebugControls.normalize_value(mode, true)["ok"]))

	# The viewport row is a live device row in this in-tree harness.
	var viewport := _adapter.get_viewport()
	var previous := int(viewport.debug_draw)
	assert_eq(_controls.set_control_value(&"viewport_debug_draw", 1), OK)
	assert_eq(int(viewport.debug_draw), 1)
	assert_eq(_controls.get_control_state(&"viewport_debug_draw").value, 1)
	assert_eq(_controls.set_control_value(&"viewport_debug_draw", previous), OK)


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
	]
	for case in refused:
		assert_eq(int(_controls.invoke_control(case[0], case[1], true)["error"]),
				ERR_INVALID_PARAMETER,
				"typed argument checks refuse %s before the engine call" % case[0])

	assert_eq(int(_controls.invoke_control(&"set_mission_variable",
			[17, -3], true)["error"]), OK)
	var killed := _controls.invoke_control(&"kill_group", [14], true)
	assert_eq(int(killed["error"]), OK)
	assert_eq(int(killed["result"]), 0,
			"the engine's kill count passes through as the action result")
	var picked := _controls.invoke_control(&"deploy_pick", [0], true)
	assert_eq(int(picked["error"]), OK)
	assert_eq(picked["result"], false,
			"in-domain arguments reach the engine's own verdict")

	var seat := _controls.invoke_control(&"crew_local_player", [11], true)
	assert_eq(int(seat["error"]), int(ERR_UNAVAILABLE),
			"the engine's seat refusal propagates as the action result")


func test_confirmation_and_authority_are_distinct_gates() -> void:
	var unconfirmed := _controls.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, 0.0, 0.0])
	assert_eq(int(unconfirmed["error"]), ERR_UNAUTHORIZED,
			"a confirmation-gated action refuses an unconfirmed caller")
	assert_eq(_controls.set_control_value(&"runtime_wac_paused", true),
			ERR_UNAUTHORIZED)
	assert_eq(_controls.set_control_value(&"local_spectator", true),
			ERR_UNAUTHORIZED,
			"spectator mode cannot mutate authority without per-call confirmation")

	var locked := _controls.get_control_state(&"teleport_local_player")
	assert_false(locked.writable)
	assert_string_contains(locked.reason, "confirm_authority")
	var confirmed := _controls.get_control_state(&"teleport_local_player", true)
	assert_true(confirmed.writable,
			"a per-call authority confirmation sees the write it may make")
	assert_eq(confirmed.reason, "")
	assert_true(_controls.get_control_state(&"local_spectator", true).writable,
			"confirmed authority tooling may drive the real spectator state")
	var rows := _controls.list_controls(&"", "Teleport", true)
	assert_eq(rows.size(), 1)
	assert_true(bool(rows[0]["state"]["writable"]),
			"listed rows reflect the same caller authority")

	_adapter.host_authority = false
	assert_eq(int(_controls.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, 0.0, 0.0], true)["error"]), ERR_UNAUTHORIZED,
			"explicit confirmation cannot override joiner authority")
	var joiner := _controls.get_control_state(&"teleport_local_player", true)
	assert_false(joiner.writable)
	assert_string_contains(joiner.reason, "host")
	assert_false(_controls.get_control_state(&"local_spectator", true).writable,
			"a joiner cannot use tooling to manufacture spectator authority")
	_adapter.host_authority = true

	assert_true(_controls.get_control_state(&"net_joiner_diagnostics").writable,
			"ungated rows stay writable for unconfirmed callers")
	assert_eq(_controls.set_control_value(&"net_joiner_diagnostics", false), OK)


func test_reads_are_live_and_nothing_replays_across_an_owner_swap() -> void:
	assert_eq(_controls.set_control_value(&"net_joiner_diagnostics", true), OK)
	assert_true(_sim.is_joiner_network_diagnostics_enabled())
	assert_eq(_controls.get_control_state(&"net_joiner_diagnostics").value, true)

	var replacement: Simulation = autofree(Simulation.new())
	_runtime.stub_sim = replacement
	assert_eq(_controls.get_control_state(&"net_joiner_diagnostics").value, false,
			"reads re-resolve the replacement owner on the next call")
	assert_false(replacement.is_joiner_network_diagnostics_enabled(),
			"nothing replays prior debug intent into a fresh mission")
	assert_true(_sim.is_joiner_network_diagnostics_enabled(),
			"the departed owner keeps its own state")

	# op=invoke on a non-action row routes through the same set path.
	var outcome := _controls.invoke_control(&"net_joiner_diagnostics", true)
	assert_eq(int(outcome["error"]), OK)
	assert_true(replacement.is_joiner_network_diagnostics_enabled())

	_runtime.stub_sim = null
	var gone := _controls.get_control_state(&"net_joiner_diagnostics")
	assert_false(gone.available)
	assert_string_contains(gone.reason, "No simulation is active")
	assert_eq(_controls.set_control_value(&"net_joiner_diagnostics", true),
			ERR_UNAVAILABLE)


func test_unavailable_owners_report_their_reason_rows() -> void:
	var world_row := _controls.get_control_state(&"show_collision")
	assert_false(world_row.available)
	assert_string_contains(world_row.reason, "No game world is loaded")
	assert_eq(_controls.set_control_value(&"show_collision", true),
			ERR_UNAVAILABLE)

	assert_string_contains(_controls.get_control_state(&"force_fp_arms").reason,
			"No local player presenter is active")
	assert_string_contains(_controls.get_control_state(&"terrain_lod_quality").reason,
			"The current world has no terrain")
	assert_string_contains(
			_controls.get_control_state(&"environment_wind_strength", true).reason,
			"The current world has no weather controller")
	assert_eq(int(_controls.invoke_control(&"set_viewmodel_weapon",
			["WPN_M4"], true)["error"]), ERR_UNAVAILABLE,
			"world actions refuse while no world is loaded")


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
		assert_eq(int(_controls.invoke_control(case[0], case[1])["error"]),
				ERR_INVALID_PARAMETER,
				"audio argument checks refuse %s" % [case[1]])

	var bus := AudioServer.get_bus_index("Master")
	var previous := AudioServer.is_bus_mute(bus)
	assert_eq(int(_controls.invoke_control(&"set_audio_bus_mute",
			["Master", true])["error"]), OK)
	assert_true(AudioServer.is_bus_mute(bus))
	assert_eq(int(_controls.invoke_control(&"set_audio_bus_mute",
			["Master", previous])["error"]), OK)
	assert_eq(AudioServer.is_bus_mute(bus), previous)


func test_unknown_controls_and_kind_mismatches_error_cleanly() -> void:
	assert_eq(_controls.set_control_value(&"nonexistent_option", true),
			ERR_DOES_NOT_EXIST)
	var unknown := _controls.get_control_state(&"nonexistent_option")
	assert_string_contains(unknown.reason, "Unknown debug control")
	assert_eq(int(_controls.invoke_control(&"nonexistent_option")["error"]),
			ERR_DOES_NOT_EXIST)
	assert_eq(_controls.set_control_value(&"kill_group", 3),
			ERR_INVALID_PARAMETER, "op=set on an action row is refused")
