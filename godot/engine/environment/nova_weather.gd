@tool
class_name NovaWeather
extends Node3D

# Weather/light smoothing owner. NovaWeatherCore (godot/engine/env, math in
# libs/env) owns the witnessed state cluster of
# [orig: Environment_UpdateWeatherTick @ 0x57e9b0]: the wind PRNG/sway
# oscillator, both lightning flash sequencers
# ([orig: Environment_SetLightningFlash @ 0x57d320] SET-per-epoch additives),
# rain fade, and all fourteen non-modulator color-block pipelines
# ([orig: interpolate_weather_color @ 0x57d9e0]). This node is scene
# plumbing only: it feeds the env node's TOD targets into the core each
# 62 Hz tick, writes the smoothed colors back, and publishes the shader
# globals. See docs/env/env-tod-re.md.

@export var environment_path: NodePath

const WEATHER_TICK_HZ := 62.0
const MAX_CATCHUP_TICKS := 31

# Declared before wind_strength: the export's default assignment runs the
# setter during init, which needs the core.
var _core := NovaWeatherCore.new()
var _configured_wind_intensity := 256
var _cached_env: Node = null
var _colors_synced := false
var _tick_credit := 0.0
var _world_tick_driven := false

# The marched iris-exposure samples (D-RLIT-2): the in-world shell stamps
# three per-sample classification codes each frame (NovaSimulation.
# compute_iris_samples — indoor / indoor-no-data / outdoor sun level); empty
# keeps the outdoor fallback sample (editor previews with no world)
# [orig: compute_ambient_light_along_direction @ 0x5c7a00].
var iris_samples := PackedInt32Array()

# 100 = the witnessed retail constant (Env_WindScale 256, always on — the
# ambient foliage sway every retail map has), which is also the default.
@export_range(0, 100, 1) var wind_strength: float = 100.0:
	set(value):
		# Env_WindScale units: 100% maps to 256 [orig: Environment_InitDefaults
		# @ 0x57c1d1, its only writer]. The oscillator's 15*prev feedback term
		# is stable only for intensity <= 273 — the previous 0..8192 mapping
		# drove the 32-bit state divergent (docs/env/env-tod-re.md).
		_configured_wind_intensity = int(value / 100.0 * 256.0)
		_core.set_wind_intensity(_configured_wind_intensity)
	get:
		return float(_configured_wind_intensity) / 256.0 * 100.0


func _ready() -> void:
	set_process_priority(-10)
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null


func _process(delta: float) -> void:
	if _world_tick_driven:
		return
	_tick_credit += maxf(delta, 0.0) * WEATHER_TICK_HZ
	var tick_count := int(floor(_tick_credit + 1.0e-9))
	if tick_count > 0:
		_tick_credit = maxf(0.0, _tick_credit - float(tick_count))
		if tick_count > MAX_CATCHUP_TICKS:
			tick_count = MAX_CATCHUP_TICKS
			_tick_credit = 0.0
	if tick_count <= 0:
		_tick_weather(0)
	else:
		for _tick in range(tick_count):
			_tick_weather(1)


## GameWorld owns the recovered 62 Hz weather/TOD accumulator while a mission
## runtime is active. Standalone/editor previews leave this false and keep the
## autonomous render-delta accumulator above.
func set_world_tick_driven(enabled: bool) -> void:
	if _world_tick_driven == enabled:
		return
	_world_tick_driven = enabled
	_tick_credit = 0.0


## Enter world-driven mode and snap every block at the mission's authored T0 before
## the first clock advance. Lazy-snapping after T1 would skip retail's first
## target chase.
func prepare_world_driven() -> void:
	_reset_for_environment(true)


## Start an independently ticking environment from the same deterministic
## mission reset epoch used by world-driven play.
func prepare_autonomous() -> void:
	_reset_for_environment(false)


func _reset_for_environment(world_tick_driven: bool) -> void:
	# GameWorld retains this node across missions, but retail's environment
	# start re-seeds the PRNG and clears every transient weather channel. A new
	# core is the single complete reset for oscillator/rings, lightning, rain,
	# color/modulator blocks, scalar springs, and cloud-scroll accumulators.
	_core = NovaWeatherCore.new()
	_core.set_wind_intensity(_configured_wind_intensity)
	iris_samples = PackedInt32Array()
	_world_tick_driven = world_tick_driven
	_tick_credit = 0.0
	resync_colors()
	_tick_weather(0)


