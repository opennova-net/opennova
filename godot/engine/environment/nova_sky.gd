@tool
class_name NovaSky
extends Node3D

# Sky dome adapter for the environment subsystem.
# Engine equivalents: sky-dome mesh/render path of [orig: build_sky_dome_mesh @ 0x578db0]
# and [orig: render_skybox @ 0x579080], fed by [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]
# interpolated TOD colors; see docs/env/env-tod-re.md.

@export var environment_path: NodePath
# The weather node owning the cloud-scroll rate + accumulators (duck-typed
# like NovaTerrain's weather_path; resolved lazily in _process).
@export var weather_path: NodePath

# Optional owner-supplied BG_COLOR resource. WorldContextPreview supplies this so
# the dome's faithful below-rim region clears to skyfog instead of black.
var frame_clear_environment: Environment = null

var mesh_instance: MeshInstance3D
var sky_material: ShaderMaterial
var built: bool = false
var _bound_cloud_tex1: Texture2D = null
var _bound_cloud_tex2: Texture2D = null
var _cached_env: Node = null
var _cached_weather: Node = null
var _cached_cam: Camera3D = null
# Standalone fallback (owners with no weather node): a private core ticked for
# its cloud scroll only — the integer math has ONE home either way.
var _fallback_scroll: NovaWeatherCore = null
var _fallback_tick_credit := 0.0


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	build()


func build() -> void:
	if mesh_instance:
		mesh_instance.queue_free()
		mesh_instance = null
	built = false
	_bound_cloud_tex1 = null
	_bound_cloud_tex2 = null

	var sky_shader := load("res://shaders/sky.gdshader") as Shader
	sky_material = ShaderMaterial.new()
	sky_material.shader = sky_shader

	# The witnessed 21x21 dome (441 verts / 800 tris, libs/env math), built
	# ONCE at the reference height: the Y-only height scale + anisotropic
	# normals are applied in the vertex shader, so height changes never
	# rebuild (env #20's ratified fold; retail re-bakes per smoothed-height
	# change) [orig: build_sky_dome_mesh @ 0x578db0].
	var arrays: Array = EnvFile.build_sky_dome_arrays(EnvFile.dome_reference_height())
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, sky_material)

	mesh_instance = MeshInstance3D.new()
	# Retail submits the sky in each render pass; the shader reanchors to that
	# pass camera. Keep CPU culling from rejecting reflection-pass relocation
	# before the vertex stage [orig: render_skybox @ 0x5790d0].
	mesh_instance.extra_cull_margin = 1.0e6
	mesh_instance.ignore_occlusion_culling = true
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mesh_instance.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	mesh_instance.mesh = mesh
	add_child(mesh_instance)
	built = true


func _process(delta: float) -> void:
	if not built or sky_material == null:
		return
	# Camera3D.current changes when ONED switches workspace cameras while the
	# old camera can remain alive. Re-resolve that transition instead of
	# continuing to follow a stale, still-in-tree camera.
	if not _cached_cam or not _cached_cam.is_inside_tree() or not _cached_cam.current:
		_cached_cam = EnvRenderCamera.find(self)
	if _cached_cam and mesh_instance:
		# The dome follows the camera in xz and rides at HALF the camera height
		# [orig: render_skybox @ 0x5790d0 - world translation z = camHeight >> 1].
		var cam_pos := _cached_cam.global_position
		mesh_instance.global_position = Vector3(cam_pos.x, cam_pos.y * 0.5, cam_pos.z)

	var sky_speed := 15.0
	var sky_height := 175.0
	sync_frame_clear_color()
	var env := _cached_env
	if env and env.has_method("is_loaded") and env.is_loaded():
		var env_data: EnvFile = env.get_environment_data()
		if env_data and env_data.get_advanced_clouds() == 0:
			# Flat cloud-color dome: the original applies the pass-1 NULL-texture
			# effect with material ambient = cloud_rgb against D3DRS_AMBIENT
			# 0xFFFFFF - the only render path that reads cloud_rgb
			# [orig: render_skybox @ 0x579b42..0x579bb6].
			sky_material.set_shader_parameter("u_flat_pass", true)
			sky_material.set_shader_parameter("u_flat_color", env.get_cloud_tint())
		else:
			sky_material.set_shader_parameter("u_flat_pass", false)
			# Retail unpacks these six packed ENV blocks at 2/255, without
			# clamping the uploaded constants [orig: Color_UnpackToFloat4
			# @ 0x578985; six call sites @ 0x579312..0x57936f]. Fog does not
			# use this helper: D3DRS_FOGCOLOR consumes its packed byte color.
			sky_material.set_shader_parameter("u_sky_base", _sky_constant(env.get_sky_base()))
			sky_material.set_shader_parameter("u_sky_bright", _sky_constant(env.get_sky_bright()))
			sky_material.set_shader_parameter("u_sky_highlight", _sky_constant(env.get_sky_highlight()))
			sky_material.set_shader_parameter("u_cloud_base", _sky_constant(env.get_cloud_base()))
			sky_material.set_shader_parameter("u_cloud_highlight", _sky_constant(env.get_cloud_highlight()))
			sky_material.set_shader_parameter("u_cloud_edge", _sky_constant(env.get_cloud_edge()))

		# Pass 1 is always sun-driven; the cloud pass follows the active light,
		# moon at night [orig: render_skybox @ 0x579287 Terrain_GetSunDirectionAsFloat
		# vs @ 0x579291 Environment_GetLightDirectionFloat].
		sky_material.set_shader_parameter("u_sun_dir", env.get_sun_direction())
		sky_material.set_shader_parameter("u_light_dir", env.get_light_direction())
		# The sky pass temporarily swaps the device fog color from world fog to
		# the post-horizon-blend, doubled skyfog block, then restores world fog.
		# [orig: sky fog wrapper @ 0x579cb0].
		sky_material.set_shader_parameter("u_fog_color", env.get_skyfog_color())
		sky_material.set_shader_parameter("u_fog_end", env.get_fog_level())
		sky_speed = env.get_sky_speed()
		sky_height = env.get_sky_height()
		sky_material.set_shader_parameter("u_sky_height", sky_height)

	_update_cloud_textures(env)

	# Cloud scroll [orig: Environment_UpdateWeatherTick rate ramp @ 0x57eecc +
	# accumulators @ 0x57f1a5..0x57f1d1; consumed render_skybox
	# @ 0x5791de..0x579260]: the weather core owns the ramping rate and the
	# four integer accumulators; the UV translation is U = +cam/4096 - acc*2^-28,
	# V = +cam/4096 + acc*2^-28 (layer 2: /8192 and 2^-29) - the accumulator
	# rides U NEGATIVELY (env #26).
	var scroll_source: Object = _scroll_source(sky_speed, delta)
	var cam_x := 0.0
	var cam_z := 0.0
	if _cached_cam:
		cam_x = _cached_cam.global_position.x
		cam_z = _cached_cam.global_position.z
	sky_material.set_shader_parameter("u_scroll_offset1", scroll_source.get_cloud_uv_offset1(cam_x, cam_z))
	sky_material.set_shader_parameter("u_scroll_offset2", scroll_source.get_cloud_uv_offset2(cam_x, cam_z))

