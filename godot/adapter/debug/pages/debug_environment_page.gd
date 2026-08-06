class_name DebugEnvironmentPage
extends NovaDebugPage
## Environment: the time-of-day / fog / sun state NovaEnvironment resolved,
## the weather core's wind, and the water surface — with the scrub knobs
## beside them. Knobs use the shared session and re-mirror the public nodes
## each refresh, so F3 and automation share one authority-checked path.

var _env_label: Label
var _water_label: Label
var _time_slider: HSlider
var _time_value: Label
var _wind_slider: HSlider
var _wind_value: Label


func page_id() -> StringName:
	return &"Environment"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_env_label = Label.new()
	_env_label.name = "EnvState"
	_env_label.text = "No environment."
	_env_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_env_label)

	_water_label = Label.new()
	_water_label.name = "WaterState"
	_water_label.text = "No water."
	_water_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_water_label)

	var time_row := HBoxContainer.new()
	time_row.name = "TimeRow"
	add_child(time_row)
	var time_label := Label.new()
	time_label.text = "Time of day"
	time_row.add_child(time_label)
	_time_slider = HSlider.new()
	_time_slider.name = "TimeOfDay"
	_time_slider.min_value = 0
	_time_slider.max_value = 1439
	_time_slider.step = 1
	_time_slider.value = 720
	_time_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_time_slider.tooltip_text = \
			"Scrub the mission clock by minute — sun, sky and lighting follow."
	_time_slider.value_changed.connect(_on_time_changed)
	time_row.add_child(_time_slider)
	_debug_controls[&"environment_time_of_day"] = _time_slider
	_time_value = Label.new()
	_time_value.name = "TimeOfDayValue"
	_time_value.text = "1200"
	time_row.add_child(_time_value)

	var wind_row := HBoxContainer.new()
	wind_row.name = "WindRow"
	add_child(wind_row)
	var wind_label := Label.new()
	wind_label.text = "Wind strength"
	wind_row.add_child(wind_label)
	_wind_slider = HSlider.new()
	_wind_slider.name = "WindStrength"
	_wind_slider.min_value = 0
	_wind_slider.max_value = 100
	_wind_slider.step = 1
	_wind_slider.value = 100
	_wind_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_wind_slider.tooltip_text = "Scale the wind driving foliage sway and weather gusts."
	_wind_slider.value_changed.connect(_on_wind_changed)
	wind_row.add_child(_wind_slider)
	_debug_controls[&"environment_wind_strength"] = _wind_slider
	_wind_value = Label.new()
	_wind_value.name = "WindStrengthValue"
	_wind_value.text = "100"
	wind_row.add_child(_wind_value)

	var lightning_row := HBoxContainer.new()
	lightning_row.name = "LightningRow"
	lightning_row.add_theme_constant_override("separation", 4)
	add_child(lightning_row)
	var short_button := Button.new()
	short_button.name = "LightningShort"
	short_button.text = "Lightning (short)"
	short_button.focus_mode = Control.FOCUS_NONE
	short_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	short_button.pressed.connect(_on_lightning_short)
	lightning_row.add_child(short_button)
	_debug_controls[&"environment_lightning_short"] = short_button
	var long_button := Button.new()
	long_button.name = "LightningLong"
	long_button.text = "Lightning (long)"
	long_button.focus_mode = Control.FOCUS_NONE
	long_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	long_button.pressed.connect(_on_lightning_long)
	lightning_row.add_child(long_button)
	_debug_controls[&"environment_lightning_long"] = long_button


func refresh() -> void:
	var env := _env()
	if env == null:
		_env_label.text = "No environment."
	else:
		var time_hhmm := float(env.get("time_of_day"))
		var sun: Vector3 = env.get_sun_direction()
		_env_label.text = (
				"Time %04d   %s (day blend %.2f)%s\n"
				+ "Fog: start %.0f   distance %.0f   type %d   level %.2f\n"
				+ "Sun dir (%.2f, %.2f, %.2f)") % [
			int(time_hhmm), "night" if bool(env.is_night_phase()) else "day",
			float(env.get_day_phase_blend()),
			"   (weather-driven)" if bool(env.is_weather_driven()) else "",
			float(env.get_fog_start()), float(env.get_fog_distance()),
			int(env.get_fog_type()), float(env.get_fog_level()),
			sun.x, sun.y, sun.z]
		_time_slider.set_value_no_signal(
				NovaEnvironment.hhmm_to_minute_of_day(time_hhmm))
		_time_value.text = "%04d" % int(time_hhmm)

	var weather := _weather()
	if weather != null:
		_wind_slider.set_value_no_signal(float(weather.get("wind_strength")))
		_wind_value.text = "%d" % int(float(weather.get("wind_strength")))

	var water := _water()
	if water == null:
		_water_label.text = "No water."
	elif not bool(water.is_water_active()):
		_water_label.text = "Water: none in this mission."
	else:
		_water_label.text = "Water: height %.2f   %s" % [
			float(water.get("water_height")),
			"rendering" if bool(water.is_water_render_active()) else "not rendering"]


func _world_node(getter: StringName) -> Object:
	var world := _ctx.world()
	if world == null or not world.has_method(getter):
		return null
	var node: Variant = world.call(getter)
	if node is Object and is_instance_valid(node):
		return node
	return null


func _env() -> Object:
	return _world_node(&"get_environment_node")


func _weather() -> Object:
	return _world_node(&"get_weather_node")


func _water() -> Object:
	return _world_node(&"get_water_node")


func _on_time_changed(value: float) -> void:
	_time_value.text = "%04d" % int(
			NovaEnvironment.minute_of_day_to_hhmm(value))
	if _ctx.session != null:
		_ctx.session.set_control_value(&"environment_time_of_day", value)


func _on_wind_changed(value: float) -> void:
	_wind_value.text = "%d" % int(value)
	if _ctx.session != null:
		_ctx.session.set_control_value(&"environment_wind_strength", value)


func _on_lightning_short() -> void:
	if _ctx.session != null:
		_ctx.session.invoke_control(&"environment_lightning_short")


func _on_lightning_long() -> void:
	if _ctx.session != null:
		_ctx.session.invoke_control(&"environment_lightning_long")
