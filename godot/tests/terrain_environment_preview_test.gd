extends GutTest

# Editor<->runtime parity (deferred from PR #24): the terrain preview drives the
# same NovaWater + NovaWeather stack as the runtime. These lock the API the
# terrain editor relies on so the bespoke hardcoded water plane cannot return.

const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const NovaWeatherScript = preload("res://engine/environment/nova_weather.gd")


func test_water_height_override_drives_height() -> void:
	var water = add_child_autofree(NovaWaterScript.new())
	water.set_height_override(42.5)
	assert_almost_eq(water.water_height, 42.5, 0.001, "Height override should drive the water height directly.")

	# Clearing the override (NaN) hands control back to the env/terrain fallback.
	water.set_height_override(NAN)
	water.water_height = 7.0
	assert_almost_eq(water.water_height, 7.0, 0.001, "After clearing the override the owner can set height freely.")


func test_weather_exposes_resync_for_discrete_scrubs() -> void:
	var weather = add_child_autofree(NovaWeatherScript.new())
	assert_true(weather.has_method("resync_colors"), "NovaWeather must expose resync_colors for TOD scrubs.")
	# Calling it before any env is bound must be safe (editor calls it on every edit).
	weather.resync_colors()
	assert_true(weather.has_method("get_smooth_fog"), "Smoothed color getters back the editor terrain push.")


func test_water_lit_color_is_engine_derived_not_hardcoded() -> void:
	# The editor preview no longer hardcodes Color(0.13, 0.34, 0.55); water color
	# is EnvFile.lit_water_color(water, light*0.707 + sky). Spot-check the path
	# produces a plausible, non-constant result that varies with lighting.
	var water_rgb := Color(0.4, 0.31, 0.22)
	var dim := EnvFile.lit_water_color(water_rgb, EnvFile.combine_terrain_light(Color(0.2, 0.2, 0.2), Color(0.1, 0.1, 0.1)))
	var bright := EnvFile.lit_water_color(water_rgb, EnvFile.combine_terrain_light(Color(1.0, 1.0, 1.0), Color(0.4, 0.4, 0.4)))
	assert_lt(dim.r, bright.r, "Lit water color should track lighting, not a fixed constant.")