static func _sky_constant(value: Vector3) -> Vector3:
	return value * 2.0


func _update_cloud_textures(env: Node) -> void:
	var tex1: Texture2D = null
	var tex2: Texture2D = null
	if env and env.has_method("is_loaded") and env.is_loaded():
		tex1 = env.get_sky_map1_tex()
		tex2 = env.get_sky_map2_tex()
	if tex1 == _bound_cloud_tex1 and tex2 == _bound_cloud_tex2:
		return
	_bound_cloud_tex1 = tex1
	_bound_cloud_tex2 = tex2
	var has_clouds := tex1 != null or tex2 != null
	if has_clouds:
		sky_material.set_shader_parameter("u_cloud_tex1", tex1 if tex1 else tex2)
		sky_material.set_shader_parameter("u_cloud_tex2", tex2 if tex2 else tex1)
	sky_material.set_shader_parameter("u_has_clouds", has_clouds)


# The faithful sky dome is open below its rim. Retail clears that region to
# the horizon-blended skyfog block; editor shells provide the BG_COLOR resource.
func sync_frame_clear_color() -> void:
	if frame_clear_environment == null:
		return
	if not _cached_env or not _cached_env.is_inside_tree():
		_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	var env := _cached_env
	if env == null or not env.has_method("is_loaded") or not env.is_loaded() or not env.has_method("get_frame_clear_color"):
		return
	var rgb: Vector3 = env.get_frame_clear_color()
	frame_clear_environment.background_color = Color(rgb.x, rgb.y, rgb.z)


# The weather node when wired (it ticks the shared core at process priority
# -10, before us), else a private fallback core advanced at the same fixed
# 62 Hz cadence. Rendering may run at any refresh rate.
func _scroll_source(sky_speed: float, delta: float) -> Object:
	if not _cached_weather or not _cached_weather.is_inside_tree():
		_cached_weather = get_node_or_null(weather_path) if not weather_path.is_empty() else null
	if _cached_weather and _cached_weather.has_method("get_cloud_uv_offset1"):
		return _cached_weather
	if _fallback_scroll == null:
		_fallback_scroll = NovaWeatherCore.new()
	_fallback_tick_credit += maxf(delta, 0.0) * NovaWeather.WEATHER_TICK_HZ
	var tick_count := int(floor(_fallback_tick_credit + 1.0e-9))
	if tick_count > 0:
		_fallback_tick_credit = maxf(0.0, _fallback_tick_credit - float(tick_count))
		if tick_count > NovaWeather.MAX_CATCHUP_TICKS:
			tick_count = NovaWeather.MAX_CATCHUP_TICKS
			_fallback_tick_credit = 0.0
		for _tick in range(tick_count):
			_fallback_scroll.tick_cloud_scroll(sky_speed)
	return _fallback_scroll


