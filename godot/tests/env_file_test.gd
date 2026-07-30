extends GutTest

# The env is not bundled with the project; it loads from an external directory
# (the resource dir at runtime). The test fixture lives in repo-root fixtures/.
const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"


class DirectionCaptureWeather extends NovaWeather:
	var published_direction := Vector3.ZERO

	func _publish_light_direction(direction: Vector3) -> void:
		published_direction = direction


func _load_full_00() -> EnvFile:
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	return env


func test_full_00_env_loads_and_parses() -> void:
	var env := _load_full_00()

	assert_not_null(env, "FULL_00 should load as the shared EnvFile resource.")
	if env == null:
		return
	assert_true(env.is_loaded(), "Loaded EnvFile should report a valid document.")
	assert_eq(env.get_env_name(), "Full_00", "Stock enviro_name should round-trip into EnvFile.")
	assert_eq(env.get_timeofday(), "Day", "Stock timeofday should be parsed.")
	assert_eq(env.get_curtime(), 1200, "Stock curtime should be parsed.")
	assert_eq(env.get_fog_level(), 1000.0, "Stock fog level should be parsed.")
	assert_eq(env.get_fog_type(), 2, "Stock fog type should be parsed.")
	assert_false(env.has_water_height(), "Stock fixture should not claim an authored water height.")
	assert_eq(env.get_advanced_clouds(), 1, "Stock advanced cloud mode should be parsed.")
	assert_eq(env.get_tod_keyframes().size(), 10, "FULL_00 should expose all TOD keyframes.")
	assert_not_null(env.get_sky_map1_tex(), "Sky map 1 should resolve through the shared texture resolver.")
	assert_not_null(env.get_sky_map2_tex(), "Sky map 2 should resolve through the shared texture resolver.")


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

	env.apply_mission_overrides({
		"fog_level": 250.0,
		"water_color": Color(0.1, 0.2, 0.3),
		"water_murk": 0.4,
	})
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
	var env_node := NovaEnvironment.new()
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
	var env_node := NovaEnvironment.new()
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
	var env_node := NovaEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.set_weather_driven(true)

	var generation_before := env_node.get_env_generation()
	env_node.set_smoothed_scalars(640.0, env_node.get_sky_height_target(), 0.0)
	assert_almost_eq(env_node.get_fog_level(), 640.0, 0.001,
			"weather consumers read the smoothed fog end")
	assert_almost_eq(env_node.get_fog_start(), 320.0, 0.001,
			"type-2 fog start follows half of that same smoothed end")
	assert_gt(env_node.get_env_generation(), generation_before,
			"moving fog bounds invalidate cached object and clear-state consumers")

	var fine_step_generation := env_node.get_env_generation()
	env_node.set_smoothed_scalars(640.001, env_node.get_sky_height_target(), 0.0)
	assert_gt(env_node.get_env_generation(), fine_step_generation,
			"sub-epsilon fixed spring steps still invalidate renderer consumers")

	var settled_generation := env_node.get_env_generation()
	env_node.set_smoothed_scalars(640.001, env_node.get_sky_height_target(), 0.0)
	assert_eq(env_node.get_env_generation(), settled_generation,
			"a settled fog spring does not churn renderer generations")


func test_object_lighting_uses_the_active_moon_direction_at_night() -> void:
	var env_node := NovaEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 2200.0

	var values = NovaObjectModel.environment_values_from(env_node)
	var expected := -env_node.get_light_direction().normalized()
	assert_true(values.dir.is_equal_approx(expected),
			"object directional light follows Environment_GetLightDirectionFloat")
	assert_false(values.dir.is_equal_approx(-env_node.get_sun_direction().normalized()),
			"night objects must not stay pinned to the solar highlight vector")


func test_entity_lighting_applies_sun_visibility_and_interior_light_transfer() -> void:
	var env_node := NovaEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 1500.0
	var world_values = NovaObjectModel.environment_values_from(env_node)

	assert_true(world_values.floor.is_equal_approx(env_node.get_floor_color()))
	assert_true(world_values.ceiling.is_equal_approx(env_node.get_ceiling_color()))

	var covered = NovaObjectModel.entity_lighting_values(
			world_values, 0.25, false, 0.0)
	assert_true(covered.dir_color.is_equal_approx(world_values.dir_color * 0.25),
			"three blocked retail rays leave one quarter directional light")
	assert_true(covered.hemi_ground.is_equal_approx(world_values.hemi_ground),
			"sun visibility does not dim the outdoor hemisphere")
	assert_true(covered.hemi_sky.is_equal_approx(world_values.hemi_sky))

	var interior = NovaObjectModel.entity_lighting_values(
			world_values, 1.0, true, 0.2)
	assert_true(interior.dir_color.is_equal_approx(world_values.dir_color * 0.2),
			"Ihq01 light_transfer 20 leaves twenty percent directional light")
	assert_true(interior.hemi_ground.is_equal_approx(
			world_values.floor.lerp(world_values.hemi_ground, 0.2)))
	assert_true(interior.hemi_sky.is_equal_approx(
			world_values.ceiling.lerp(world_values.hemi_sky, 0.2)))


func test_weather_publishes_the_active_moon_direction_at_night() -> void:
	var env_node := NovaEnvironment.new()
	add_child_autofree(env_node)
	env_node.environment_data = _load_full_00()
	env_node.time_of_day = 2200.0

	var weather := DirectionCaptureWeather.new()
	weather.environment_path = env_node.get_path()
	add_child_autofree(weather)
	simulate(weather, 1, 0.016)
	assert_true(weather.published_direction.is_equal_approx(env_node.get_light_direction()),
			"terrain and foliage globals follow Environment_GetLightDirectionFloat")
	assert_false(weather.published_direction.is_equal_approx(env_node.get_sun_direction()),
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
