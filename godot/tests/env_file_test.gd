extends GutTest

# The env is not bundled with the project; it loads from an external directory
# (the resource dir at runtime). The test fixture lives in repo-root fixtures/.
const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"


func _load_full_00() -> EnvFile:
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	return env


func test_full_00_env_loads_and_parses() -> void:
	var env := _load_full_00()

	assert_not_null(env, "the synthetic env should load as the shared EnvFile resource.")
	if env == null:
		return
	assert_true(env.is_loaded(), "Loaded EnvFile should report a valid document.")
	assert_eq(env.get_env_name(), "Synth_Full", "The authored enviro_name should round-trip into EnvFile.")
	assert_eq(env.get_timeofday(), "Day", "Stock timeofday should be parsed.")
	assert_eq(env.get_curtime(), 1200, "Stock curtime should be parsed.")
	assert_eq(env.get_fog_level(), 1000.0, "Stock fog level should be parsed.")
	assert_eq(env.get_fog_type(), 2, "Stock fog type should be parsed.")
	assert_false(env.has_water_height(), "Stock fixture should not claim an authored water height.")
	assert_eq(env.get_advanced_clouds(), 1, "Stock advanced cloud mode should be parsed.")
	assert_eq(env.get_tod_keyframes().size(), 10, "the synthetic env should expose all TOD keyframes.")
	assert_not_null(env.get_sky_map1_tex(), "Sky map 1 should resolve through the shared texture resolver.")
	assert_not_null(env.get_sky_map2_tex(), "Sky map 2 should resolve through the shared texture resolver.")


func test_water_murk_matches_retail_upper_only_clamp() -> void:
	var env := EnvFile.new()
	env.set_water_murk(-0.5)
	assert_almost_eq(env.get_water_murk(), -0.5, 0.000001,
			"retail retains a negative authored murk value")
	env.set_water_murk(1.5)
	assert_almost_eq(env.get_water_murk(), 0.99, 0.000001,
			"retail clamps murk only at the witnessed 0.99 upper bound")


func test_env_interpolation_and_export_round_trip() -> void:
	var env := _load_full_00()
	assert_not_null(env, "Fixture should load before interpolation.")
	if env == null:
		return

	var midday := env.interpolate_time_of_day(1200.0)
	assert_true(midday.has("sun"), "Interpolated TOD state should include sun lighting.")
	assert_true((midday["sun"] as Vector3).length() > 0.0, "Midday sun lighting should be non-zero.")
	var sun_dir := env.compute_sun_direction(1200.0)
	assert_almost_eq(sun_dir.x, -22414.0 / 65536.0, 1.0e-7,
			"sun x preserves the direct fixed getter tuple")
	assert_almost_eq(sun_dir.y, 61583.0 / 65536.0, 1.0e-7,
			"sun magnitude preserves the direct fixed getter tuple")

	env.set_env_name("Round Trip")
	var output_path := "user://round_trip.env"
	var save_err := env.save_to_path(output_path)
	assert_eq(save_err, OK, "EnvFile should save authored environments directly.")

	var reloaded := EnvFile.new()
	reloaded.set_source_path(output_path)
	var load_err := reloaded.load()
	assert_eq(load_err, OK, "Saved env should load back through the shared parser.")
	assert_eq(reloaded.get_env_name(), "Round Trip", "Saved env name should round-trip.")
	assert_eq(reloaded.get_tod_keyframes().size(), env.get_tod_keyframes().size(), "TOD keyframes should survive export.")


func test_set_sky_map_reresolves_cached_texture() -> void:
	var env := _load_full_00()
	var before := env.get_sky_map1_tex()
	assert_not_null(before, "fixture sky map resolves at load")

	env.set_sky_map1(env.get_sky_map2())

	var after := env.get_sky_map1_tex()
	assert_not_null(after, "the setter re-resolves against the fixture folder")
	assert_ne(after, before, "the cached texture follows the edit instead of going stale")


func test_set_sky_map_to_missing_name_clears_texture() -> void:
	var env := _load_full_00()
	assert_not_null(env.get_sky_map1_tex(), "fixture sky map resolves at load")

	env.set_sky_map1("no_such_cloud.pcx")

	assert_null(env.get_sky_map1_tex(), "an unresolvable name clears the cached texture (truthful preview)")


