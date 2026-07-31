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

const HOURS_PER_DAY := 24
# One day in the HHMM time-of-day encoding (0..2400): the wrap modulus every
# HHMM consumer shares.
const HHMM_DAY := 2400.0
const MINUTES_PER_HOUR := 60.0
const HHMM_HOUR_SCALE := 100.0
const CLOCK_MINUTES_PER_DAY := HOURS_PER_DAY * MINUTES_PER_HOUR
const FIXED24_ONE_HOUR := 1 << 24
const TOD_DAY_FIXED24 := HOURS_PER_DAY * FIXED24_ONE_HOUR
const TOD_TICKS_PER_REAL_MINUTE := 3720  # 60 seconds * 62 logic ticks
const Q8_8_TO_FIXED24_SHIFT := 16
const MIN_MINUTES_PER_DAY := 60
const DEFAULT_MINUTES_PER_DAY := 1440
const DEFAULT_START_HOUR := 12

@export var environment_data: EnvFile:
	set(value):
		if environment_data and environment_data.environment_changed.is_connected(_on_environment_changed):
			environment_data.environment_changed.disconnect(_on_environment_changed)
		environment_data = value
		if environment_data and not environment_data.environment_changed.is_connected(_on_environment_changed):
			environment_data.environment_changed.connect(_on_environment_changed)
		if is_inside_tree():
			_ensure_loaded()
			if is_loaded():
				time_of_day = float(environment_data.get_curtime())
			else:
				_update_tod()

@export_range(0, 2359, 1) var time_of_day: float = 1200.0:
	set(value):
		time_of_day = fposmod(value, HHMM_DAY)
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
var _sky_ambient_rt := Vector3(0.3, 0.4, 0.6)
var _sun_light := Vector3(0.9, 0.85, 0.75)
var _fog_color_rt := Vector3(0.5, 0.7, 0.9)
var _skyfog_color_rt := Vector3(0.5, 0.7, 0.9)
var _ceiling_color_rt := Vector3(0.5, 0.5, 0.5)
var _cloud_tint_rt := Vector3(0.5, 0.5, 0.5)
var _floor_color_rt := Vector3(0.5, 0.5, 0.5)
var _sky_base_rt := Vector3(0.3, 0.4, 0.6)
var _sky_bright_rt := Vector3(0.3, 0.4, 0.6)
var _sky_highlight_rt := Vector3(0.5, 0.5, 0.5)
var _cloud_base_rt := Vector3(0.3, 0.4, 0.6)
var _cloud_highlight_rt := Vector3(0.5, 0.5, 0.5)
var _cloud_edge_rt := Vector3(0.3, 0.4, 0.6)
# ColorSrcGlobalGain — the modulator /64 (iris exposure), NovaWeather-written.
var _color_src_gain := Vector3.ONE
# The NVG world-lighting rewrite is a view concern, so the shell supplies the
# already camera-gated (first-person-visible) state here. Gain remains in the
# retail 0..4 range even while the effect is inactive.
var _nvg_view_active := false
var _nvg_gain: int = 0
var _fog_distance: float = 1000.0
var _mission_time_fixed24: int = DEFAULT_START_HOUR * FIXED24_ONE_HOUR
var _mission_advance_per_tick: int = int(
	TOD_DAY_FIXED24 / (TOD_TICKS_PER_REAL_MINUTE * DEFAULT_MINUTES_PER_DAY))
# TRUE once a NovaWeather node drives this environment: the weather tick then
# OWNS the current render colors + the smoothed fog distance + the shader
# globals (its per-frame writeback), and _update_tod refreshes only the
# keyframe TARGETS — the witnessed split [orig: Environment_ComputeTimeOfDayColors
# @ 0x57de40 refreshes target slots; the smoothers own the currents]. Without
# this split the mission clock's per-tick TOD writes alternate RAW keyframe
# colors against the weather's modulated writeback — the whole scene then
# strobes between the two at the tick/frame beat (the 2026-07-13 "black
# flicker" regression, introduced when #224's mission clock made _update_tod
# per-tick). Standalone owners (the editor env preview without a weather node)
# keep the direct writes.
var _weather_driven := false

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
	# day_speed is an OpenNova AUTHORING knob (HHMM units/second, default off)
	# for scrubbing previews — shell plumbing, not the witnessed day advance.
	# Retail advances Env_CurTimeFixed24 by 0x18000000/(3720 x minutes) per
	# 62 Hz tick [orig: Environment_SetTodAdvanceRate @ 0x57d170 (BMS day
	# length) / Environment_SetTodRate @ 0x57c4f0 (options)]; GameWorld drives
	# that path through advance_mission_clock, separately from this preview knob.
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


