extends GutTest

# The environment weather routine is a fixed 62 Hz engine tick. Rendering is
# intentionally decoupled: equal elapsed time must produce identical cloud,
# wind, and water-scroll state at any display refresh rate. Water's generated
# noise textures are excluded because retail regenerates those once per
# rendered water frame.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"


func _loaded_env() -> EnvFile:
	var data := EnvFile.new()
	data.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	assert_eq(data.load(), OK)
	return data


func _advance_one_second(node: Node, hz: int) -> void:
	simulate(node, hz, 1.0 / float(hz))


func _weather_state_after_one_second(hz: int) -> Array:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env := NovaEnvironment.new()
	env.name = "Env"
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var weather := NovaWeather.new()
	weather.environment_path = NodePath("../Env")
	mount.add_child(weather)
	_advance_one_second(weather, hz)
	return [
		weather.get_cloud_uv_offset1(0.0, 0.0),
		weather.get_cloud_uv_offset2(0.0, 0.0),
		weather.get_water_uv_state(0.0, 0.0, env.get_fog_level()),
		weather.get_sway_amount(),
		weather.get_sway_phase(),
	]


func test_weather_state_is_invariant_across_render_refresh_rates() -> void:
	var expected := _weather_state_after_one_second(62)
	assert_eq(_weather_state_after_one_second(30), expected)
	assert_eq(_weather_state_after_one_second(144), expected)


func _world_driven_weather_fixture() -> Array:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env := NovaEnvironment.new()
	env.name = 'Env'
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var weather := NovaWeather.new()
	weather.environment_path = NodePath('../Env')
	mount.add_child(weather)
	weather.prepare_world_driven()
	return [env, weather]


func _world_driven_weather_state(weather: NovaWeather, env: NovaEnvironment) -> Array:
	return [
		weather.get_cloud_uv_offset1(0.0, 0.0),
		weather.get_cloud_uv_offset2(0.0, 0.0),
		weather.get_water_uv_state(0.0, 0.0, env.get_fog_level()),
		weather.get_sway_amount(),
		weather.get_sway_phase(),
		weather.get_lightning_intensity(),
		weather.get_smooth_fill(),
		weather.get_smooth_sun(),
		weather.get_smooth_fog(),
		weather.get_smooth_sky(),
		weather.get_smooth_skyfog(),
		weather.get_smooth_ceiling(),
		weather.get_smooth_cloud(),
		weather.get_smooth_floor(),
		weather.get_smooth_sky_base(),
		weather.get_smooth_sky_bright(),
		weather.get_smooth_sky_highlight(),
		weather.get_smooth_cloud_base(),
		weather.get_smooth_cloud_highlight(),
		weather.get_smooth_cloud_edge(),
	]


func test_world_driven_mission_restart_reseeds_complete_weather_state() -> void:
	var reused_fixture := _world_driven_weather_fixture()
	var reused_env := reused_fixture[0] as NovaEnvironment
	var reused := reused_fixture[1] as NovaWeather
	reused.trigger_lightning_long()
	for _tick in range(47):
		reused.tick_fixed()
	assert_ne(reused.get_cloud_uv_offset1(0.0, 0.0), Vector2.ZERO)

	reused.prepare_world_driven()
	var fresh_fixture := _world_driven_weather_fixture()
	var fresh_env := fresh_fixture[0] as NovaEnvironment
	var fresh := fresh_fixture[1] as NovaWeather
	assert_eq(_world_driven_weather_state(reused, reused_env),
			_world_driven_weather_state(fresh, fresh_env))

	for _tick in range(64):
		reused.tick_fixed()
		fresh.tick_fixed()
	assert_eq(_world_driven_weather_state(reused, reused_env),
			_world_driven_weather_state(fresh, fresh_env))


func test_mission_start_prewarm_advances_exactly_255_weather_ticks() -> void:
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as NovaEnvironment
	var weather := fixture[1] as NovaWeather
	env.configure_mission_clock(0x0540, 60)
	var expected := NovaEnvironment.new()
	expected.configure_mission_clock(0x0540, 60)
	expected.advance_mission_clock(NovaWeather.MISSION_START_PREWARM_TICKS)

	weather.prewarm_mission_start()

	assert_almost_eq(env.time_of_day, expected.time_of_day, 0.000001)
	expected.free()


