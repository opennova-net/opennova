@tool
class_name NovaWeather
extends Node3D

# Weather/light smoothing host. NovaWeatherCore (godot/engine/env, math in
# libs/env) owns the witnessed state cluster of
# [orig: Environment_UpdateWeatherTick @ 0x57e9b0]: the wind PRNG/sway
# oscillator, both lightning flash sequencers
# ([orig: Environment_SetLightningFlash @ 0x57d320] SET-per-epoch additives),
# rain fade, and the four color-block pipelines
# ([orig: interpolate_weather_color @ 0x57d9e0]). This node is scene
# plumbing only: it feeds the env node's TOD targets into the core each
# 62 Hz tick, writes the smoothed colors back, and publishes the shader
# globals. See docs/env/env-tod-re.md.

@export var environment_path: NodePath

# Declared before wind_strength: the export's default assignment runs the
# setter during init, which needs the core.
var _core := NovaWeatherCore.new()
var _cached_env: Node = null
var _colors_synced := false

# 100 = the witnessed retail constant (Env_WindScale 256, always on — the
# ambient foliage sway every retail map has), which is also the default.
@export_range(0, 100, 1) var wind_strength: float = 100.0:
	set(value):
		# Env_WindScale units: 100% maps to 256 [orig: Environment_InitDefaults
		# @ 0x57c1d1, its only writer]. The oscillator's 15*prev feedback term
		# is stable only for intensity <= 273 — the previous 0..8192 mapping
		# drove the 32-bit state divergent (docs/env/env-tod-re.md).
		_core.set_wind_intensity(int(value / 100.0 * 256.0))
	get:
		return float(_core.get_wind_intensity()) / 256.0 * 100.0


func _ready() -> void:
	set_process_priority(-10)
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null


func _process(_delta: float) -> void:
	if not _cached_env or not _cached_env.has_method("is_loaded") or not _cached_env.is_loaded():
		return
	var env := _cached_env
	if not _colors_synced:
		_core.snap_colors(
			_vec3_color(env.get_fill_light()),
			_vec3_color(env.get_sun_light()),
			_vec3_color(env.get_fog_color()),
			_vec3_color(env.get_sky_ambient()))
		_colors_synced = true
	var env_data: EnvFile = env.get_environment_data()
	var lightning := Color.WHITE
	if env_data:
		lightning = env_data.get_lightning_color()
		# The iris auto-exposure target (env #17): the outdoor gain from the
		# current smoothed colors, chased by the modulator over 62 ticks —
		# retail re-targets every render pass, i.e. every tick
		# [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538;
		#  curve terrain_sector_compute_lighting @ 0x5c7550].
		_core.set_exposure_from_iris(
			env.get_sun_direction(),
			env_data.get_iris_percent(),
			env_data.get_iris_center())
	# The smoothers chase the TOD keyframe targets, never their own written-
	# back output [orig: Environment_ComputeTimeOfDayColors @ 0x57de40
	# refreshes every block's target slot ahead of the weather tick]. The
	# cloud-scroll rate ramps toward sky_speed << 10 at the tick's tail
	# [orig: @ 0x57eecc; accumulators @ 0x57f1a5..0x57f1d1].
	# env #27: refresh the scalar spring targets from the PARSED values before
	# the tick (retail refreshes targets ahead of the smoothers; the snap
	# touches targets only — currents always ramp [orig: @ 0x57d1e0]).
	if env.has_method("get_fog_level_target"):
		_core.set_scalar_targets(env.get_fog_level_target(), env.get_sky_height_target())
	_core.tick(
		_vec3_color(env.get_fill_light_target()),
		_vec3_color(env.get_sun_light_target()),
		_vec3_color(env.get_fog_color_target()),
		_vec3_color(env.get_sky_ambient_target()),
		lightning,
		env.get_sky_speed())
	env.set_fill_light(get_smooth_fill())
	env.set_sun_light(get_smooth_sun())
	env.set_fog_color_rt(get_smooth_fog())
	# The sky block joins the writeback set — entity hemi_sky serves the
	# smoothed+modulated block like fill/sun/fog [orig: Env_SkyBlock[0]
	# consumed by the entity-constants writer @ 0x5c8090].
	if env.has_method("set_sky_ambient_rt"):
		env.set_sky_ambient_rt(get_smooth_sky())
	if env.has_method("set_color_src_gain"):
		env.set_color_src_gain(_core.get_color_src_gain())
	# env #27 writeback: the smoothed scalar currents flow back through the
	# env seam so every consumer (dome, water, object/terrain fog, the frame
	# clear) serves the ramp.
	if env.has_method("set_smoothed_scalars"):
		env.set_smoothed_scalars(_core.get_fog_distance(), _core.get_sky_height(), _core.get_sun_dim_pct())
	_write_shader_globals(env)