## Start the one runtime mission clock from the BMS header. start_time is Q8.8
## hours and widens to the engine's 8.24 accumulator; minutes_per_day is clamped
## to retail's 60-minute minimum [orig: Game_StartMission @ 0x525371;
## Environment_SetTodAdvanceRate @ 0x57d170].
func configure_mission_clock(start_time_q8_8: int, minutes_per_day: int) -> void:
	var raw := start_time_q8_8 & 0xFFFF
	_mission_time_fixed24 = (raw << Q8_8_TO_FIXED24_SHIFT) % TOD_DAY_FIXED24
	var rate := maxi(minutes_per_day, MIN_MINUTES_PER_DAY)
	_mission_advance_per_tick = int(TOD_DAY_FIXED24 / (TOD_TICKS_PER_REAL_MINUTE * rate))
	time_of_day = mission_start_time_hhmm(start_time_q8_8)


## Convert the unsigned Q8.8 BMS mission header clock to the HHMM value used
## by NovaEnvironment and editor previews. Keeping this public conversion here
## prevents runtime and Mission preview clocks from drifting apart.
static func mission_start_time_hhmm(start_time_q8_8: int) -> float:
	var raw := start_time_q8_8 & 0xFFFF
	return _fixed24_to_hhmm((raw << Q8_8_TO_FIXED24_SHIFT) % TOD_DAY_FIXED24)


## Advance by completed logic ticks using the exact integer increment
## [orig: Env_TodAdvancePerTick =
## 0x18000000 / (3720 * minutes_per_day) @ 0x57d108].
func advance_mission_clock(ticks: int) -> void:
	if ticks <= 0:
		return
	_mission_time_fixed24 = (
		_mission_time_fixed24 + ticks * _mission_advance_per_tick
	) % TOD_DAY_FIXED24
	time_of_day = _fixed24_to_hhmm(_mission_time_fixed24)


## Public debug clock seam. The catalog exposes minute-of-day rather than raw
## HHMM so its linear slider/JSON range contains no impossible values such as
## 12:79. Updating the fixed-point accumulator is essential: merely assigning
## time_of_day would be overwritten by the next world-driven weather tick.
func debug_set_mission_minute_of_day(minute_of_day: float) -> Error:
	if not is_finite(minute_of_day) \
			or minute_of_day < 0.0 \
			or minute_of_day >= CLOCK_MINUTES_PER_DAY:
		return ERR_INVALID_PARAMETER
	_mission_time_fixed24 = int(roundf(
			minute_of_day * float(FIXED24_ONE_HOUR) / MINUTES_PER_HOUR
	)) % TOD_DAY_FIXED24
	time_of_day = minute_of_day_to_hhmm(minute_of_day)
	return OK


func get_mission_minute_of_day() -> float:
	return hhmm_to_minute_of_day(time_of_day)


static func minute_of_day_to_hhmm(minute_of_day: float) -> float:
	var wrapped := fposmod(minute_of_day, CLOCK_MINUTES_PER_DAY)
	var hour := floorf(wrapped / MINUTES_PER_HOUR)
	return hour * HHMM_HOUR_SCALE + fposmod(wrapped, MINUTES_PER_HOUR)


static func hhmm_to_minute_of_day(hhmm: float) -> float:
	var wrapped := fposmod(hhmm, HHMM_DAY)
	var hour := floorf(wrapped / HHMM_HOUR_SCALE)
	return hour * MINUTES_PER_HOUR + (wrapped - hour * HHMM_HOUR_SCALE)


static func _fixed24_to_hhmm(value: int) -> float:
	return _hours_to_hhmm(float(value) / float(FIXED24_ONE_HOUR))


