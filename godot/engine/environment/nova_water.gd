@tool
class_name NovaWater
extends Node3D

# Water plane preview/runtime adapter.
# Engine equivalents: water color/height/murk consume globals parsed by
# [orig: TimeOfDay_ParseProperty @ 0x57c590] and fog colors interpolated by
# [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] (docs/env/env-tod-re.md).

@export var environment_path: NodePath
# The weather node owning the cloud-scroll core: the water scroll speed rides
# the same RAMPING rate the sky layers consume (duck-typed, lazy resolve).
@export var weather_path: NodePath
@export var terrain_data: NovaTerrainData:
	set(value):
		terrain_data = value
		# Terrain carries the map's water height. It's assigned after the .trn loads
		# — long after _ready — so recompute here too, or the plane stays at the
		# scene default instead of dropping to the map's level. A present terrain
		# height beats the .env one (witnessed precedence, env #28).
		_recompute_terrain_water_fallback()
		_apply_environment_water_height()
@export_range(-100, 200, 0.1) var water_height: float = 0.0:
	set(value):
		water_height = value
		# The strip vertices carry the plane height themselves (the mesh node
		# stays pinned at the world origin) — no node repositioning here.
		_push_water_split_height()
@export_range(0, 1, 0.01) var water_alpha: float = 0.6

# When set (not NaN), the host drives water height directly and the env/terrain
# fallback is ignored — the terrain editor authors height through its document.
var _height_override: float = NAN


func set_height_override(value: float) -> void:
	_height_override = value
	if not is_nan(value):
		water_height = value


# Publish this water plane's height as the session's transparent water-split
# (the g_WaterSplitHeightFloat equivalent [orig: @ 0x5c93e2..0x5c93f0]) so
# blended world materials can take their far/camera-side rung; cleared when
# the water node leaves the tree.
func _push_water_split_height() -> void:
	if built and is_inside_tree():
		NovaObjectShaderCache.get_singleton().set_water_split_height(water_height)


func _exit_tree() -> void:
	NovaObjectShaderCache.get_singleton().clear_water_split_height()

var mesh_instance: MeshInstance3D
var water_material: ShaderMaterial
var built: bool = false
var _cached_env: Node = null
var _cached_weather: Node = null
var _cached_cam: Camera3D = null
var _terrain_water_height: float = 0.0
# The witnessed per-frame water core: the noise texture pair
# [orig: render_water_surface @ 0x5c32c0 -> Water_GenerateNoiseTextures
# @ 0x5c0360] plus the screen-marched strip tessellation (env #29)
# [orig: render_water_strip_detailed @ 0x5c27d0]; math in libs/env.
var _water_core := NovaWaterCore.new()
var _noise_color_img: Image = null
var _noise_normal_img: Image = null
var _noise_color_tex: ImageTexture = null
var _noise_normal_tex: ImageTexture = null
var _frame_counter: int = 0
# Standalone fallback when no weather node is wired (the UV offsets ride the
# weather core's cloud-scroll accumulators).
var _fallback_scroll: NovaWeatherCore = null


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	_recompute_terrain_water_fallback()
	if _terrain_water_height != 0.0:
		water_height = _terrain_water_height
	_apply_environment_water_height()
	build()


# The map's water height (engine half-world units) from the loaded terrain.
# Retail stores the terrain value AFTER the .env parse when its bit-31 "has
# water" flag is set [orig: Terrain_Init @ 0x60fcb1..0x60fcba], so a present
# terrain height BEATS the .env one (env #28). Our .trn text config proxies
# "flagged" as a nonzero value. Zero when there's no terrain or no water.
func _recompute_terrain_water_fallback() -> void:
	_terrain_water_height = 0.0
	if terrain_data and terrain_data.is_loaded():
		var raw := terrain_data.get_water_height()
		if raw > 0:
			_terrain_water_height = float(raw) * 0.5


