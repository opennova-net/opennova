extends GutTest

# The env is not bundled with the project; it loads from an external directory
# (the resource dir at runtime). The test fixture lives in repo-root fixtures/.
const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"


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
	assert_true(absf(env.compute_sun_direction(1200.0).length() - 1.0) < 0.01, "Sun direction should be normalized.")

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


func test_fog_start_follows_engine_policy() -> void:
	var env := _new_default_env()
	env.set_fog_level(1000.0)
	env.set_fog_type(1)
	assert_almost_eq(env.get_fog_start(), 0.5, 0.01, "fog_type 1 starts at the 0.5 unit constant.")
	env.set_fog_type(2)
	assert_almost_eq(env.get_fog_start(), 500.0, 0.5, "fog_type 2 starts at half the end distance.")
	env.set_fog_type(3)
	assert_almost_eq(env.get_fog_start(), 250.0, 0.5, "fog_type 3 starts at quarter the end distance.")


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