static func _hours_to_hhmm(hours: float) -> float:
	var wrapped := fposmod(hours, float(HOURS_PER_DAY))
	var hour := floorf(wrapped)
	return hour * HHMM_HOUR_SCALE + (wrapped - hour) * MINUTES_PER_HOUR


func _ensure_loaded() -> void:
	if environment_data and not environment_data.is_loaded():
		var err := environment_data.load()
		if err != OK:
			push_warning("NovaEnvironment: failed to load .env: ", err)


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
	if not _weather_driven and not _tod.is_empty():
		# Standalone (no weather tick): this node owns the current render
		# colors directly. Weather-driven, these fields belong to the smoothed
		# writeback — writing raw keyframes here would fight it (see
		# _weather_driven).
		var light_key := "moon" if _is_night else "sun"
		_sun_light = _tod.get(light_key, _sun_light)
		_fill_light = _tod.get("ground", _fill_light)
		_sky_ambient_rt = _tod.get("sky", _sky_ambient_rt)
		_sky_base_rt = _tod.get("skybase", _sky_base_rt)
		_sky_bright_rt = _tod.get("skybright", _sky_bright_rt)
		_sky_highlight_rt = _tod.get("skyhighlight", _sky_highlight_rt)
		_cloud_base_rt = _tod.get("cloudbase", _cloud_base_rt)
		_cloud_highlight_rt = _tod.get("cloudhighlight", _cloud_highlight_rt)
		_cloud_edge_rt = _tod.get("cloudedge", _cloud_edge_rt)
		_ceiling_color_rt = get_ceiling_color_target()
		_cloud_tint_rt = get_cloud_tint_target()
		_floor_color_rt = get_floor_color_target()
		# Fog/skyfog blocks operate in undoubled authored bytes. Standalone
		# owners derive the same post-blend doubled colors that NovaWeather
		# writes after its block tick.
		var fog_raw: Vector3 = _tod.get("fog", _fog_color_rt * 0.5)
		_fog_color_rt = _double_vec3(fog_raw)
		_skyfog_color_rt = _derive_skyfog_render_color(
			fog_raw, _tod.get("skyfog", fog_raw), get_fog_level())
	if not _weather_driven:
		_fog_distance = environment_data.get_fog_level()
	# A TOD recompute can move any object-consumed value (weather-driven, the
	# moving targets flow through the smoothers instead); the bump keeps
	# material restamps tracking either way.
	_env_generation += 1
	if not _weather_driven:
		_write_shader_globals()


## NovaWeather marks itself the owner of the current render colors / smoothed
## scalars / shader globals (see _weather_driven). One-way for the node's life:
## the weather tick writes back every frame from then on.
func set_weather_driven(driven: bool) -> void:
	_weather_driven = driven


func is_weather_driven() -> bool:
	return _weather_driven


static func _double_vec3(value: Vector3) -> Vector3:
	var doubled := EnvFile.double_saturate_color(Color(value.x, value.y, value.z))
	return Vector3(doubled.r, doubled.g, doubled.b)


static func _derive_skyfog_render_color(
		fog_raw: Vector3, skyfog_raw: Vector3, fog_distance: float) -> Vector3:
	var blended := EnvFile.horizon_blend_skyfog(
		Color(fog_raw.x, fog_raw.y, fog_raw.z),
		Color(skyfog_raw.x, skyfog_raw.y, skyfog_raw.z),
		fog_distance, 1024.0)
	return _double_vec3(Vector3(blended.r, blended.g, blended.b))


## Global (non-TOD) colors stay raw on EnvFile so editor/export round-trips do
## not bake envscale into authored values. The runtime view performs the same
## byte quantize -> envscale truncation -> saturation as the retail parser
## [orig: Color_ScaleRGBAndPack @ 0x57f890; divergence #8].
static func _scale_global_channel(channel: float, envscale: float) -> float:
	var authored_byte := clampi(int(channel * 255.0 + 0.5), 0, 255)
	var scaled_byte := clampi(int(float(authored_byte) * envscale), 0, 255)
	return float(scaled_byte) / 255.0


static func _scale_global_color(color: Color, envscale: float) -> Vector3:
	return Vector3(
		_scale_global_channel(color.r, envscale),
		_scale_global_channel(color.g, envscale),
		_scale_global_channel(color.b, envscale))


