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
