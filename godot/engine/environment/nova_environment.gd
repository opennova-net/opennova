@tool
class_name NovaEnvironment
extends Node

# Runtime/editor TOD state manager.
# Engine equivalents:
# - sub_540B90@0x540B90 advances time.
# - [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolates keyframe colors.
# - Jointops.exe Render_SetFogParams@0x54B4B0 / Terrain_SetLightingColors@0x5C4B10
#   push fog and terrain lighting state.

@export var environment_data: EnvFile:
	set(value):
		if environment_data and environment_data.environment_changed.is_connected(_on_environment_changed):
			environment_data.environment_changed.disconnect(_on_environment_changed)
		environment_data = value
		if environment_data and not environment_data.environment_changed.is_connected(_on_environment_changed):
			environment_data.environment_changed.connect(_on_environment_changed)
		if is_inside_tree():
			_ensure_loaded()
			_update_tod()

@export_range(0, 2359, 1) var time_of_day: float = 1200.0:
	set(value):
		time_of_day = fposmod(value, 2400.0)
		if is_loaded():
			_update_tod()

@export_range(0, 200, 1) var day_speed: float = 0.0

var _tod: Dictionary = {}
var _sun_dir := Vector3(0.0, 0.70710678, 0.70710678)
var _moon_dir := Vector3.ZERO
var _fill_light := Vector3(0.4, 0.45, 0.55)
var _sun_light := Vector3(0.9, 0.85, 0.75)
var _fog_color_rt := Vector3(0.5, 0.7, 0.9)
var _fog_distance: float = 1000.0


func _ready() -> void:
	set_process_priority(-20)
	_ensure_loaded()
	if is_loaded():
		time_of_day = float(environment_data.get_curtime())
	_update_tod()


func _process(delta: float) -> void:
	if not is_loaded():
		return
	if day_speed <= 0.0:
		return
	time_of_day += delta * day_speed
	while time_of_day >= 2400.0:
		time_of_day -= 2400.0
	while time_of_day < 0.0:
		time_of_day += 2400.0
	_update_tod()


func _ensure_loaded() -> void:
	if environment_data and not environment_data.is_loaded():
		var err := environment_data.load()
		if err != OK:
			printerr("NovaEnvironment: failed to load .env: ", err)


func is_loaded() -> bool:
	return environment_data != null and environment_data.is_loaded()


func _on_environment_changed() -> void:
	if is_loaded():
		_update_tod()


func _update_tod() -> void:
	if not is_loaded():
		_write_shader_globals()
		return
	_sun_dir = environment_data.compute_sun_direction(time_of_day)
	_moon_dir = environment_data.compute_moon_direction(time_of_day)
	_tod = environment_data.interpolate_time_of_day(time_of_day)
	if not _tod.is_empty():
		_sun_light = _tod.get("sun", _sun_light)
		_fill_light = _tod.get("ground", _fill_light)
		_fog_color_rt = _tod.get("fog", _fog_color_rt)
	_fog_distance = environment_data.get_fog_level()
	_write_shader_globals()


func _write_shader_globals() -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", _fill_light)
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", _sun_light)
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", get_sky_ambient())
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", _sun_dir)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_color", _fog_color_rt)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_end", get_fog_level())
	RenderingServer.global_shader_parameter_set(&"opennova_fog_start", get_fog_start())
	RenderingServer.global_shader_parameter_set(&"opennova_fog_type", get_fog_type())
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_amount", 1.0)
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_phase", 0.0)


func get_sun_light() -> Vector3:
	return _sun_light


func get_fill_light() -> Vector3:
	return _fill_light


func get_sky_ambient() -> Vector3:
	return _tod.get("sky", Vector3(0.3, 0.4, 0.6))


func get_fog_color() -> Vector3:
	return _fog_color_rt


func get_terrain_tint() -> Vector3:
	if environment_data == null:
		return Vector3.ONE
	var color := environment_data.get_terrain_tint()
	return Vector3(color.r, color.g, color.b)


func get_terrain_lighting_attenuation() -> Vector3:
	# terrain_rgb is a recovered reciprocal attenuation LUT, not a direct tint.
	# Jointops.exe Terrain_SetEnvironmentData@0x53F840 is only a buffer copy;
	# keep this neutral until the exact runtime consumer is located.
	return Vector3.ONE


