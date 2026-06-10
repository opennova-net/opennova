@tool
class_name NovaWater
extends Node3D

# Water plane preview/runtime adapter.
# Engine equivalents: water color/height/murk consume globals parsed by
# [orig: TimeOfDay_ParseProperty @ 0x57c590] and fog colors interpolated by
# [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] (docs/env/env-tod-re.md).

@export var environment_path: NodePath
@export var terrain_data: NovaTerrainData
@export_range(-100, 200, 0.1) var water_height: float = 0.0:
	set(value):
		water_height = value
		if mesh_instance:
			mesh_instance.position.y = water_height
@export_range(0, 1, 0.01) var water_alpha: float = 0.6

# When set (not NaN), the host drives water height directly and the env/terrain
# fallback is ignored — the terrain editor authors height through its document.
var _height_override: float = NAN


func set_height_override(value: float) -> void:
	_height_override = value
	if not is_nan(value):
		water_height = value

var mesh_instance: MeshInstance3D
var water_material: ShaderMaterial
var elapsed_time: float = 0.0
var built: bool = false
var _cached_env: Node = null
var _cached_cam: Camera3D = null
var _terrain_fallback_water_height: float = 0.0


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	if terrain_data and terrain_data.is_loaded():
		var raw := terrain_data.get_water_height()
		if raw > 0:
			_terrain_fallback_water_height = float(raw) * 0.5
			water_height = _terrain_fallback_water_height
	_apply_environment_water_height()
	build()


func build() -> void:
	if mesh_instance:
		mesh_instance.queue_free()
		mesh_instance = null
	built = false
	water_material = ShaderMaterial.new()
	water_material.shader = load("res://shaders/water.gdshader") as Shader

	const GRID := 65
	const CELL := 32.0
	var half := (GRID - 1) * CELL * 0.5
	var positions := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	positions.resize(GRID * GRID)
	uvs.resize(GRID * GRID)

	for z in GRID:
		for x in GRID:
			var idx := z * GRID + x
			var px := x * CELL - half
			var pz := z * CELL - half
			positions[idx] = Vector3(px, 0.0, pz)
			uvs[idx] = Vector2(px / 2048.0, pz / 2048.0)

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
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, water_material)
	mesh_instance = MeshInstance3D.new()
	mesh_instance.mesh = mesh
	mesh_instance.position = Vector3(0.0, water_height, 0.0)
	add_child(mesh_instance)
	built = true
	elapsed_time = 0.0


func _process(delta: float) -> void:
	if not built or water_material == null:
		return
	elapsed_time += delta
	water_material.set_shader_parameter("u_time", elapsed_time)
	water_material.set_shader_parameter("u_water_alpha", water_alpha)

	if not _cached_cam or not _cached_cam.is_inside_tree():
		_cached_cam = _find_camera()
	if _cached_cam and mesh_instance:
		var cam_pos := _cached_cam.global_position
		mesh_instance.global_position = Vector3(floorf(cam_pos.x / 32.0) * 32.0, water_height, floorf(cam_pos.z / 32.0) * 32.0)

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
		water_material.set_shader_parameter("u_scroll_speed", env.get_sky_speed() * (1024.0 * 62.0 / 268435456.0))
		var fog_end: float = env.get_fog_level()
		var fog_start: float = env.get_fog_start() if env.has_method("get_fog_start") else 0.5
		water_material.set_shader_parameter("u_fog_color", env.get_fog_color())
		water_material.set_shader_parameter("u_fog_start", fog_start)
		water_material.set_shader_parameter("u_fog_end", fog_end)
		water_material.set_shader_parameter("u_fog_type", env.get_fog_type())
		var env_data: EnvFile = env.get_environment_data()
		if env_data:
			water_material.set_shader_parameter("u_water_alpha", env_data.get_water_murk())


func _apply_environment_water_height() -> void:
	if not is_nan(_height_override):
		water_height = _height_override
		return
	var env := _cached_env
	if env and env.has_method("has_water_height") and env.has_water_height():
		# .env water_height is stored <<15 by the engine — half world units,
		# same convention as the terrain fallback above
		# [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
		water_height = float(env.get_water_height()) * 0.5
	elif _terrain_fallback_water_height != 0.0:
		water_height = _terrain_fallback_water_height


func _find_camera() -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := get_viewport()
	return viewport.get_camera_3d() if viewport else null