func test_set_sky_map_with_unchanged_name_keeps_texture_instance() -> void:
	var env := _load_full_00()
	var before := env.get_sky_map1_tex()

	env.set_sky_map1(env.get_sky_map1())

	assert_eq(env.get_sky_map1_tex(), before,
		"the diff guard must not hit the disk for an unchanged name (undo snapshot replays)")


# --- Engine-faithful surfaces from the environment-workspace effort ----------


func _new_default_env() -> EnvFile:
	var env := EnvFile.new()
	env.reset_to_default()
	return env


func test_to_bytes_load_bytes_round_trips() -> void:
	var env := _new_default_env()
	env.set_env_name("Snapshot Test")
	env.set_curtime(1830)
	env.set_fog_level(750.0)
	var bytes := env.to_bytes()
	assert_gt(bytes.size(), 0, "to_bytes should emit the serialized .env text.")

	var restored := _new_default_env()
	assert_true(restored.load_bytes(bytes), "load_bytes should parse a to_bytes snapshot.")
	assert_eq(restored.get_env_name(), "Snapshot Test", "Name should survive the byte round-trip.")
	assert_eq(restored.get_curtime(), 1830, "Curtime should survive the byte round-trip.")
	assert_almost_eq(restored.get_fog_level(), 750.0, 0.5, "Fog level should survive the byte round-trip.")


func test_mission_overrides_apply_as_live_view_only() -> void:
	var env := _new_default_env()
	env.set_fog_level(1000.0)
	env.set_water_murk(0.8)
	var base_bytes := env.to_bytes()

	var overrides := MissionEnvironmentOverrides.new()
	overrides.fog_level = 250.0
	overrides.water_color = Color(0.1, 0.2, 0.3)
	overrides.water_murk = 0.4
	env.apply_mission_overrides(overrides)
	assert_true(env.has_mission_overrides(), "Applying overrides should set the active flag.")
	assert_almost_eq(env.get_fog_level(), 250.0, 0.5, "Getters should see the overridden fog level.")
	assert_almost_eq(env.get_water_murk(), 0.4, 0.01, "Getters should see the overridden murk.")

	# Critical invariant: save/to_bytes always write the BASE config so a
	# mission-opened env can never persist contaminated values.
	assert_eq(env.to_bytes(), base_bytes, "to_bytes must emit the base config while overrides are active.")

	env.clear_mission_overrides()
	assert_false(env.has_mission_overrides(), "Clearing overrides should reset the flag.")
	assert_almost_eq(env.get_fog_level(), 1000.0, 0.5, "Clearing overrides should restore the base fog level.")


func test_environment_owns_the_authored_mission_clock() -> void:
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	var env := _load_full_00()
	env.set_curtime(900)
	env_node.environment_data = env
	assert_almost_eq(env_node.time_of_day, 900.0, 0.001,
		"assigning environment data after _ready synchronizes its authored curtime")

	# BMS start_time is Q8.8 hours. 0x0540 = 05:15 exactly.
	env_node.configure_mission_clock(0x0540, 60)
	assert_almost_eq(env_node.time_of_day, 515.0, 0.001,
		"the mission header, not a stale noon default, initializes the shared clock")

	# At the witnessed 62 logic ticks/s, 9,300 ticks are 150 seconds: one
	# game-hour step for a 60-real-minute authored day.
	env_node.advance_mission_clock(9300)
	assert_almost_eq(env_node.time_of_day, 615.0, 0.001,
		"the environment advances at minutes_per_day for every downstream consumer")


func test_weather_driven_clock_advance_never_clobbers_smoothed_currents() -> void:
	# The witnessed writer split [orig: Environment_ComputeTimeOfDayColors
	# @ 0x57de40]: TOD recomputes refresh the keyframe TARGETS; the weather
	# tick's smoothed writeback owns the CURRENT render colors. Before this pin,
	# the per-tick mission clock re-stamped raw keyframe colors between weather
	# writebacks and the whole scene strobed at the tick/frame beat (the
	# 2026-07-13 black-flicker regression).
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.set_weather_driven(true)
	var smoothed := Vector3(0.25, 0.5, 0.75)
	env_node.set_fill_light(smoothed)
	env_node.set_sun_light(smoothed)
	env_node.configure_mission_clock(0x0C00, 60)
	env_node.advance_mission_clock(310)  # 5 s of ticks -- many TOD recomputes
	assert_eq(env_node.get_fill_light(), smoothed,
		"weather-driven, the clock's TOD recompute must not clobber the smoothed fill")
	assert_eq(env_node.get_sun_light(), smoothed,
		"weather-driven, the clock's TOD recompute must not clobber the smoothed sun")
	assert_true(env_node.get_fill_light_target() != smoothed,
		"the keyframe TARGETS keep refreshing for the smoothers to chase")

	# Standalone (no weather node): the direct writes remain -- the editor env
	# preview owns its currents.
	env_node.set_weather_driven(false)
	env_node.advance_mission_clock(310)
	assert_true(env_node.get_fill_light() != smoothed,
		"without a weather driver the TOD recompute updates the currents directly")


