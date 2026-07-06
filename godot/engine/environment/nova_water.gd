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
		if mesh_instance:
			mesh_instance.position.y = water_height
@export_range(-100, 200, 0.1) var water_height: float = 0.0:
	set(value):
		water_height = value
		if mesh_instance:
			mesh_instance.position.y = water_height
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
# The witnessed per-frame noise texture pair [orig: render_water_surface
# @ 0x5c32c0 -> Water_GenerateNoiseTextures @ 0x5c0360]; math in libs/env.
var _noise_core := NovaWaterCore.new()
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

	# The 65x65 camera-snapped plane is the TRACKED stand-in for the witnessed
	# screen-marched strips (env #29 - spec complete in env-tod-re.md); the
	# shader computes the witnessed camera-relative UVs itself, so the mesh
	# carries none.
	const GRID := 65
	const CELL := 32.0
	var half := (GRID - 1) * CELL * 0.5
	var positions := PackedVector3Array()
	var indices := PackedInt32Array()
	positions.resize(GRID * GRID)

	for z in GRID:
		for x in GRID:
			var idx := z * GRID + x
			var px := x * CELL - half
			var pz := z * CELL - half
			positions[idx] = Vector3(px, 0.0, pz)

	for z in GRID - 1:
		for x in GRID - 1:
			var i0 := z * GRID + x
			var i1 := i0 + 1
			var i2 := i0 + GRID
			var i3 := i2 + 1
			indices.push_back(i0)
			indices.push_back(i2)
			indices.push_back(i1)
			indices.push_back(i1)
			indices.push_back(i2)
			indices.push_back(i3)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = positions
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, water_material)
	mesh_instance = MeshInstance3D.new()
	mesh_instance.mesh = mesh
	mesh_instance.position = Vector3(0.0, water_height, 0.0)
	add_child(mesh_instance)
	built = true
	_push_water_split_height()

	# The witnessed per-frame noise texture pair (created once, updated per
	# frame) [orig: Water_GenerateNoiseTextures @ 0x5c0360].
	var size := _noise_core.get_texture_size()
	_noise_core.update(0)
	_noise_color_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _noise_core.get_color_rgba8())
	_noise_normal_img = Image.create_from_data(size, size, false, Image.FORMAT_RGBA8, _noise_core.get_normal_rgba8())
	_noise_color_tex = ImageTexture.create_from_image(_noise_color_img)
	_noise_normal_tex = ImageTexture.create_from_image(_noise_normal_img)
	water_material.set_shader_parameter("u_noise_color", _noise_color_tex)
	water_material.set_shader_parameter("u_noise_normal", _noise_normal_tex)


func _process(_delta: float) -> void:
	if not built or water_material == null:
		return
	water_material.set_shader_parameter("u_water_murk", water_alpha)

	if not _cached_cam or not _cached_cam.is_inside_tree():
		_cached_cam = _find_camera()
	var cam_pos := Vector3.ZERO
	if _cached_cam:
		cam_pos = _cached_cam.global_position
	if _cached_cam and mesh_instance:
		mesh_instance.global_position = Vector3(floorf(cam_pos.x / 32.0) * 32.0, water_height, floorf(cam_pos.z / 32.0) * 32.0)

	# Regenerate the animated noise pair, one tick per rendered frame like the
	# weather core [orig: render_water_surface @ 0x5c3326 regenerates per frame].
	_frame_counter += 1
	_noise_core.update(_frame_counter)
	_noise_color_img.set_data(_noise_core.get_texture_size(), _noise_core.get_texture_size(), false, Image.FORMAT_RGBA8, _noise_core.get_color_rgba8())
	_noise_normal_img.set_data(_noise_core.get_texture_size(), _noise_core.get_texture_size(), false, Image.FORMAT_RGBA8, _noise_core.get_normal_rgba8())
	_noise_color_tex.update(_noise_color_img)
	_noise_normal_tex.update(_noise_normal_img)

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
		var lit := EnvFile.lit_water_color(Color(water.x, water.y, water.z), combined)
		water_material.set_shader_parameter("u_water_color", Vector3(lit.r, lit.g, lit.b))
		var fog_end: float = env.get_fog_level()
		# The witnessed UV transform (scale/bias from the fog-distance INT
		# part, offsets from the layer-1 cloud accumulators + 32x camera)
		# [orig: render_water_surface @ 0x5c3348..0x5c33db]. The weather node
		# owns the shared accumulators; standalone hosts tick a private core.
		if not _cached_weather or not _cached_weather.is_inside_tree():
			_cached_weather = get_node_or_null(weather_path) if not weather_path.is_empty() else null
		var uv_state: Vector4
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
		var env_data: EnvFile = env.get_environment_data()
		if env_data:
			water_material.set_shader_parameter("u_water_murk", env_data.get_water_murk())


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