func _global_color_target(color: Color) -> Vector3:
	var envscale := environment_data.get_envscale() if environment_data != null else 1.0
	return _scale_global_color(color, envscale)


func _write_shader_globals() -> void:
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", get_fill_light())
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
	return _apply_nvg_hemi_gain(_fill_light) if _nvg_view_active else _fill_light


## The SMOOTHED sky block when the weather tick drives it — written back per
## tick like fill/sun/fog, so object hemi_sky serves the post-modulator block
## [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 reads
## Env_SkyBlock[0]; the blocks smooth + modulate in the weather tick
## @ 0x57ef97..0x57f03c]. Discrete TOD recomputes re-seed it from the keyframe
## (like _fill_light); the chase target stays get_sky_ambient_target().
func get_sky_ambient() -> Vector3:
	return _apply_nvg_hemi_gain(_sky_ambient_rt) if _nvg_view_active else _sky_ambient_rt


## Applies the first-person-visible NVG hemisphere rewrite. _color_src_gain is
## the retail modulator byte unpacked as /64, so modulator*f/640 becomes
## _color_src_gain*f/10 here [orig: NVG world-light gain rewrite].
func _apply_nvg_hemi_gain(color: Vector3) -> Vector3:
	var f := float(_nvg_gain + 1) * 0.2
	return color * (0.25 * f) + _color_src_gain * (f / 10.0)


## active is already gated to the view where retail applies NVG lighting
## (first person). The raw NVG toggle remains simulation-owned.
func set_nvg_view(active: bool, gain: int) -> void:
	var clamped_gain := clampi(gain, 0, 4)
	if active == _nvg_view_active and clamped_gain == _nvg_gain:
		return
	_nvg_view_active = active
	_nvg_gain = clamped_gain
	_env_generation += 1
	# NovaWeather owns the full per-frame global write while present. Refresh
	# only the two affected channels immediately, and let its next tick publish
	# the same getter-derived values again without disturbing wind/fog state.
	RenderingServer.global_shader_parameter_set(&"opennova_fill_light", get_fill_light())
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", get_sky_ambient())


func get_fog_color() -> Vector3:
	return _fog_color_rt


## The TOD keyframe targets the weather smoothers chase — read straight from
## the interpolated keyframe state, never from the smoothed values written
## back by NovaWeather [orig: Environment_ComputeTimeOfDayColors @ 0x57de40
## refreshes every color block's target slot each frame; the smoothers chase
## keyframe colors, not their own output].
func get_fill_light_target() -> Vector3:
	return _tod.get("ground", _fill_light)


func get_sun_light_target() -> Vector3:
	var light_key := "moon" if _is_night else "sun"
	return _tod.get(light_key, _sun_light)


func get_fog_color_target() -> Vector3:
	# The render fog target is the keyframe color doubled, saturating
	# [orig: Environment_UpdateWeatherTick @ 0x57f17c].
	return _double_vec3(_tod.get("fog", _fog_color_rt * 0.5))


func get_fog_color_base_target() -> Vector3:
	# WeatherColorBlock chases the authored half-intensity color; doubling is
	# the derived tail after smoothing/modulation [orig: @ 0x57f17c].
	return _tod.get("fog", _fog_color_rt * 0.5)


func get_sky_ambient_target() -> Vector3:
	return _tod.get("sky", Vector3(0.3, 0.4, 0.6))


func get_skyfog_color_target() -> Vector3:
	return _tod.get("skyfog", Vector3.ZERO)


func get_ceiling_color_target() -> Vector3:
	if environment_data == null:
		return _ceiling_color_rt
	return _global_color_target(environment_data.get_ceiling_color())


func get_cloud_tint_target() -> Vector3:
	if environment_data == null:
		return _cloud_tint_rt
	return _global_color_target(environment_data.get_cloud_tint())


func get_floor_color_target() -> Vector3:
	if environment_data == null:
		return _floor_color_rt
	return _global_color_target(environment_data.get_floor_color())


func get_lightning_color_target() -> Vector3:
	if environment_data == null:
		return Vector3.ONE
	return _global_color_target(environment_data.get_lightning_color())


func get_sky_base_target() -> Vector3:
	return _tod.get("skybase", _sky_base_rt)


