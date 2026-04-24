@tool
class_name NovaWeather
extends Node3D

# Weather/light smoothing adapter.
# Engine equivalents: sub_540B90@0x540B90 weather tick, with the wind PRNG,
# lightning timers, and 1/8 color smoothing described in docs/engine_spec_env.md.

@export var environment_path: NodePath
@export_range(0, 100, 1) var wind_strength: float = 0.0:
	set(value):
		_weather_intensity = int(value / 100.0 * 8192.0)
	get:
		return float(_weather_intensity) / 8192.0 * 100.0

var _cached_env: Node = null
var _prng: int = 0x12345633
var _sway_buffer: Array[int] = []
var _sway_smoothed_buffer: Array[int] = []
var _sway_index: int = 0
var _sway_pos: int = 0
var _sway_smoothed: int = 0
var _sway_velocity: int = 0
var _sway_prev_noise: int = 0
var _weather_intensity: int = 0
var _wind_duration: int = 0
var _lightning_short_timer: int = 0
var _lightning_long_timer: int = 0
var _lightning_intensity: float = 0.0
var _color_fade_timer: int = 0
var _color_fade_rate: int = 0
var _smooth_fill := Vector3.ZERO
var _smooth_sun := Vector3.ZERO
var _smooth_fog := Vector3.ZERO
var _smooth_sky := Vector3.ZERO
var _colors_synced := false
var _outdoor_color := Vector3.ZERO
var _indoor_color := Vector3.ZERO


static func _clamp_vec3(value: Vector3, min_value: float = 0.0, max_value: float = 1.0) -> Vector3:
	return Vector3(
		clampf(value.x, min_value, max_value),
		clampf(value.y, min_value, max_value),
		clampf(value.z, min_value, max_value)
	)


func _init() -> void:
	_sway_buffer.resize(256)
	_sway_buffer.fill(0)
	_sway_smoothed_buffer.resize(256)
	_sway_smoothed_buffer.fill(0)


func _ready() -> void:
	set_process_priority(-10)
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null


func _process(_delta: float) -> void:
	if not _cached_env or not _cached_env.has_method("is_loaded") or not _cached_env.is_loaded():
		return
	if not _colors_synced:
		_smooth_fill = _cached_env.get_fill_light()
		_smooth_sun = _cached_env.get_sun_light()
		_smooth_fog = _cached_env.get_fog_color()
		_smooth_sky = _cached_env.get_sky_ambient()
		_colors_synced = true
	_weather_tick(_cached_env)
	_write_shader_globals(_cached_env)


static func _rol32(value: int, shift: int) -> int:
	value = value & 0xFFFFFFFF
	return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


