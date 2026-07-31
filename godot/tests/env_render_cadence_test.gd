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