func get_sky_bright_target() -> Vector3:
	return _tod.get("skybright", _sky_bright_rt)


func get_sky_highlight_target() -> Vector3:
	return _tod.get("skyhighlight", _sky_highlight_rt)


func get_cloud_base_target() -> Vector3:
	return _tod.get("cloudbase", _cloud_base_rt)


func get_cloud_highlight_target() -> Vector3:
	return _tod.get("cloudhighlight", _cloud_highlight_rt)


func get_cloud_edge_target() -> Vector3:
	return _tod.get("cloudedge", _cloud_edge_rt)


func get_terrain_tint() -> Vector3:
	if environment_data == null:
		return Vector3.ONE
	var color := environment_data.get_terrain_tint()
	return Vector3(color.r, color.g, color.b)


func get_terrain_lighting_attenuation() -> Vector3:
	# Identity is FAITHFUL for the terrain surface: the terrain texture bake
	# consumer of terrain_rgb is DEAD CODE in retail — the bake buffer is
	# written and freed but its three readers (0x606ce0, 0x606c30,
	# Terrain_GetColorMapBilinear @ 0x606d80) have zero xrefs (full .text
	# E8/E9 scan), so the GPU terrain textures ship untinted
	# (docs/env/env-tod-re.md #19). The live terrain_rgb consumer in this path is
	# the .til tile overlay (get_tile_overlay_tint below); the re-grilled foliage
	# lightmap constant is its neutral :fd/detail average, not terrain_rgb.
	return Vector3.ONE


## The .til tile-overlay tint: DIFFUSE = HALF(terrain_rgb) on the quad under a
## TEXTURE x DIFFUSE MODULATE2X combine, folded to one shader multiply —
## 254/255 at the default tint (witnessed near-identity, one LSB dark).
## [orig: PolyTrn_SetTerrainTintColors @ 0x605e20; PolyTrn_RenderTile @ 0x60df0d]
func get_tile_overlay_tint() -> Vector3:
	var tint := Color.WHITE
	if environment_data != null:
		tint = environment_data.get_terrain_tint()
	var factor := EnvFile.tile_overlay_tint_factor(tint)
	return Vector3(factor.r, factor.g, factor.b)


## Push the env-derived terrain lighting + fog uniforms onto a terrain
## ShaderMaterial. terrain.gdshader (runtime) and terrain_editor.gdshader (editor
## preview) share these uniforms via terrain_lighting.gdshaderinc, so both drive
## them through here. Callers may override individual values afterwards (the
## runtime layers NovaWeather-smoothed colors + the tile-overlay tint on top).
func apply_terrain_uniforms(material: ShaderMaterial) -> void:
	if material == null:
		return
	# c1 <- the light block, c0 <- the sky block — the witnessed terrain PS
	# constants (fill/ground does not reach the terrain surface)
	# [orig: terrain_setup_lighting_and_shader @ 0x604420;
	#  init_terrain_lighting_color_ramps @ 0x604ee0].
	material.set_shader_parameter("u_sun_light", get_sun_light())
	material.set_shader_parameter("u_sky_ambient", get_sky_ambient())
	material.set_shader_parameter("u_sun_direction", get_light_direction())
	material.set_shader_parameter("u_tile_overlay_tint", get_tile_overlay_tint())
	material.set_shader_parameter("u_fog_color", get_fog_color())
	material.set_shader_parameter("u_fog_end", get_fog_level())
	material.set_shader_parameter("u_fog_start", get_fog_start())
	material.set_shader_parameter("u_fog_type", get_fog_type())


func get_water_color() -> Vector3:
	if environment_data == null:
		return Vector3(0.408, 0.314, 0.224)
	return _global_color_target(environment_data.get_water_color())


func has_water_height() -> bool:
	return environment_data != null and environment_data.has_water_height()


func get_water_height() -> float:
	return environment_data.get_water_height() if has_water_height() else 0.0


func get_cloud_tint() -> Vector3:
	return _cloud_tint_rt


func get_ceiling_color() -> Vector3:
	return _ceiling_color_rt


func get_floor_color() -> Vector3:
	return _floor_color_rt


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
	return _sky_base_rt