## Advance exactly one recovered weather tick. The shell advances the integer
## mission clock immediately before this call, so every target read below sees
## curtime + advance like Environment_UpdateWeatherTick.
func tick_fixed() -> void:
	_tick_weather(1)


func _tick_weather(tick_count: int) -> void:
	if not _cached_env or not _cached_env.has_method("is_loaded") or not _cached_env.is_loaded():
		return
	var env := _cached_env
	# Claim the current render colors + shader globals: from here on the env's
	# _update_tod refreshes TARGETS only and this tick's writeback owns the
	# currents — the witnessed split [orig: Environment_ComputeTimeOfDayColors
	# @ 0x57de40], and the fix for the raw-vs-modulated strobe the per-tick
	# mission clock exposed.
	if env.has_method("set_weather_driven") and not env.is_weather_driven():
		env.set_weather_driven(true)
	if not _colors_synced:
		# Snap TO THE TARGETS — the witnessed snap form [orig:
		# Environment_SnapStateToTargets @ 0x57d1e0]: at mission start the
		# targets ARE the load-time keyframes, and after a discrete scrub
		# (resync_colors) they are the new keyframes. Seeding from the env
		# CURRENTS would re-seed the stale writeback (the currents are
		# weather-owned once driven).
		_core.snap_colors(
			_vec3_color(env.get_fill_light_target()),
			_vec3_color(env.get_sun_light_target()),
			_vec3_color(env.get_fog_color_base_target()),
			_vec3_color(env.get_sky_ambient_target()))
		_core.snap_sky_colors(
			_vec3_color(env.get_skyfog_color_target()),
			_vec3_color(env.get_ceiling_color_target()),
			_vec3_color(env.get_cloud_tint_target()),
			_vec3_color(env.get_floor_color_target()),
			_vec3_color(env.get_sky_base_target()),
			_vec3_color(env.get_sky_bright_target()),
			_vec3_color(env.get_sky_highlight_target()),
			_vec3_color(env.get_cloud_base_target()),
			_vec3_color(env.get_cloud_highlight_target()),
			_vec3_color(env.get_cloud_edge_target()))
		_colors_synced = true
	if tick_count <= 0:
		_write_weather_state(env)
		return
	var env_data: EnvFile = env.get_environment_data()
	var lightning := Color.WHITE
	if env_data:
		# lightning_rgb is a global parser color, so it takes the same envscale
		# engine view as the world-driven static blocks and water. Keep a raw fallback
		# for duck-typed legacy environment owners.
		if env.has_method("get_lightning_color_target"):
			lightning = _vec3_color(env.get_lightning_color_target())
		else:
			lightning = env_data.get_lightning_color()
		# The iris auto-exposure target (env #17): the marched in-world gain
		# when the shell stamps samples, else the outdoor fallback — chased by
		# the modulator over 62 ticks; retail re-targets every render pass,
		# i.e. every tick
		# [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538;
		#  compute_ambient_light_along_direction @ 0x5c7a00;
		#  curve terrain_sector_compute_lighting @ 0x5c7550].
		_core.set_exposure_from_iris_samples(
			iris_samples,
			env.get_sun_direction(),
			_core.get_ceiling_pre_mod(),
			_core.get_floor_pre_mod(),
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
	_core.set_sky_color_targets(
		_vec3_color(env.get_skyfog_color_target()),
		_vec3_color(env.get_ceiling_color_target()),
		_vec3_color(env.get_cloud_tint_target()),
		_vec3_color(env.get_floor_color_target()),
		_vec3_color(env.get_sky_base_target()),
		_vec3_color(env.get_sky_bright_target()),
		_vec3_color(env.get_sky_highlight_target()),
		_vec3_color(env.get_cloud_base_target()),
		_vec3_color(env.get_cloud_highlight_target()),
		_vec3_color(env.get_cloud_edge_target()))
	for _tick in range(tick_count):
		_core.tick(
			_vec3_color(env.get_fill_light_target()),
			_vec3_color(env.get_sun_light_target()),
			_vec3_color(env.get_fog_color_base_target()),
			_vec3_color(env.get_sky_ambient_target()),
			lightning,
			env.get_sky_speed())
	_write_weather_state(env)


func _write_weather_state(env: Node) -> void:
	env.set_fill_light(get_smooth_fill())
	env.set_sun_light(get_smooth_sun())
	env.set_fog_color_rt(get_smooth_fog())
	# The sky block joins the writeback set — entity hemi_sky serves the
	# smoothed+modulated block like fill/sun/fog [orig: Env_SkyBlock[0]
	# consumed by the entity-constants writer @ 0x5c8090].
	if env.has_method("set_sky_ambient_rt"):
		env.set_sky_ambient_rt(get_smooth_sky())
	if env.has_method("set_static_colors_rt"):
		env.set_static_colors_rt(
			get_smooth_ceiling(),
			get_smooth_cloud(),
			get_smooth_floor())
	if env.has_method("set_sky_colors_rt"):
		env.set_sky_colors_rt(
			get_smooth_skyfog(),
			get_smooth_sky_base(),
			get_smooth_sky_bright(),
			get_smooth_sky_highlight(),
			get_smooth_cloud_base(),
			get_smooth_cloud_highlight(),
			get_smooth_cloud_edge())
	if env.has_method("set_color_src_gain"):
		env.set_color_src_gain(_core.get_color_src_gain())
	# env #27 writeback: the smoothed scalar currents flow back through the
	# env seam so every consumer (dome, water, object/terrain fog, the frame
	# clear) serves the ramp.
	if env.has_method("set_smoothed_scalars"):
		env.set_smoothed_scalars(_core.get_fog_distance(), _core.get_sky_height(), _core.get_sun_dim_pct())
	_write_shader_globals(env)


## Snap the smoothing state to the env's current colors on the next tick —
## call after discrete TOD scrubs so the rendered runtime doesn't lag.
func resync_colors() -> void:
	_colors_synced = false


## Discrete runtime scrubs must update the rendered currents even while the
## mission transport is paused (and therefore no world-driven weather tick runs).
## A zero-tick refresh snaps the core to the new targets and writes them back
## without advancing wind, lightning, rain, or the mission clock.
func resync_colors_now() -> void:
	resync_colors()
	_tick_weather(0)


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


func get_smooth_skyfog() -> Vector3:
	return _color_vec3(_core.get_skyfog())


func get_smooth_ceiling() -> Vector3:
	return _color_vec3(_core.get_ceiling())


func get_smooth_cloud() -> Vector3:
	return _color_vec3(_core.get_cloud())


func get_smooth_floor() -> Vector3:
	return _color_vec3(_core.get_floor())


func get_smooth_sky_base() -> Vector3:
	return _color_vec3(_core.get_skybase())


func get_smooth_sky_bright() -> Vector3:
	return _color_vec3(_core.get_skybright())


func get_smooth_sky_highlight() -> Vector3:
	return _color_vec3(_core.get_skyhighlight())


func get_smooth_cloud_base() -> Vector3:
	return _color_vec3(_core.get_cloudbase())


func get_smooth_cloud_highlight() -> Vector3:
	return _color_vec3(_core.get_cloudhighlight())


func get_smooth_cloud_edge() -> Vector3:
	return _color_vec3(_core.get_cloudedge())


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
	# NovaEnvironment's public hemisphere getters include the camera-gated NVG
	# gain rewrite; publishing the smoother directly would bypass it each tick.
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", env.get_fill_light())
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", get_smooth_sun())
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", env.get_sky_ambient())
	RenderingServer.global_shader_parameter_set(&"opennova_fog_color", get_smooth_fog())
	# Terrain tile DOT3 and foliage follow the current environment light
	# (sun by day, moon by night), not the always-solar sky highlight vector.
	# [orig: Environment_GetLightDirectionFloat @ 0x57d870]
	_publish_light_direction(env.get_light_direction())
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


func _publish_light_direction(direction: Vector3) -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", direction)