func test_network_environment_sample_roundtrips_exact_retail_units_through_live_owner() -> void:
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as NovaEnvironment
	var weather := fixture[1] as NovaWeather
	var wire_sample := {
		"fog_dist": 380,
		"fog_accel": 0xFF00,
		"tod_fixed": 0x3088,
		"quake_ticks": 17,
		"cloud_scroll": 15,
		"rain_pct": 0x56,
		"overcast": 0x78,
		"precipitation_kind": 0x9A,
	}

	weather.apply_network_environment_sample(wire_sample)
	var native: Dictionary = weather.get_network_environment_snapshot()
	assert_eq(int(native.get("fog_target_q16", -1)), 380 << 16)
	assert_eq(int(native.get("fog_current_q16", -1)), 1000 << 16,
			"the client preserves its fog current when the network target changes")
	assert_eq(int(native.get("fog_accel_clamp", -1)), 0x00FF0000)
	assert_eq(int(native.get("tod_fixed24", -1)), 0x3088 << 13)
	assert_eq(int(native.get("quake_ticks", -1)), 17)
	assert_eq(int(native.get("cloud_scroll_rate_target", -1)), 15 << 10)
	assert_eq(int(native.get("rain_pct_current_q16", -1)), 0)
	assert_eq(int(native.get("overcast_blend_q16", -1)), 0)
	assert_eq(int(native.get("precipitation_kind", -1)), 0x9A)
	assert_eq(env.get_mission_time_fixed24(), 0x3088 << 13,
			"the received clock, not the local authored clock, owns TOD targets")
	assert_eq(env.get_fog_level_target(), 380.0,
			"the received fog target reaches the live render owner")
	assert_eq(env.get_sky_speed(), 15.0,
			"the received cloud target drives the live cloud-scroll owner")
	assert_eq(env.get_overcast_blend(), 0.0,
			"the received overcast byte is a target, not an immediate current snap")

	weather.tick_fixed()
	native = weather.get_network_environment_snapshot()
	assert_eq(int(native.get("fog_current_q16", -1)), 0x03D4A000)
	assert_eq(int(native.get("rain_pct_current_q16", -1)), 0x000002B0)
	assert_eq(int(native.get("overcast_blend_q16", -1)), 0x000003C0)
	assert_almost_eq(env.get_network_rain_current(), 0x02B0 / 65536.0, 0.000001)
	assert_almost_eq(env.get_overcast_blend(), 0x03C0 / 65536.0, 0.000001,
			"fog/celestial consumers see the locally smoothed overcast current")

	env.advance_mission_clock(1)
	assert_eq(env.get_network_quake_ticks(), 16,
			"joiner quake duration counts down once per 62 Hz environment tick")
	env.advance_mission_clock(100)
	assert_eq(env.get_network_quake_ticks(), 0)
	weather.apply_network_environment_sample(wire_sample)
	assert_eq(env.get_network_quake_ticks(), 17,
			"a later authoritative phase-2 sample replaces the local countdown")

	# Applying the same wire state is still safe, but a discrete replacement ENV
	# must clear the remote-owner overrides so the next mission starts from its
	# own authored fog/cloud values.
	env.environment_data = _loaded_env()
	assert_eq(env.get_fog_level_target(), 1000.0)
	assert_eq(env.get_sky_speed(), 15.0)
	assert_eq(env.get_overcast_blend(), 0.0)


func _sky_fallback_state_after_one_second(hz: int) -> Array:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env := NovaEnvironment.new()
	env.name = "Env"
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var sky := NovaSky.new()
	sky.environment_path = NodePath("../Env")
	mount.add_child(sky)
	_advance_one_second(sky, hz)
	return [
		sky.sky_material.get_shader_parameter("u_scroll_offset1"),
		sky.sky_material.get_shader_parameter("u_scroll_offset2"),
	]


func test_standalone_sky_scroll_is_invariant_across_render_refresh_rates() -> void:
	var expected := _sky_fallback_state_after_one_second(62)
	assert_eq(_sky_fallback_state_after_one_second(30), expected)
	assert_eq(_sky_fallback_state_after_one_second(144), expected)


func _water_fallback_state_after_one_second(hz: int) -> Vector4:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(160, 90)
	add_child_autofree(viewport)
	var env_data := _loaded_env()
	env_data.set_water_height(14.0)
	var env := NovaEnvironment.new()
	env.name = "Env"
	env.environment_data = env_data
	viewport.add_child(env)
	var water := NovaWater.new()
	water.environment_path = NodePath("../Env")
	viewport.add_child(water)
	var camera := Camera3D.new()
	camera.position = Vector3(20.0, 27.0, -30.0)
	viewport.add_child(camera)
	camera.make_current()
	_advance_one_second(water, hz)
	return water.water_material.get_shader_parameter("u_water_uv")


func test_standalone_water_scroll_is_invariant_across_render_refresh_rates() -> void:
	var expected := _water_fallback_state_after_one_second(62)
	assert_eq(_water_fallback_state_after_one_second(30), expected)
	assert_eq(_water_fallback_state_after_one_second(144), expected)
