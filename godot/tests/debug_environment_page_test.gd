extends GutTest

# DebugEnvironmentPage: env/weather/water readouts through the typed
# GameWorld -> MissionEnvironment/Weather/Water seams (REAL native nodes —
# GDScript cannot intercept native methods, so the pins observe real state),
# the shared-session scrub knobs and authority gates, and empty states
# without a world.

const PageScript := preload("res://game/debug/pages/debug_environment_page.gd")
const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"


class WorldHarness:
	extends GameWorld
	var env: MissionEnvironment = null
	var weather: Weather = null
	var water: Water = null
	var clock_resyncs := 0

	func get_environment_node() -> MissionEnvironment:
		return env

	func get_weather_node() -> Weather:
		return weather

	func get_water_node() -> Water:
		return water

	func get_debug_mission_minute_of_day() -> float:
		return env.get_mission_minute_of_day() \
				if env != null else 0.0

	func debug_set_mission_minute_of_day(value: float) -> Error:
		if env == null:
			return ERR_UNAVAILABLE
		var err: Error = env.debug_set_mission_minute_of_day(value)
		if err == OK and weather != null:
			clock_resyncs += 1
			weather.resync_colors_now()
		return err


func _make_world() -> WorldHarness:
	var world := WorldHarness.new()
	world.env = MissionEnvironment.new()
	world.env.name = "DebugEnv"
	var data := EnvFile.new()
	data.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	data.load()
	world.env.environment_data = data
	world.env.time_of_day = 2200.0  # a real night phase
	world.weather = Weather.new()
	world.weather.wind_strength = 50.0  # 128/256: exact through the Env_WindScale units
	world.water = Water.new()
	world.water.water_height = 12.5
	world.add_child(world.env)
	world.add_child(world.weather)
	world.add_child(world.water)
	# Off-tree: the GameWorld script class alone has no scene children, and
	# the harness getters hand out direct refs. Sibling wiring resolves the
	# relative path without a tree; one real tick claims weather-driven.
	world.weather.environment_path = NodePath("../DebugEnv")
	world.weather.tick_fixed()
	autofree(world)
	return world


func _make_page(
		world: GameWorld = null,
		has_authority: bool = true,
		edit_unlocked: bool = true) -> DebugEnvironmentPage:
	var ctx := DebugContext.new()
	ctx.options = DebugOptionState.new()
	ctx.world_source = func(): return world
	ctx.session = DebugSession.new()
	DebugCatalog.install(ctx.session)
	DebugCatalog.bind_runtime_targets(
			ctx.session, func(): return null, ctx.world_source)
	ctx.session.set_authority_source(func(): return has_authority)
	ctx.session.set_edit_unlocked(edit_unlocked)
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
	assert_string_contains(env_text, "Time 2200")
	assert_string_contains(env_text, "night")
	assert_string_contains(env_text, "weather-driven")
	assert_string_contains(env_text, "distance 1000")
	var water_text := (page.find_child("WaterState", true, false) as Label).text
	assert_string_contains(water_text, "height 12.50")
	assert_string_contains(water_text, "rendering")

	var time_slider := page.find_child("TimeOfDay", true, false) as HSlider
	assert_almost_eq(float(time_slider.value), 22.0 * 60.0, 0.001,
			"the clock knob mirrors the live env")
	assert_eq(int(time_slider.max_value), 1439,
			"every slider value is a valid minute of day")
	var wind_slider := page.find_child("WindStrength", true, false) as HSlider
	assert_almost_eq(float(wind_slider.value), 50.0, 0.001,
			"the wind knob mirrors the live weather")


func test_knobs_and_lightning_poke_the_live_nodes() -> void:
	var world := _make_world()
	var page := _make_page(world)
	page.refresh()

	(page.find_child("TimeOfDay", true, false) as HSlider).value = 10 * 60
	assert_almost_eq(float(world.env.time_of_day), 1000.0, 0.001,
			"scrubbing the clock uses the shared public control")
	assert_eq(world.clock_resyncs, 1,
			"the world immediately resyncs rendered weather for paused scrubs")
	(page.find_child("WindStrength", true, false) as HSlider).value = 25
	assert_almost_eq(float(world.weather.wind_strength), 25.0, 0.001)

	# Real sequencers: the short trigger's first witnessed epoch lands within
	# six ticks, the long trigger flashes on its first tick.
	(page.find_child("LightningShort", true, false) as Button).pressed.emit()
	for _i in 6:
		world.weather.tick_fixed()
	assert_gt(world.weather.get_lightning_intensity(), 0.0,
			"the short-strike button reaches the live sequencer")
	var world_b := _make_world()
	var page_b := _make_page(world_b)
	page_b.refresh()
	(page_b.find_child("LightningLong", true, false) as Button).pressed.emit()
	for _i in 6:
		world_b.weather.tick_fixed()
	assert_gt(world_b.weather.get_lightning_intensity(), 0.0,
			"the long-strike button reaches the live sequencer")


func test_joiner_or_locked_overlay_cannot_mutate_environment() -> void:
	var world := _make_world()
	var joiner_page := _make_page(world, false, true)
	(joiner_page.find_child("TimeOfDay", true, false) as HSlider).value = 10 * 60
	(joiner_page.find_child("LightningShort", true, false) as Button).pressed.emit()
	assert_almost_eq(float(world.env.time_of_day), 2200.0, 0.001)

	var locked_page := _make_page(world, true, false)
	(locked_page.find_child("WindStrength", true, false) as HSlider).value = 25
	(locked_page.find_child("LightningLong", true, false) as Button).pressed.emit()
	assert_almost_eq(float(world.weather.wind_strength), 50.0, 0.001)
	# Neither blocked trigger reaches the live sequencer.
	for _i in 6:
		world.weather.tick_fixed()
	assert_eq(world.weather.get_lightning_intensity(), 0.0,
			"blocked strike buttons never reach the sequencer")
	assert_eq(world.clock_resyncs, 0, "blocked scrubs never resync weather")


func test_public_clock_scrub_reseeds_the_fixed_point_mission_clock() -> void:
	var env: MissionEnvironment = autofree(MissionEnvironment.new()) as MissionEnvironment
	env.configure_mission_clock(0x0540, 60)

	assert_eq(env.debug_set_mission_minute_of_day(12.0 * 60.0 + 34.0), OK)
	assert_almost_eq(env.time_of_day, 1234.0, 0.001)
	env.advance_mission_clock(1)

	assert_gt(env.time_of_day, 1234.0,
			"the next world-driven tick advances from the scrub instead of restoring the old clock")
	assert_lt(env.time_of_day, 1235.0)
	var advanced: float = env.time_of_day
	assert_eq(env.debug_set_mission_minute_of_day(1440.0), ERR_INVALID_PARAMETER)
	assert_eq(env.debug_set_mission_minute_of_day(NAN), ERR_INVALID_PARAMETER)
	assert_almost_eq(env.time_of_day, advanced, 0.000001,
			"invalid minute-of-day values never disturb the accumulator")