func get_sky_bright() -> Vector3:
	return _sky_bright_rt


func get_sky_highlight() -> Vector3:
	return _sky_highlight_rt


## Cloud-pass color blocks (sky dome VS constants c24/c27/c26)
## [orig: render_skybox uploads @ 0x57934a..0x57936f].
func get_cloud_base() -> Vector3:
	return _cloud_base_rt


func get_cloud_highlight() -> Vector3:
	return _cloud_highlight_rt


func get_cloud_edge() -> Vector3:
	return _cloud_edge_rt


func get_skyfog_color() -> Vector3:
	# Weather-owned, horizon-blended in undoubled block space, then doubled.
	# Both the dome fog and the modulate2x frame clear consume this value.
	return _skyfog_color_rt


func get_frame_clear_color() -> Vector3:
	# The frame CLEAR color (divergence #21, closed): skyfog horizon-blended
	# toward fog when the fog distance drops below half the reference distance -
	# pure fog at <= ref/4, a linear fade across [ref/4, ref/2], untouched
	# skyfog above. The blend's distance input is the SMOOTHED fog-distance
	# current served by get_fog_level() (env #27) [orig: Env_FogDistCurrent
	# @ 0x26c681c]. The blend runs on the UNDOUBLED keyframe colors, THEN the
	# result doubles with saturation (the witnessed order) - this is the
	# POST-BLEND DOUBLED skyfog render color, and the modulate2x-path device
	# Clear consumes it VERBATIM. That is the path this reimpl reproduces
	# everywhere (D-RMAT-7 calibrate proof: the x2 fixed-function combine and
	# the doubled fog/skyfog render colors are in our shaders), and the dome
	# pass fogs toward the SAME doubled skyfog - the dome-rim/clear seam is
	# invisible because both sides converge on this one value. The halving
	# branch in the device Clear is the non-modulate2x compat fallback, with
	# NO reimpl analog. Reference distance: the retail default 1024 (the
	# session authority forces it; 768 is an adapter-caps fallback with no
	# reimpl analog).
	# [orig: Environment_UpdateWeatherTick @ 0x57e9b0 blend @ 0x57f037..0x57f0a1,
	#  doubling @ 0x57f1b1; consumer Render_ProcessMainSceneFrame
	#  @ 0x5ca776..0x5ca7bf; dome fog toward the same value sub_579CB0; device
	#  Clear @ 0x677100, its non-modulate2x halving fallback @ 0x67715d;
	#  defaults Environment_InitDefaults @ 0x57c0b0 / Terrain_Init @ 0x60fca3]
	return _skyfog_color_rt


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


func set_sky_ambient_rt(value: Vector3) -> void:
	if value != _sky_ambient_rt:
		_sky_ambient_rt = value
		_env_generation += 1


func set_static_colors_rt(ceiling: Vector3, cloud: Vector3, floor_color: Vector3) -> void:
	var changed := (
		_ceiling_color_rt != ceiling
		or _cloud_tint_rt != cloud
		or _floor_color_rt != floor_color
	)
	_ceiling_color_rt = ceiling
	_cloud_tint_rt = cloud
	_floor_color_rt = floor_color
	# cloud_rgb is sampled by the flat dome every frame; ceiling/floor are live
	# indoor-light inputs. Keep the shared material-generation seam honest for
	# any additional consumers that cache environment values.
	if changed:
		_env_generation += 1


func set_sky_colors_rt(
		skyfog: Vector3, sky_base: Vector3, sky_bright: Vector3,
		sky_highlight: Vector3, cloud_base: Vector3,
		cloud_highlight: Vector3, cloud_edge: Vector3) -> void:
	var skyfog_changed := _skyfog_color_rt != skyfog
	_skyfog_color_rt = skyfog
	_sky_base_rt = sky_base
	_sky_bright_rt = sky_bright
	_sky_highlight_rt = sky_highlight
	_cloud_base_rt = cloud_base
	_cloud_highlight_rt = cloud_highlight
	_cloud_edge_rt = cloud_edge
	# GameWorld generation-gates the clear-color push; the six dome ramps are
	# sampled directly every frame, but the shared skyfog/clear value must wake
	# that gate when its weather block moves.
	if skyfog_changed:
		_env_generation += 1


