@tool
class_name NovaEnvironment
extends Node

# Runtime/editor TOD state manager.
# Engine equivalents (docs/env/env-tod-re.md):
# - [orig: Environment_UpdateWeatherTick @ 0x57e9b0] advances time per tick.
# - [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolates keyframe
#   colors and selects sun-vs-moon light by the hardcoded day-phase windows.
# - [orig: Environment_ApplyFogAndAmbient @ 0x57e440] pushes fog state;
#   fog/skyfog render colors are doubled with saturation (@ 0x57f17c).

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
var _light_dir := Vector3(0.0, 0.70710678, 0.70710678)
var _is_night := false
var _day_phase_blend := 1.0
var _fill_light := Vector3(0.4, 0.45, 0.55)
var _sun_light := Vector3(0.9, 0.85, 0.75)
var _fog_color_rt := Vector3(0.5, 0.7, 0.9)
var _fog_distance: float = 1000.0

# Monotonic counter bumped only when a value object materials consume (lighting/fog) actually
# changes -- via _update_tod (TOD scrub / reload / day_speed advance) or the per-frame weather
# setters below (guarded so a settled smoother stops bumping). NovaObjectModel caches the last
# generation it applied and skips its per-material environment push while this is unchanged.
var _env_generation: int = 0


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
	# The light source switches sun<->moon at the hardcoded 06:00/18:45
	# boundaries [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	var phase: Dictionary = environment_data.get_day_phase(time_of_day)
	_is_night = bool(phase.get("is_night", false))
	_day_phase_blend = float(phase.get("blend", 1.0))
	_light_dir = _moon_dir if _is_night else _sun_dir
	_tod = environment_data.interpolate_time_of_day(time_of_day)
	if not _tod.is_empty():
		var light_key := "moon" if _is_night else "sun"
		_sun_light = _tod.get(light_key, _sun_light)
		_fill_light = _tod.get("ground", _fill_light)
		# Fog render color is the keyframe color doubled, saturating
		# [orig: Environment_UpdateWeatherTick @ 0x57f17c].
		var fog_raw: Vector3 = _tod.get("fog", _fog_color_rt * 0.5)
		_fog_color_rt = _double_vec3(fog_raw)
	_fog_distance = environment_data.get_fog_level()
	# A TOD recompute can move any object-consumed value; this path runs only on discrete
	# changes (scrub / reload / day_speed advance), never per-frame at rest, so an
	# unconditional bump here costs nothing in steady state.
	_env_generation += 1
	_write_shader_globals()


static func _double_vec3(value: Vector3) -> Vector3:
	var doubled := EnvFile.double_saturate_color(Color(value.x, value.y, value.z))
	return Vector3(doubled.r, doubled.g, doubled.b)


func _write_shader_globals() -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", _fill_light)
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", _sun_light)
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", get_sky_ambient())
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", _light_dir)
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
	# terrain_rgb IS a direct live tint with three witnessed consumers (C6/G3):
	# the terrain texture bake (channel*v >> 12 [orig: PolyTrn_InitTextures
	# @ 0x60b8cb]), the water-quad half tint [orig: PolyTrn_RenderTile @ 0x60df0d],
	# and the foliage lightmap */128 [orig: sample_terrain_lightmap @ 0x606030].
	# Those per-consumer ports ride the terrain/foliage work, not the env slice -
	# returning identity here is tracked divergence #19 (docs/env/env-tod-re.md),
	# kept neutral rather than faked through the wrong (uniform-tint) path.
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
	material.set_shader_parameter("u_sun_direction", get_light_direction())
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


## Sun by day, moon by night [orig: Environment_GetLightDirectionFloat @ 0x57d870].
func get_light_direction() -> Vector3:
	return _light_dir


func is_night_phase() -> bool:
	return _is_night


## 0..1 ramp toward the current phase across the 20-minute sunrise/sunset
## windows [orig: Environment_ComputeTimeOfDayColors @ 0x57de99].
func get_day_phase_blend() -> float:
	return _day_phase_blend


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


## Cloud-pass color blocks (sky dome VS constants c24/c27/c26)
## [orig: render_skybox uploads @ 0x57934a..0x57936f].
func get_cloud_base() -> Vector3:
	return _tod.get("cloudbase", Vector3.ZERO)


func get_cloud_highlight() -> Vector3:
	return _tod.get("cloudhighlight", Vector3.ZERO)


func get_cloud_edge() -> Vector3:
	return _tod.get("cloudedge", Vector3.ZERO)


func get_skyfog_color() -> Vector3:
	# The skyfog render color, doubled like fog [orig: Environment_UpdateWeatherTick
	# @ 0x57f190]. Hosts wanting the frame CLEAR color use get_frame_clear_color()
	# (the horizon-blended form; divergence #21, closed).
	return _double_vec3(_tod.get("skyfog", Vector3.ZERO))


func get_frame_clear_color() -> Vector3:
	# The frame CLEAR color (divergence #21, closed): skyfog horizon-blended
	# toward fog when the fog distance drops below half the reference distance -
	# pure fog at <= ref/4, a linear fade across [ref/4, ref/2], untouched
	# skyfog above. The blend runs on the UNDOUBLED keyframe colors (retail
	# doubles after), and the result stays undoubled: a 1x-intensity host
	# matches the witnessed non-modulate2x device path, whose Clear halves the
	# doubled color back. Reference distance: the retail default 1024 (the
	# session authority forces it; 768 is an adapter-caps fallback with no
	# host analog).
	# [orig: Environment_UpdateWeatherTick @ 0x57e9b0 blend @ 0x57f037..0x57f0a1;
	#  consumer Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca7bf; device Clear
	#  halving @ 0x67715d; defaults Environment_InitDefaults @ 0x57c0b0 /
	#  Terrain_Init @ 0x60fca3]
	var fog_raw: Vector3 = _tod.get("fog", Vector3.ZERO)
	var sky_raw: Vector3 = _tod.get("skyfog", Vector3.ZERO)
	var blended := EnvFile.horizon_blend_skyfog(
		Color(fog_raw.x, fog_raw.y, fog_raw.z),
		Color(sky_raw.x, sky_raw.y, sky_raw.z),
		_fog_distance, 1024.0)
	return Vector3(blended.r, blended.g, blended.b)


func set_fill_light(value: Vector3) -> void:
	if value != _fill_light:
		_fill_light = value
		_env_generation += 1


func set_sun_light(value: Vector3) -> void:
	if value != _sun_light:
		_sun_light = value
		_env_generation += 1


func set_fog_color_rt(value: Vector3) -> void:
	if value != _fog_color_rt:
		_fog_color_rt = value
		_env_generation += 1


## Monotonic generation, bumped only when a lighting/fog value object materials read actually
## changes. Lets a NovaObjectModel skip its per-material environment push with one int compare.
func get_env_generation() -> int:
	return _env_generation


func get_fog_distance() -> float:
	return _fog_distance


func get_fog_level() -> float:
	return environment_data.get_fog_level() if environment_data else 1000.0


func get_fog_start() -> float:
	# Policy lives in libs/env env_render [orig: Render_SetFogState @ 0x58a950];
	# overcast stays 0 until a weather system drives it.
	if environment_data:
		return environment_data.get_fog_start(0.0)
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