func test_fog_start_follows_engine_policy() -> void:
	var env := _new_default_env()
	env.set_fog_level(1000.0)
	env.set_fog_type(1)
	assert_almost_eq(env.get_fog_start(), 0.5, 0.01, "fog_type 1 starts at the 0.5 unit constant.")
	env.set_fog_type(2)
	assert_almost_eq(env.get_fog_start(), 500.0, 0.5, "fog_type 2 starts at half the end distance.")
	env.set_fog_type(3)
	assert_almost_eq(env.get_fog_start(), 250.0, 0.5, "fog_type 3 starts at quarter the end distance.")

func test_smoothed_fog_start_tracks_current_end_and_invalidates_consumers() -> void:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env_node := MissionEnvironment.new()
	env_node.name = "Env"
	env_node.environment_data = _load_full_00()
	mount.add_child(env_node)
	var weather := Weather.new()
	weather.environment_path = NodePath("../Env")
	mount.add_child(weather)
	weather.prepare_world_driven()

	# fogdist(640) on the standalone weather home: the 1/32 spring settles the
	# smoothed current exactly on the target (retail WacCmd_FogDist @ 0x4ee100;
	# the spring @ 0x57ede2).
	var generation_before := env_node.get_env_generation()
	weather.command_fog_distance(640)
	for _i in range(1024):
		weather.tick_fixed()
	assert_almost_eq(env_node.get_fog_level(), 640.0, 0.001,
			"weather consumers read the smoothed fog end")
	assert_almost_eq(env_node.get_fog_start(), 320.0, 0.001,
			"type-2 fog start follows half of that same smoothed end")
	assert_gt(env_node.get_env_generation(), generation_before,
			"moving fog bounds invalidate cached object and clear-state consumers")


func test_underwater_pass_transition_publishes_once_and_is_idempotent() -> void:
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	var state: EnvLightState = env_node.get_light_state()
	var pass_events: Array[bool] = []
	state.pass_changed.connect(func() -> void:
		pass_events.append(true))
	var dry_generation := state.get_generation()

	env_node.set_underwater_view(false)
	assert_eq(state.get_generation(), dry_generation,
			"reselecting the current dry pass is a no-op")
	assert_eq(pass_events.size(), 0)
	env_node.set_underwater_view(true)
	assert_eq(pass_events.size(), 1,
			"crossing below emits one immediate-restamp event")
	assert_gt(state.get_generation(), dry_generation)
	var underwater_generation := state.get_generation()
	env_node.set_underwater_view(true)
	assert_eq(state.get_generation(), underwater_generation,
			"reselecting underwater neither republishes nor re-restamps")
	assert_eq(pass_events.size(), 1)
	env_node.set_underwater_view(false)
	assert_eq(pass_events.size(), 2,
			"surfacing emits the matching dry-pass restamp")
	assert_gt(state.get_generation(), underwater_generation)


func test_object_lighting_uses_the_active_moon_direction_at_night() -> void:
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 2200.0

	var values: EnvLightValues = env_node.get_light_state().get_values()
	var expected := -env_node.get_light_direction().normalized()
	assert_true(values.dir.is_equal_approx(expected),
			"object directional light follows Environment_GetLightDirectionFloat")
	assert_false(values.dir.is_equal_approx(-env_node.get_sun_direction().normalized()),
			"night objects must not stay pinned to the solar highlight vector")


