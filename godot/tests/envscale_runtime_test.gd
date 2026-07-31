extends GutTest

# Global-color EnvScale coverage at the runtime boundary. EnvFile deliberately
# retains raw authored colors for export; NovaEnvironment exposes the parser-
# equivalent scaled bytes to render/weather consumers.


static func _units(value: Vector3) -> Array:
	return [
		int(value.x * 255.0 + 0.5),
		int(value.y * 255.0 + 0.5),
		int(value.z * 255.0 + 0.5),
	]


static func _color_units(value: Color) -> Array:
	return _units(Vector3(value.r, value.g, value.b))


func _make_static_data() -> EnvFile:
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_envscale(0.5)
	data.set_ceiling_color(Color8(71, 61, 51))
	data.set_cloud_tint(Color8(91, 121, 151))
	data.set_floor_color(Color8(31, 21, 11))
	data.set_water_color(Color8(21, 81, 121))
	data.set_lightning_color(Color8(201, 101, 51))
	return data


func _add_environment(data: EnvFile, node_name: String) -> NovaEnvironment:
	var env := NovaEnvironment.new()
	env.name = node_name
	env.environment_data = data
	add_child_autofree(env)
	return env


func _add_world_driven_weather(env: NovaEnvironment, node_name: String) -> NovaWeather:
	var weather := NovaWeather.new()
	weather.name = node_name
	weather.environment_path = env.get_path()
	add_child_autofree(weather)
	weather.prepare_world_driven()
	return weather


func test_global_colors_scale_at_runtime_without_mutating_authored_values() -> void:
	var data := _make_static_data()
	var env := _add_environment(data, "ScaledStaticEnv")

	# Odd source bytes pin truncation after the multiply (71 * .5 -> 35),
	# rather than a float-only render multiplier or round-to-nearest.
	assert_eq(_units(env.get_ceiling_color_target()), [35, 30, 25])
	assert_eq(_units(env.get_cloud_tint_target()), [45, 60, 75])
	assert_eq(_units(env.get_floor_color_target()), [15, 10, 5])
	assert_eq(_units(env.get_water_color()), [10, 40, 60])
	assert_eq(_units(env.get_lightning_color_target()), [100, 50, 25])

	# Standalone current colors are seeded from those same engine targets.
	assert_eq(_units(env.get_ceiling_color()), [35, 30, 25])
	assert_eq(_units(env.get_cloud_tint()), [45, 60, 75])
	assert_eq(_units(env.get_floor_color()), [15, 10, 5])

	# The authoring/export surface remains raw, preserving exact round-trips.
	assert_eq(_color_units(data.get_ceiling_color()), [71, 61, 51])
	assert_eq(_color_units(data.get_cloud_tint()), [91, 121, 151])
	assert_eq(_color_units(data.get_floor_color()), [31, 21, 11])
	assert_eq(_color_units(data.get_water_color()), [21, 81, 121])
	assert_eq(_color_units(data.get_lightning_color()), [201, 101, 51])


func test_world_driven_static_blocks_snap_to_scaled_targets() -> void:
	var env := _add_environment(_make_static_data(), "WorldDrivenStaticEnv")
	var weather := _add_world_driven_weather(env, "WorldDrivenStaticWeather")

	assert_eq(_units(weather.get_smooth_ceiling()), [35, 30, 25])
	assert_eq(_units(weather.get_smooth_cloud()), [45, 60, 75])
	assert_eq(_units(weather.get_smooth_floor()), [15, 10, 5])
	assert_eq(env.get_ceiling_color(), weather.get_smooth_ceiling())
	assert_eq(env.get_cloud_tint(), weather.get_smooth_cloud())
	assert_eq(env.get_floor_color(), weather.get_smooth_floor())


func _make_black_lightning_data(envscale: float, lightning: Color) -> EnvFile:
	var data := EnvFile.new()
	data.reset_to_default()
	data.set_envscale(envscale)
	data.set_lightning_color(lightning)
	var black := Color8(0, 0, 0)
	data.set_ceiling_color(black)
	data.set_cloud_tint(black)
	data.set_floor_color(black)
	for keyframe in data.get_tod_keyframes():
		keyframe.set_sun_color(black)
		keyframe.set_ground_color(black)
		keyframe.set_fog_color(black)
		keyframe.set_sky_color(black)
		keyframe.set_moon_color(black)
		keyframe.set_skyfog_color(black)
		keyframe.set_skybase_color(black)
		keyframe.set_skybright_color(black)
		keyframe.set_skyhighlight_color(black)
		keyframe.set_cloudbase_color(black)
		keyframe.set_cloudhighlight_color(black)
		keyframe.set_cloudedge_color(black)
	return data


static func _flash_snapshot(weather: NovaWeather) -> Array:
	return [
		_units(weather.get_smooth_sky()),
		_units(weather.get_smooth_fog()),
		_units(weather.get_smooth_fill()),
		_units(weather.get_smooth_skyfog()),
	]


func test_lightning_consumes_the_scaled_runtime_color() -> void:
	# These describe the same engine byte color: trunc(Color8(201,101,51) *
	# .5) == Color8(100,50,25). If NovaWeather bypasses the EnvScale view and
	# reads the raw EnvFile color, their first flash epoch diverges.
	var scaled_env := _add_environment(
		_make_black_lightning_data(0.5, Color8(201, 101, 51)),
		"ScaledLightningEnv")
	var reference_env := _add_environment(
		_make_black_lightning_data(1.0, Color8(100, 50, 25)),
		"ReferenceLightningEnv")
	var scaled_weather := _add_world_driven_weather(scaled_env, "ScaledLightningWeather")
	var reference_weather := _add_world_driven_weather(reference_env, "ReferenceLightningWeather")

	scaled_weather.trigger_lightning_short()
	reference_weather.trigger_lightning_short()
	for _tick in 6: # timer 16 -> first witnessed epoch at remaining tick 10
		scaled_weather.tick_fixed()
		reference_weather.tick_fixed()

	assert_gt(scaled_weather.get_lightning_intensity(), 0.0,
		"the comparison must sample a live flash epoch")
	assert_ne(_units(scaled_weather.get_smooth_sky()), [0, 0, 0],
		"the flash must reach a rendered color block")
	assert_eq(_flash_snapshot(scaled_weather), _flash_snapshot(reference_weather),
		"EnvScale must reach every lightning additive consumer")
