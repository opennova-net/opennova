extends GutTest

# The environment weather routine is a fixed 62 Hz engine tick. Rendering is
# intentionally decoupled: equal elapsed time must produce identical cloud,
# wind, and water-scroll state at any display refresh rate. Water's generated
# noise textures are excluded because retail regenerates those once per
# rendered water frame.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"


func _loaded_env() -> EnvFile:
	var data := EnvFile.new()
	data.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	assert_eq(data.load(), OK)
	return data


func _advance_one_second(node: Node, hz: int) -> void:
	var delta := 1.0 / float(hz)
	# Native weather nodes expose advance_frame (a GDExtension _process
	# virtual is not externally callable); script nodes keep simulate().
	if node is Weather:
		for _i in hz:
			(node as Weather).advance_frame(delta)
	elif node is SkyDome:
		for _i in hz:
			(node as SkyDome).advance_frame(delta)
	elif node is Water:
		for _i in hz:
			(node as Water).advance_frame(delta)
	else:
		simulate(node, hz, delta)


func _weather_state_after_one_second(hz: int) -> Array:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env := MissionEnvironment.new()
	env.name = "Env"
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var weather := Weather.new()
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
	var env := MissionEnvironment.new()
	env.name = 'Env'
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var weather := Weather.new()
	weather.environment_path = NodePath('../Env')
	mount.add_child(weather)
	weather.prepare_world_driven()
	return [env, weather]


func _world_driven_weather_state(weather: Weather, env: MissionEnvironment) -> Array:
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
	var reused_env := reused_fixture[0] as MissionEnvironment
	var reused := reused_fixture[1] as Weather
	reused.trigger_lightning_long()
	for _tick in range(47):
		reused.tick_fixed()
	assert_ne(reused.get_cloud_uv_offset1(0.0, 0.0), Vector2.ZERO)

	reused.prepare_world_driven()
	var fresh_fixture := _world_driven_weather_fixture()
	var fresh_env := fresh_fixture[0] as MissionEnvironment
	var fresh := fresh_fixture[1] as Weather
	assert_eq(_world_driven_weather_state(reused, reused_env),
			_world_driven_weather_state(fresh, fresh_env))

	for _tick in range(64):
		reused.tick_fixed()
		fresh.tick_fixed()
	assert_eq(_world_driven_weather_state(reused, reused_env),
			_world_driven_weather_state(fresh, fresh_env))


func test_frozen_fixture_exposure_settle_publishes_a_non_identity_gain() -> void:
	# The capture-refresh seam (the D-RLIT-2 fixture starvation): a frozen
	# fixture never runs the weather tick, so the modulator chain holds its
	# mission-reset identity snap and the published exposure gain stays flat.
	# settle_exposure must chase the stamped iris target to its fixed point
	# [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538] while
	# leaving every time-owning weather leg (cloud scroll, wind, the mission
	# clock) untouched.
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as MissionEnvironment
	var weather := fixture[1] as Weather
	env.configure_mission_clock(0x0540, 60)
	assert_eq(env.get_color_src_gain(), Vector3.ONE,
			"the frozen fixture starts at the mission-reset identity gain")

	var scroll_before: Vector2 = weather.get_cloud_uv_offset1(0.0, 0.0)
	var sway_phase_before: float = weather.get_sway_phase()
	var clock_before: int = env.get_mission_time_fixed24()
	# Indoor-no-data samples pin the curve's 255 clamp — a target that can
	# never alias the identity 64, whatever the fixture's TOD colors serve
	# [orig: the pool_entry[12] == 0 skip @ 0x5c7652].
	weather.iris_samples = PackedInt32Array([-2, -2, -2])
	weather.settle_exposure()

	var gain: Vector3 = env.get_color_src_gain()
	assert_ne(gain, Vector3.ONE,
			"the settled fixture publishes the chased iris gain, not identity")
	assert_almost_eq(gain.x, gain.y, 1.5 / 64.0)
	assert_almost_eq(gain.x, gain.z, 1.5 / 64.0)
	assert_eq(weather.get_cloud_uv_offset1(0.0, 0.0), scroll_before,
			"the settle may not advance the cloud-scroll accumulators")
	assert_eq(weather.get_sway_phase(), sway_phase_before,
			"the settle may not advance the wind oscillator")
	assert_eq(env.get_mission_time_fixed24(), clock_before,
			"the settle may not advance the mission clock")