## The modulator /64 gain (the iris auto-exposure reaching shaders), written
## back per tick by NovaWeather like the smoothed colors — ColorSrcGlobalGain
## [orig: Render_UnpackModulatorToLightScale @ 0x58db30; bind @ 0x58e05d].
func set_color_src_gain(value: Vector3) -> void:
	if value != _color_src_gain:
		_color_src_gain = value
		_env_generation += 1


func get_color_src_gain() -> Vector3:
	return _color_src_gain


## Monotonic generation, bumped only when a lighting/fog value object materials read actually
## changes. Lets a NovaObjectModel skip its per-material environment push with one int compare.
func get_env_generation() -> int:
	return _env_generation


# env #27 smoothed scalar currents (negative = not driven; parsed fallback).
var _fog_dist_smoothed: float = -1.0
var _sky_height_smoothed: float = -1.0
var _sun_dim_smoothed: float = 0.0


func get_fog_distance() -> float:
	return _fog_distance


func get_fog_level() -> float:
	# The SMOOTHED fog distance when the weather tick drives it (env #27): the
	# scrub/keyframe value is the spring TARGET, the served value ramps
	# [orig: Env_FogDistCurrent @ 0x26c681c <- the (d+31)>>5 spring
	# @ 0x57edd7; targets-only snap @ 0x57d1e0]. Every consumer (dome c9,
	# water UV state, object/terrain fog ends, the frame clear) reads through
	# here, so the ramp reaches them all.
	if _fog_dist_smoothed >= 0.0:
		return _fog_dist_smoothed
	return environment_data.get_fog_level() if environment_data else 1000.0


func get_fog_level_target() -> float:
	# The parsed .env value — the spring target the weather tick chases.
	return environment_data.get_fog_level() if environment_data else 1000.0


func get_fog_start() -> float:
	# Policy lives in libs/env env_render [orig: Render_SetFogState @ 0x58a950].
	# Consume the smoothed CURRENT end so both bounds stay on the same curve
	# during the 62 Hz fog spring; overcast remains 0 until weather drives it.
	return EnvFile.fog_start_for(get_fog_type(), get_fog_level(), 0.0)


func get_fog_type() -> int:
	return environment_data.get_fog_type() if environment_data else 2


func get_sky_speed() -> float:
	return environment_data.get_sky_speed() if environment_data else 15.0


func get_sky_height() -> float:
	# The SMOOTHED sky height when the weather tick drives it (env #27):
	# retail eighth-snaps toward the parsed value and rebuilds the dome only
	# as the SMOOTHED height moves [orig: Env_SkyHeightCurrent @ 0x26c6858
	# eighth-snap @ 0x57ee97; the dome rebuild gate @ 0x57e4f4].
	if _sky_height_smoothed >= 0.0:
		return _sky_height_smoothed
	return environment_data.get_sky_height() if environment_data else 175.0


func get_sky_height_target() -> float:
	# The parsed .env value — the eighth-snap target.
	return environment_data.get_sky_height() if environment_data else 175.0


## env #27: the weather tick pushes the smoothed scalar currents back here
## (the same writeback seam as the smoothed colors), so every scalar
## consumer serves the ramped values.
func set_smoothed_scalars(fog_distance: float, sky_height: float, sun_dim_pct: float = 0.0) -> void:
	var fog_changed := _fog_dist_smoothed != fog_distance
	_fog_dist_smoothed = fog_distance
	_sky_height_smoothed = sky_height
	_sun_dim_smoothed = sun_dim_pct
	if fog_changed:
		_env_generation += 1


## The smoothed Env_SunDimPct channel (0..100; default 0 — nothing writes the
## target in stock data) — dims the sun body + glare
## [orig: @ 0x26c6830 spring @ 0x57ee17; consumers @ 0x5acbc1/0x5acfb8].
func get_sun_dim_pct() -> float:
	return _sun_dim_smoothed


func get_sky_map1_tex() -> Texture2D:
	return environment_data.get_sky_map1_tex() if environment_data else null


func get_sky_map2_tex() -> Texture2D:
	return environment_data.get_sky_map2_tex() if environment_data else null


func get_environment_data() -> EnvFile:
	return environment_data