## Push the env-derived terrain lighting + fog uniforms onto a terrain
## ShaderMaterial. terrain.gdshader (runtime) and terrain_editor.gdshader (editor
## preview) share these uniforms via terrain_lighting.gdshaderinc, so both drive
## them through here. Callers may override individual values afterwards (the
## runtime layers NovaWeather-smoothed colors + the tile-overlay tint on top).
func apply_terrain_uniforms(material: ShaderMaterial) -> void:
	if material == null:
		return
	material.set_shader_parameter("u_sun_light", get_sun_light())
	material.set_shader_parameter("u_fill_light", get_fill_light())
	material.set_shader_parameter("u_sky_ambient", get_sky_ambient())
	material.set_shader_parameter("u_sun_direction", get_sun_direction())
	material.set_shader_parameter("u_terrain_tint", get_terrain_lighting_attenuation())
	material.set_shader_parameter("u_fog_color", get_fog_color())
	material.set_shader_parameter("u_fog_end", get_fog_level())
	material.set_shader_parameter("u_fog_start", get_fog_start())
	material.set_shader_parameter("u_fog_type", get_fog_type())


func get_water_color() -> Vector3:
	if environment_data == null:
		return Vector3(0.408, 0.314, 0.224)
	var color := environment_data.get_water_color()
	return Vector3(color.r, color.g, color.b)


func has_water_height() -> bool:
	return environment_data != null and environment_data.has_water_height()


func get_water_height() -> float:
	return environment_data.get_water_height() if has_water_height() else 0.0


func get_cloud_tint() -> Vector3:
	if environment_data == null:
		return Vector3(0.5, 0.5, 0.5)
	var color := environment_data.get_cloud_tint()
	return Vector3(color.r, color.g, color.b)


func get_sun_direction() -> Vector3:
	return _sun_dir


func get_moon_direction() -> Vector3:
	return _moon_dir


func get_sun_color() -> Vector3:
	return _tod.get("sun", Vector3.ZERO)


func get_moon_color() -> Vector3:
	return _tod.get("moon", Vector3.ZERO)


func get_sky_base() -> Vector3:
	return _tod.get("skybase", Vector3.ZERO)


func get_sky_bright() -> Vector3:
	return _tod.get("skybright", Vector3.ZERO)


func get_sky_highlight() -> Vector3:
	return _tod.get("skyhighlight", Vector3.ZERO)


func get_horizon_color() -> Vector3:
	return _tod.get("cloudbase", Vector3.ZERO)


func get_ground_fog_color() -> Vector3:
	return _tod.get("cloudhighlight", Vector3.ZERO)


func get_secondary_ambient() -> Vector3:
	return _tod.get("cloudedge", Vector3.ZERO)


func get_skyfog_color() -> Vector3:
	return _tod.get("skyfog", Vector3.ZERO)


func set_fill_light(value: Vector3) -> void:
	_fill_light = value


func set_sun_light(value: Vector3) -> void:
	_sun_light = value


func set_fog_color_rt(value: Vector3) -> void:
	_fog_color_rt = value


func get_fog_distance() -> float:
	return _fog_distance


func get_fog_level() -> float:
	return environment_data.get_fog_level() if environment_data else 1000.0


func get_fog_start() -> float:
	var fog_end := get_fog_level()
	match get_fog_type():
		2:
			return fog_end * 0.5
		3:
			return fog_end * 0.25
		_:
			return 0.5


func get_fog_type() -> int:
	return environment_data.get_fog_type() if environment_data else 2


func get_sky_speed() -> float:
	return environment_data.get_sky_speed() if environment_data else 15.0


func get_sky_height() -> float:
	return environment_data.get_sky_height() if environment_data else 175.0


func get_sky_map1_tex() -> Texture2D:
	return environment_data.get_sky_map1_tex() if environment_data else null


func get_sky_map2_tex() -> Texture2D:
	return environment_data.get_sky_map2_tex() if environment_data else null


func get_environment_data() -> EnvFile:
	return environment_data