## Snap the smoothing state to the env's current colors on the next tick —
## call after discrete TOD scrubs so the editor preview doesn't lag.
func resync_colors() -> void:
	_colors_synced = false


static func _vec3_color(value: Vector3) -> Color:
	return Color(value.x, value.y, value.z)


static func _color_vec3(value: Color) -> Vector3:
	return Vector3(value.r, value.g, value.b)


func trigger_lightning_short() -> void:
	_core.trigger_lightning_short()


func trigger_lightning_long() -> void:
	_core.trigger_lightning_long()


func set_wind_duration(seconds: int) -> void:
	_core.set_wind_duration_ticks(6 * seconds)
	if seconds > 0 and _core.get_wind_intensity() == 0:
		_core.set_wind_intensity(256)


func get_wind_duration() -> int:
	return _core.get_wind_duration_ticks() / 6


func get_sway_amount() -> float:
	return _core.get_sway_amount()


func get_sway_phase() -> float:
	return _core.get_sway_phase()


func get_smooth_fill() -> Vector3:
	return _color_vec3(_core.get_fill())


func get_smooth_sun() -> Vector3:
	return _color_vec3(_core.get_sun())


func get_smooth_fog() -> Vector3:
	return _color_vec3(_core.get_fog())


func get_smooth_sky() -> Vector3:
	return _color_vec3(_core.get_sky())


func get_lightning_intensity() -> float:
	return _core.get_lightning_intensity()


# The witnessed cloud-scroll UV translations for a camera at (cam_x, cam_z)
# world units — U carries the accumulator NEGATIVELY, V positively
# [orig: render_skybox @ 0x5791de..0x579260]. The sky dome (and the water
# scroll rate below) consume these through this seam, like the terrain
# consumes get_smooth_sun.
func get_cloud_uv_offset1(cam_x: float, cam_z: float) -> Vector2:
	return _core.get_cloud_uv_offset1(cam_x, cam_z)


func get_cloud_uv_offset2(cam_x: float, cam_z: float) -> Vector2:
	return _core.get_cloud_uv_offset2(cam_x, cam_z)


func get_cloud_uv_rate_per_second() -> float:
	return _core.get_cloud_uv_rate_per_second()


# The witnessed water UV transform (scale, bias, offset_u, offset_v) - the
# water surface shares the layer-1 cloud accumulators with a 32x camera term
# [orig: render_water_surface @ 0x5c3348..0x5c33db].
func get_water_uv_state(cam_x: float, cam_z: float, fog_distance: float) -> Vector4:
	return _core.get_water_uv_state(cam_x, cam_z, fog_distance)


func _write_shader_globals(env: Node) -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", get_smooth_fill())
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", get_smooth_sun())
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", get_smooth_sky())
	RenderingServer.global_shader_parameter_set(&"opennova_fog_color", get_smooth_fog())
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", env.get_sun_direction())
	var fog_end: float = float(env.get_fog_level())
	var fog_start: float = float(env.get_fog_start()) if env.has_method("get_fog_start") else 0.5
	RenderingServer.global_shader_parameter_set(&"opennova_fog_end", fog_end)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_start", fog_start)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_type", env.get_fog_type())
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_amount", maxf(0.25, absf(get_sway_amount())))
	RenderingServer.global_shader_parameter_set(&"opennova_wind_sway_phase", get_sway_phase())
	# The modulator /64 gain (iris exposure) for self-lit/effect shaders
	# [orig: Render_UnpackModulatorToLightScale @ 0x58db30].
	RenderingServer.global_shader_parameter_set(&"opennova_color_src_gain", _core.get_color_src_gain())