func _weather_tick(env: Node) -> void:
	# sub_540B90 PRNG/wind oscillator: rotate-left seed, square-weight noise,
	# 256-entry cyclic buffers, then smooth toward the target sway.
	var next := _rol32(_prng, 9)
	_prng = (next + ((next >> 31) & 0x1ABB09)) & 0xFFFFFFFF
	var noise := _prng & 0xFFF
	var scaled := (_weather_intensity * (15 * _sway_prev_noise + ((noise * noise) >> 8))) >> 12
	_sway_index = (_sway_index + 1) & 0xFF
	_sway_buffer[_sway_index] = maxi(0, 0xFFFF - 2 * scaled)
	_sway_pos += _sway_velocity + scaled
	_sway_smoothed = (_sway_pos + 31 * _sway_smoothed) >> 5
	_sway_velocity = (63 * (_sway_velocity + ((0x8000 - _sway_pos) >> 4))) >> 6
	_sway_smoothed = clampi(_sway_smoothed, 0, 0xFFFF)
	_sway_smoothed_buffer[_sway_index] = _sway_smoothed
	_sway_prev_noise = scaled

	if _wind_duration > 0:
		_wind_duration -= 1
	if _wind_duration == 0 and _weather_intensity > 0:
		_weather_intensity = (_weather_intensity * 31) >> 5

	if _lightning_short_timer > 0:
		_lightning_short_timer -= 1
		match _lightning_short_timer:
			10, 4:
				_lightning_intensity = 0xC8 / 255.0
			6, 2:
				_lightning_intensity = 1.0
			0:
				_lightning_intensity = 0.0

	if _lightning_long_timer > 0:
		_lightning_long_timer -= 1
		match _lightning_long_timer:
			31, 26:
				_lightning_intensity = maxf(_lightning_intensity, 0xC8 / 255.0)
			28, 24:
				_lightning_intensity = maxf(_lightning_intensity, 0x96 / 255.0)
			23:
				_lightning_intensity = maxf(_lightning_intensity, 0x64 / 255.0)
			22:
				_lightning_intensity = maxf(_lightning_intensity, 0x32 / 255.0)
			20:
				_lightning_intensity = 0.0

	if _lightning_intensity > 0.0:
		var env_data: EnvFile = env.get_environment_data()
		if env_data:
			var color := env_data.get_lightning_color()
			var lightning := Vector3(color.r, color.g, color.b)
			env.set_fill_light(_clamp_vec3(env.get_fill_light() + lightning * _lightning_intensity))
			env.set_sun_light(_clamp_vec3(env.get_sun_light() + lightning * _lightning_intensity))

	_color_fade_timer = maxi(0, _color_fade_timer - _color_fade_rate)
	_smooth_fill += (env.get_fill_light() - _smooth_fill) * 0.125
	_smooth_sun += (env.get_sun_light() - _smooth_sun) * 0.125
	_smooth_fog += (env.get_fog_color() - _smooth_fog) * 0.125
	_smooth_sky += (env.get_sky_ambient() - _smooth_sky) * 0.125

	if _color_fade_timer > 0:
		var raw := maxi(0, 0x8000 - _color_fade_timer)
		var rain_atten := float(raw) / float(0x8000)
		_smooth_fill *= rain_atten
		_smooth_sun *= rain_atten
		_smooth_fog *= rain_atten
		_smooth_sky *= rain_atten

	env.set_fill_light(_smooth_fill)
	env.set_sun_light(_smooth_sun)
	env.set_fog_color_rt(_smooth_fog)

	var env_data: EnvFile = env.get_environment_data()
	if env_data:
		var max_fog := env_data.get_fog_level()
		var half_fog := max_fog * 0.5
		var quarter_fog := max_fog * 0.25
		var current_fog: float = env.get_fog_distance()
		var ceiling := env_data.get_ceiling_color()
		var ceiling_vec := Vector3(ceiling.r, ceiling.g, ceiling.b)
		if current_fog < half_fog and quarter_fog > 0.0:
			var floor_color := env_data.get_floor_color()
			var floor_vec := Vector3(floor_color.r, floor_color.g, floor_color.b)
			var t := clampf((current_fog - quarter_fog) / quarter_fog, 0.0, 1.0)
			_outdoor_color = _smooth_fog * t + ceiling_vec * (1.0 - t)
			_indoor_color = ceiling_vec * (181.0 / 256.0) + floor_vec * (181.0 / 256.0)
		else:
			_outdoor_color = _smooth_fog
			_indoor_color = ceiling_vec


func trigger_lightning_short() -> void:
	_lightning_short_timer = 16


func trigger_lightning_long() -> void:
	_lightning_long_timer = 32


func set_wind_duration(seconds: int) -> void:
	_wind_duration = 6 * seconds
	if seconds > 0 and _weather_intensity == 0:
		_weather_intensity = 2048


func get_wind_duration() -> int:
	return _wind_duration / 6


func get_sway_amount() -> float:
	return float(_sway_smoothed - 0x8000) / float(0x8000) * 2.0


func get_sway_phase() -> float:
	return float(_sway_index) * (PI * 2.0 / 256.0)


func get_smooth_fill() -> Vector3:
	return _smooth_fill


func get_smooth_sun() -> Vector3:
	return _smooth_sun


func get_smooth_fog() -> Vector3:
	return _smooth_fog


func get_smooth_sky() -> Vector3:
	return _smooth_sky


func get_lightning_intensity() -> float:
	return _lightning_intensity


func _write_shader_globals(env: Node) -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", _smooth_fill)
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", _smooth_sun)
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", _smooth_sky)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_color", _smooth_fog)
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", env.get_sun_direction())
	var fog_end: float = float(env.get_fog_level())
	RenderingServer.global_shader_parameter_set(&"opennova_fog_end", fog_end)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_start", fog_end * 0.3)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_type", env.get_fog_type())
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_amount", maxf(0.25, absf(get_sway_amount())))
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_phase", get_sway_phase())