func build() -> void:
	if mesh_instance:
		mesh_instance.queue_free()
		mesh_instance = null
	built = false
	water_material = ShaderMaterial.new()
	water_material.shader = load("res://shaders/water.gdshader") as Shader
	# The water surface draws between the two water-side transparent brackets
	# [orig: Terrain_RenderWaterPass @ 0x610640 between the SortAndFlush pair
	# @ 0x5c9596 / @ 0x5c967a; ladder in libs/renderer/render_order, REN-3].
	water_material.render_priority = NovaObjectShaderCache.RENDER_RUNG_WATER

	# The witnessed screen-marched strip mesh is LIVE (env #29): _process
	# rebuilds the surface every frame from NovaWaterCore.strip_build
	# [orig: render_water_strip_detailed @ 0x5c27d0], so the mesh starts
	# empty. Remaining variants: the LOW tier (render_water_strip @ 0x5c1d60,
	# water detail <= 1 — sin-table Y displacement) and the reflection RTT
	# consumer (env #30).
	var mesh := ArrayMesh.new()
	mesh_instance = MeshInstance3D.new()
	mesh_instance.mesh = mesh
	# Strip vertices are ABSOLUTE world positions (the plane height rides the
	# rows, not the node): pin the mesh at the world origin; top_level guards
	# against a transformed host parent.
	mesh_instance.top_level = true
	mesh_instance.position = Vector3.ZERO
	add_child(mesh_instance)
	built = true
	_push_water_split_height()

	# The witnessed per-frame noise texture pair (created once, updated per
	# frame) [orig: Water_GenerateNoiseTextures @ 0x5c0360].
	var size := _water_core.get_texture_size()
	_water_core.update(0)
	_noise_color_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _water_core.get_color_rgba8())
	_noise_normal_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _water_core.get_normal_rgba8())
	_noise_color_tex = ImageTexture.create_from_image(_noise_color_img)
	_noise_normal_tex = ImageTexture.create_from_image(_noise_normal_img)
	water_material.set_shader_parameter("u_noise_color", _noise_color_tex)
	water_material.set_shader_parameter("u_noise_normal", _noise_normal_tex)


func _process(_delta: float) -> void:
	if not built or water_material == null:
		return
	# The murk uniform feed stays for host/probe compatibility even though the
	# shader's murk role moved to the per-vertex COLOR.a (env #29).
	water_material.set_shader_parameter("u_water_murk", water_alpha)

	if not _cached_cam or not _cached_cam.is_inside_tree():
		_cached_cam = _find_camera()
	var cam_pos := Vector3.ZERO
	if _cached_cam:
		cam_pos = _cached_cam.global_position

	# Regenerate the animated noise pair, one tick per rendered frame like the
	# weather core [orig: render_water_surface @ 0x5c3326 regenerates per frame].
	_frame_counter += 1
	_water_core.update(_frame_counter)
	_noise_color_img.set_data(_water_core.get_texture_size(), _water_core.get_texture_size(), false, Image.FORMAT_RGBA8, _water_core.get_color_rgba8())
	_noise_normal_img.set_data(_water_core.get_texture_size(), _water_core.get_texture_size(), false, Image.FORMAT_RGBA8, _water_core.get_normal_rgba8())
	_noise_color_tex.update(_noise_color_img)
	_noise_normal_tex.update(_noise_normal_img)

	# Strip inputs default to the shader-uniform stand-in values so hosts
	# without a loaded env still march strips.
	var murk := water_alpha
	var fog_end := 1000.0
	var uv_state := Vector4(1.0, 0.2, 0.0, 0.0)
	var lit := Color(0.408, 0.314, 0.224)
	var env_data: EnvFile = null

	var env := _cached_env
	if env and env.has_method("is_loaded") and env.is_loaded():
		_apply_environment_water_height()
		# Water renders lit: water_rgb x (light*0.707 + sky) x 2, saturating
		# [orig: Environment_UpdateWeatherTick @ 0x57f16b].
		var water: Vector3 = env.get_water_color()
		var light: Vector3 = env.get_sun_light()
		var sky: Vector3 = env.get_sky_ambient()
		var combined := EnvFile.combine_terrain_light(
				Color(light.x, light.y, light.z), Color(sky.x, sky.y, sky.z))
		lit = EnvFile.lit_water_color(Color(water.x, water.y, water.z), combined)
		water_material.set_shader_parameter("u_water_color", Vector3(lit.r, lit.g, lit.b))
		fog_end = env.get_fog_level()
		# The witnessed UV transform (scale/bias from the fog-distance INT
		# part, offsets from the layer-1 cloud accumulators + 32x camera)
		# [orig: render_water_surface @ 0x5c3348..0x5c33db]. The weather node
		# owns the shared accumulators; standalone hosts tick a private core.
		if not _cached_weather or not _cached_weather.is_inside_tree():
			_cached_weather = get_node_or_null(weather_path) if not weather_path.is_empty() else null
		if _cached_weather and _cached_weather.has_method("get_water_uv_state"):
			uv_state = _cached_weather.get_water_uv_state(cam_pos.x, cam_pos.z, fog_end)
		else:
			if _fallback_scroll == null:
				_fallback_scroll = NovaWeatherCore.new()
			_fallback_scroll.tick_cloud_scroll(env.get_sky_speed())
			uv_state = _fallback_scroll.get_water_uv_state(cam_pos.x, cam_pos.z, fog_end)
		water_material.set_shader_parameter("u_water_uv", uv_state)
		var fog_start: float = env.get_fog_start() if env.has_method("get_fog_start") else 0.5
		water_material.set_shader_parameter("u_fog_color", env.get_fog_color())
		water_material.set_shader_parameter("u_fog_start", fog_start)
		water_material.set_shader_parameter("u_fog_end", fog_end)
		water_material.set_shader_parameter("u_fog_type", env.get_fog_type())
		env_data = env.get_environment_data()
		if env_data:
			murk = env_data.get_water_murk()
			water_material.set_shader_parameter("u_water_murk", murk)

	_rebuild_strip_mesh(cam_pos, murk, fog_end, uv_state, lit, env_data)