func test_light_direction_render_tuple_is_the_raw_getter_and_the_godot_vector_its_swap() -> void:
	# At 15:00 Environment_ComputeSunDirection gives the fixed tuple
	# (0.9397 sin 225, 0.342, -0.9397 cos 225) and Environment_GetLightDirectionFloat
	# serves (-f1, f2, f0) = (-0.342, +0.6645, -0.6645); the Godot-axes vector is
	# that tuple's x/z swap. The terrain page path must see the RAW tuple.
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 1500.0
	var raw := env_node.get_light_direction_render_tuple()
	assert_almost_eq(raw.x, -0.342, 0.002, "raw tuple x is the fixed 22414 tilt, negated")
	assert_almost_eq(raw.y, 0.6645, 0.002, "raw tuple y is the up component at 15:00")
	assert_almost_eq(raw.z, -0.6645, 0.002, "raw tuple z is the east component at 15:00")
	var godot_axes := env_node.get_light_direction()
	assert_true(godot_axes.is_equal_approx(Vector3(raw.z, raw.y, raw.x)),
			"the Godot-axes light is the raw tuple's x/z swap (env_axes.h), nothing else")


func test_environment_publishes_the_world_lighting_block_as_shader_globals() -> void:
	# The object family reads retail's per-pass lighting block as global shader
	# parameters written on every publication; the per-entity factors ride the
	# u_entity_light instance uniform (object_model tests) and the lerp/
	# scale math is pinned engine-side (renderer::compute_entity_lighting).
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 1500.0
	var world_values: EnvLightValues = env_node.get_light_state().get_values()

	assert_true(world_values.floor_color.is_equal_approx(env_node.get_floor_color()))
	assert_true(world_values.ceiling.is_equal_approx(env_node.get_ceiling_color()))
	var expected := {
		"opennova_light_block_dir": world_values.dir,
		"opennova_light_block_dir_color": world_values.dir_color,
		"opennova_light_block_hemi_sky": world_values.hemi_sky,
		"opennova_light_block_hemi_ground": world_values.hemi_ground,
		"opennova_light_block_ceiling": world_values.ceiling,
		"opennova_light_block_floor": world_values.floor_color,
		"opennova_light_block_gain": world_values.gain,
	}
	# The headless Dummy RenderingServer does not retain global shader
	# parameters (get returns null); a rendering run verifies the writes.
	if RenderingServer.global_shader_parameter_get(
			"opennova_light_block_dir_color") == null:
		pending("the headless RenderingServer retains no global shader parameters")
		return
	for name in expected:
		var published: Vector3 = RenderingServer.global_shader_parameter_get(name)
		assert_true(published.is_equal_approx(expected[name]),
				"%s carries the published block value" % name)
	assert_true(bool(RenderingServer.global_shader_parameter_get("opennova_fog_enabled")),
			"a loaded world fogs the object family")

	# Leaving the tree restores the shipped noon register so a later preview
	# or mission never inherits this world's block (the globals are process-wide).
	remove_child(env_node)
	var noon := EnvLightValues.retail_noon_defaults()
	assert_true(Vector3(RenderingServer.global_shader_parameter_get(
			"opennova_light_block_dir_color")).is_equal_approx(noon.dir_color),
			"the exiting writer leaves the noon directional color behind")
	assert_false(bool(RenderingServer.global_shader_parameter_get("opennova_fog_enabled")),
			"and clears the object fog enable")
	# Re-entering republishes the live block even though its generation did
	# not move (the exit forgot the published generation).
	add_child(env_node)
	assert_true(Vector3(RenderingServer.global_shader_parameter_get(
			"opennova_light_block_dir_color")).is_equal_approx(world_values.dir_color),
			"a re-entered environment writes its block again")
	assert_true(bool(RenderingServer.global_shader_parameter_get("opennova_fog_enabled")),
			"and re-enables the object fog")


func test_weather_publishes_the_active_moon_direction_at_night() -> void:
	var env_node := MissionEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 2200.0

	var weather := Weather.new()
	weather.environment_path = env_node.get_path()
	add_child_autofree(weather)
	weather.advance_frame(0.016)
	# The weather writeback publishes the ACTIVE light direction (the engine
	# build_weather_shader_globals pins sun_direction <- light_direction; the
	# environment_state ctest covers that seam): at night that is the moon,
	# never the solar highlight vector.
	assert_true(env_node.is_night_phase(), "22:00 reads as night")
	assert_true(env_node.get_light_direction().is_equal_approx(
			env_node.get_moon_direction()),
			"terrain and foliage globals follow Environment_GetLightDirectionFloat")
	assert_false(env_node.get_light_direction().is_equal_approx(
			env_node.get_sun_direction()),
			"night shader globals must not stay pinned to the solar highlight vector")


