extends GutTest

# DebugEnvironmentPage: env/weather/water readouts from duck-typed stubs, the
# scrub knobs poking the live nodes directly and mirroring them back, and the
# empty states without a world.

const PageScript := preload("res://engine/debug/pages/debug_environment_page.gd")


class StubEnv:
	extends Node
	var time_of_day := 1830.0

	func is_night_phase() -> bool:
		return true

	func get_day_phase_blend() -> float:
		return 0.25

	func is_weather_driven() -> bool:
		return true

	func get_fog_start() -> float:
		return 40.0

	func get_fog_distance() -> float:
		return 320.0

	func get_fog_type() -> int:
		return 2

	func get_fog_level() -> float:
		return 0.6

	func get_sun_direction() -> Vector3:
		return Vector3(0.1, -0.9, 0.2)


class StubWeather:
	extends Node
	var wind_strength := 55.0
	var short_strikes := 0
	var long_strikes := 0

	func trigger_lightning_short() -> void:
		short_strikes += 1

	func trigger_lightning_long() -> void:
		long_strikes += 1


class StubWater:
	extends Node
	var water_height := 12.5

	func is_water_active() -> bool:
		return true

	func is_water_render_active() -> bool:
		return true


class StubWorld:
	extends Node
	var env: Node = null
	var weather: Node = null
	var water: Node = null

	func get_environment_node() -> Node:
		return env

	func get_weather_node() -> Node:
		return weather

	func get_water_node() -> Node:
		return water


func _make_world() -> StubWorld:
	var world := StubWorld.new()
	world.env = StubEnv.new()
	world.weather = StubWeather.new()
	world.water = StubWater.new()
	world.add_child(world.env)
	world.add_child(world.weather)
	world.add_child(world.water)
	add_child_autofree(world)
	return world


func _make_page(world: Node = null) -> DebugEnvironmentPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	if world != null:
		ctx.world_source = func(): return world
	var page: DebugEnvironmentPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func test_renders_empty_states_without_a_world() -> void:
	var page := _make_page()
	page.refresh()
	assert_string_contains((page.find_child("EnvState", true, false) as Label).text,
			"No environment")
	assert_string_contains((page.find_child("WaterState", true, false) as Label).text,
			"No water")


func test_formats_the_environment_and_mirrors_knobs() -> void:
	var world := _make_world()
	var page := _make_page(world)
	page.refresh()

	var env_text := (page.find_child("EnvState", true, false) as Label).text
	assert_string_contains(env_text, "Time 1830")
	assert_string_contains(env_text, "night")
	assert_string_contains(env_text, "weather-driven")
	assert_string_contains(env_text, "distance 320")
	var water_text := (page.find_child("WaterState", true, false) as Label).text
	assert_string_contains(water_text, "height 12.50")
	assert_string_contains(water_text, "rendering")

	var time_slider := page.find_child("TimeOfDay", true, false) as HSlider
	assert_almost_eq(float(time_slider.value), 1830.0, 0.001,
			"the clock knob mirrors the live env")
	var wind_slider := page.find_child("WindStrength", true, false) as HSlider
	assert_almost_eq(float(wind_slider.value), 55.0, 0.001,
			"the wind knob mirrors the live weather")


func test_knobs_and_lightning_poke_the_live_nodes() -> void:
	var world := _make_world()
	var page := _make_page(world)
	page.refresh()

	(page.find_child("TimeOfDay", true, false) as HSlider).value = 2200
	assert_almost_eq(float((world.env as StubEnv).time_of_day), 2200.0, 0.001,
			"scrubbing the clock pokes the env directly")
	(page.find_child("WindStrength", true, false) as HSlider).value = 10
	assert_almost_eq(float((world.weather as StubWeather).wind_strength), 10.0, 0.001)

	(page.find_child("LightningShort", true, false) as Button).pressed.emit()
	(page.find_child("LightningLong", true, false) as Button).pressed.emit()
	assert_eq((world.weather as StubWeather).short_strikes, 1)
	assert_eq((world.weather as StubWeather).long_strikes, 1)