# Rebuilds the surface from the witnessed screen march (env #29)
# [orig: render_water_strip_detailed @ 0x5c27d0; per-side callers
# render_water_surface @ 0x5c3492 (camera above) / @ 0x5c3542 (underwater)].
# Vertices are absolute world positions; COLOR carries the row diffuse,
# CUSTOM1 the row specular (the ps.1.1 v1 register [orig: add r0.rgb, r0, v1
# — Water_InitSurfaceShaders @ 0x5c19b0]), CUSTOM0 = (depth, rhw, screen U,
# screen V), TEX_UV the witnessed world/32 pair (carried for parity/debug —
# the shader keeps its camera-relative UV model).
func _rebuild_strip_mesh(cam_pos: Vector3, murk: float, fog_end: float,
		uv_state: Vector4, lit: Color, env_data: EnvFile) -> void:
	if mesh_instance == null or not (mesh_instance.mesh is ArrayMesh):
		return
	if _cached_cam == null or not _cached_cam.is_inside_tree():
		_clear_strip_surfaces()
		return
	var viewport := _cached_cam.get_viewport()
	if viewport == null:
		_clear_strip_surfaces()
		return
	var vp_size := Vector2i(viewport.get_visible_rect().size)
	if vp_size.x <= 1 or vp_size.y <= 1:
		_clear_strip_surfaces()
		return
	# The camera-side gate and the pass fog end both ride the underwater flag
	# [orig: side gate against Env_WaterHeightFixed in render_water_surface;
	#  Environment_GetFogEndDistance(underwater) @ 0x5c28a2 — below the
	#  surface the murk visibility curve replaces the weather fog distance].
	var underwater := cam_pos.y < water_height
	var pass_fog_end := fog_end
	if underwater and env_data:
		pass_fog_end = env_data.get_fog_end_underwater()
	_water_core.strip_set_view(_cached_cam.global_transform,
			_cached_cam.get_camera_projection(), vp_size, pass_fog_end)
	# The nightvision redraw variant is a FrameFX pass, not hosted yet.
	var rows: int = _water_core.strip_build(water_height, murk, lit,
			uv_state.x, uv_state.y, underwater, false)
	if rows < 2:
		# Plane off-screen or a sub-2-row march — nothing submits
		# [orig: windows under 2 rows draw nothing @ 0x5c3195].
		_clear_strip_surfaces()
		return
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = _water_core.strip_positions()
	arrays[Mesh.ARRAY_COLOR] = _water_core.strip_colors()
	arrays[Mesh.ARRAY_TEX_UV] = _water_core.strip_uv0()
	arrays[Mesh.ARRAY_CUSTOM0] = _water_core.strip_custom0()
	arrays[Mesh.ARRAY_CUSTOM1] = _water_core.strip_custom1()
	arrays[Mesh.ARRAY_INDEX] = _water_core.strip_indices()
	var mesh := mesh_instance.mesh as ArrayMesh
	mesh.clear_surfaces()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
			(Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT)
			| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT))
	mesh.surface_set_material(0, water_material)


func _clear_strip_surfaces() -> void:
	if mesh_instance == null:
		return
	var mesh := mesh_instance.mesh as ArrayMesh
	if mesh and mesh.get_surface_count() > 0:
		mesh.clear_surfaces()


func _apply_environment_water_height() -> void:
	# The witnessed precedence is BMS > TRN(flagged) > ENV [orig: env parse
	# @ 0x52073b, then Terrain_Init @ 0x60fcba overrides when flagged, then
	# the BMS override @ 0x525371] — the host override rung is the authoring
	# seam on top (env #28).
	if not is_nan(_height_override):
		water_height = _height_override
		return
	if _terrain_water_height != 0.0:
		water_height = _terrain_water_height
		return
	var env := _cached_env
	if env and env.has_method("has_water_height") and env.has_water_height():
		# .env water_height is stored <<15 by the engine — half world units,
		# same convention as the terrain value above
		# [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
		water_height = float(env.get_water_height()) * 0.5


func _find_camera() -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := get_viewport()
	return viewport.get_camera_3d() if viewport else null