func test_day_phase_selects_night_and_day() -> void:
	var env := _new_default_env()
	var noon: Dictionary = env.get_day_phase(1200.0)
	assert_false(bool(noon["is_night"]), "Noon should be day.")
	assert_almost_eq(float(noon["blend"]), 1.0, 0.01, "Noon should be fully blended into day.")
	assert_true(bool(env.get_day_phase(0.0)["is_night"]), "Midnight should be night.")
	assert_true(bool(env.get_day_phase(1845.0)["is_night"]), "18:45 is the sunset switch into night.")


func test_double_saturate_and_lit_water_helpers() -> void:
	var doubled := EnvFile.double_saturate_color(Color(0.5, 0.25, 1.0))
	assert_almost_eq(doubled.r, 1.0, 0.01, "0.5 doubles to 1.0.")
	assert_almost_eq(doubled.g, 0.5, 0.01, "0.25 doubles to 0.5.")
	assert_almost_eq(doubled.b, 1.0, 0.01, "1.0 saturates at 1.0.")
	var mid := Color(0.5, 0.5, 0.5)
	var lit := EnvFile.lit_water_color(mid, mid)
	assert_almost_eq(lit.r, 0.5, 0.02, "water*light>>7 is identity at mid-gray.")


func test_field_consumption_table_mirrors_the_matrix() -> void:
	var table := EnvFile.get_field_consumption()
	assert_false(table.is_empty(), "the consumption table is populated")

	var terrain: Dictionary = table.get("terrain_tint", {})
	assert_eq(String(terrain.get("status", "")), "partial", "terrain_tint is partial (divergence #19)")
	assert_string_contains(String(terrain.get("anchor", "")), "0x60b8cb", "anchored to the bake consumer")
	assert_false(String(terrain.get("note", "")).is_empty(), "deferred rows explain themselves")

	# The modulator chain landed at REN-5 (divergence #17 FIXED) — the outdoor
	# exposure runs; the row keeps its interior-sampling caveat as the note
	# (docs/render/render-lighting-re.md D-RLIT-2).
	assert_eq(String((table.get("iris_percent", {}) as Dictionary).get("status", "")), "honored",
		"iris is honored since REN-5 (the modulator chain is live, divergence #17)")
	assert_false(String((table.get("iris_percent", {}) as Dictionary).get("note", "")).is_empty(),
		"the iris row keeps its interior-sampling caveat")
	assert_true(bool((table.get("vertex_tint", {}) as Dictionary).get("faithful", false)),
		"vertex_tint is faithfully unconsumed (retail ignores it too)")

	var cloud: Dictionary = table.get("cloud_tint", {})
	assert_eq(String(cloud.get("status", "")), "honored", "cloud_tint is honored after the C7 flat pass")
	assert_false(String(cloud.get("note", "")).is_empty(), "honored-with-scope rows keep their caveat")

	assert_eq(String((table.get("fog_level", {}) as Dictionary).get("status", "")), "honored",
		"plainly honored fields are in the table too")


func test_weather_releases_a_freed_simulation_before_the_environment_reads_it() -> void:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env_node := MissionEnvironment.new()
	env_node.name = "Env"
	env_node.environment_data = _load_full_00()
	mount.add_child(env_node)
	var weather := Weather.new()
	weather.environment_path = NodePath("../Env")
	mount.add_child(weather)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.weather_state_bound())
	weather.bind_simulation(sim)
	# GameWorld.unload frees the runtime and the off-tree sim with it: the
	# environment must stop viewing the dead World's WeatherState before its
	# next reload reads the view (the mission-lifecycle segfault).
	sim.free()
	env_node.environment_data = _load_full_00()
	env_node.set_time_of_day(12.0)
	# GameWorld re-prepares the standalone home on the world-only load.
	weather.prepare_autonomous()
	weather.command_fog_distance(640)
	for _i in range(1024):
		weather.tick_fixed()
	assert_almost_eq(env_node.get_fog_level(), 640.0, 0.001,
			"after the release the weather ticks the environment's standalone home")