func test_sun_veil_stopdown_dims_and_releases_the_published_gain() -> void:
	# Modulator-2's only witnessed target writer [orig:
	# Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0]: stop-down 40
	# chases modulator-2 to bytes 4 over 8 ticks (the whole published gain
	# stops down through the chain), and release returns it to identity over
	# 124 ticks.
	# The control fixture runs the identical tick count with no stop-down:
	# the modulator itself legitimately chases the iris target during the
	# run, so "released" means "matches a fixture that never stopped down",
	# not "matches the pre-run snapshot".
	var control_fixture := _world_driven_weather_fixture()
	var control_env := control_fixture[0] as MissionEnvironment
	var control := control_fixture[1] as Weather
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as MissionEnvironment
	var weather := fixture[1] as Weather

	# The block chase is exponential (delta = dist >> 3, rate-clamped), so
	# landing takes ~35 ticks; retail rewrites the target every frame.
	for _tick in range(64):
		weather.set_sun_veil_stopdown(40)
		weather.tick_fixed()
		control.tick_fixed()
	var stopped: Vector3 = env.get_color_src_gain()
	assert_lt(stopped.x, control_env.get_color_src_gain().x * 0.2,
			"a full stop-down collapses the published gain through modulator-2")

	# Release is the slow 124-tick asymptotic recovery (retail rewrites the
	# identity target every frame while not staring at the sun).
	for _tick in range(700):
		weather.set_sun_veil_stopdown(0)
		weather.tick_fixed()
		control.tick_fixed()
	assert_almost_eq(env.get_color_src_gain().x,
			control_env.get_color_src_gain().x, 2.0 / 64.0,
			"the release chase restores the un-stopped gain")


func test_mission_start_prewarm_advances_exactly_255_weather_ticks() -> void:
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as MissionEnvironment
	var weather := fixture[1] as Weather
	env.configure_mission_clock(0x0540, 60)
	var expected := MissionEnvironment.new()
	expected.configure_mission_clock(0x0540, 60)
	expected.advance_mission_clock(Weather.MISSION_START_PREWARM_TICKS)

	weather.prewarm_mission_start()

	assert_almost_eq(env.time_of_day, expected.time_of_day, 0.000001)
	expected.free()


func test_network_environment_sample_roundtrips_exact_retail_units_through_live_owner() -> void:
	var fixture := _world_driven_weather_fixture()
	var env := fixture[0] as MissionEnvironment
	var weather := fixture[1] as Weather
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
	assert_eq(env.get_sky_speed(), 12.0, "synth_full.env authors sky_speed 12")
	assert_eq(env.get_overcast_blend(), 0.0)


func _sky_fallback_state_after_one_second(hz: int) -> Array:
	var mount := Node3D.new()
	add_child_autofree(mount)
	var env := MissionEnvironment.new()
	env.name = "Env"
	env.environment_data = _loaded_env()
	mount.add_child(env)
	var sky := SkyDome.new()
	sky.environment_path = NodePath("../Env")
	mount.add_child(sky)
	_advance_one_second(sky, hz)
	return [
		sky.get_sky_material().get_shader_parameter("u_scroll_offset1"),
		sky.get_sky_material().get_shader_parameter("u_scroll_offset2"),
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
	var env := MissionEnvironment.new()
	env.name = "Env"
	env.environment_data = env_data
	viewport.add_child(env)
	var water := Water.new()
	water.environment_path = NodePath("../Env")
	viewport.add_child(water)
	var camera := Camera3D.new()
	camera.position = Vector3(20.0, 27.0, -30.0)
	viewport.add_child(camera)
	camera.make_current()
	_advance_one_second(water, hz)
	return water.get_water_material().get_shader_parameter("u_water_uv")


func test_standalone_water_scroll_is_invariant_across_render_refresh_rates() -> void:
	var expected := _water_fallback_state_after_one_second(62)
	assert_eq(_water_fallback_state_after_one_second(30), expected)
	assert_eq(_water_fallback_state_after_one_second(144), expected)
