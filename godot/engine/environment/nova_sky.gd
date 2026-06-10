@tool
class_name NovaSky
extends Node3D

# Sky dome adapter for the environment subsystem.
# Engine equivalents: sky-dome mesh/render path of [orig: build_sky_dome_mesh @ 0x578db0]
# and [orig: render_skybox @ 0x579080], fed by [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]
# interpolated TOD colors; see docs/env/env-tod-re.md.

@export var environment_path: NodePath

var mesh_instance: MeshInstance3D
var sky_material: ShaderMaterial
var sky_scroll1: float = 0.0
var sky_scroll2_x: float = 0.0
var sky_scroll2_y: float = 0.0
var built: bool = false
var clouds_set: bool = false
var _cached_env: Node = null
var _cached_cam: Camera3D = null


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	build()


func build() -> void:
	if mesh_instance:
		mesh_instance.queue_free()
		mesh_instance = null
	built = false
	clouds_set = false

	var sky_shader := load("res://shaders/sky.gdshader") as Shader
	sky_material = ShaderMaterial.new()
	sky_material.shader = sky_shader

	const ROWS := 21
	const COLS := 21
	var sqrt_base := sqrt(8388608.0)
	var positions := PackedVector3Array()
	var uv1_arr := PackedVector2Array()
	var uv2_arr := PackedVector2Array()
	positions.resize(ROWS * COLS)
	uv1_arr.resize(ROWS * COLS)
	uv2_arr.resize(ROWS * COLS)

	# Port of the 21x21 sky dome generation described for the original sky path.
	for row in ROWS:
		var radius := row * 51.2
		var y := sqrt(9437184.0 - radius * radius) - sqrt_base
		for col in COLS:
			var theta := col * 0.31415927
			var x := sin(theta) * radius
			var z := cos(theta) * radius
			var idx := row * COLS + col
			positions[idx] = Vector3(x, y, z)
			uv1_arr[idx] = Vector2(x * 0.003125, z * 0.003125)
			uv2_arr[idx] = Vector2(x * 0.0014648438, z * 0.0014648438)

	var indices := PackedInt32Array()
	for row in ROWS - 1:
		var base := row * COLS
		for col in COLS - 1:
			indices.push_back(base + col)
			indices.push_back(base + col + COLS + 1)
			indices.push_back(base + col + COLS)
			indices.push_back(base + col)
			indices.push_back(base + col + 1)
			indices.push_back(base + col + COLS + 1)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = positions
	arrays[Mesh.ARRAY_TEX_UV] = uv1_arr
	arrays[Mesh.ARRAY_TEX_UV2] = uv2_arr
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, sky_material)

	mesh_instance = MeshInstance3D.new()
	mesh_instance.mesh = mesh
	add_child(mesh_instance)
	built = true


func _process(delta: float) -> void:
	if not built or sky_material == null:
		return
	if not _cached_cam or not _cached_cam.is_inside_tree():
		_cached_cam = _find_camera()
	if _cached_cam and mesh_instance:
		var cam_pos := _cached_cam.global_position
		mesh_instance.global_position = Vector3(cam_pos.x, 0.0, cam_pos.z)

	var sky_speed := 15.0
	var sky_height := 175.0
	var env := _cached_env
	if env and env.has_method("is_loaded") and env.is_loaded():
		var env_data: EnvFile = env.get_environment_data()
		if env_data and env_data.get_advanced_clouds() == 0:
			var fog_color: Vector3 = env.get_fog_color()
			sky_material.set_shader_parameter("u_sky_base", fog_color)
			sky_material.set_shader_parameter("u_sky_bright", fog_color)
			sky_material.set_shader_parameter("u_sky_highlight", fog_color)
			sky_material.set_shader_parameter("u_horizon_color", fog_color)
			sky_material.set_shader_parameter("u_ground_fog_color", fog_color)
			sky_material.set_shader_parameter("u_secondary_ambient", fog_color)
			sky_material.set_shader_parameter("u_cloud_tint", fog_color)
			sky_material.set_shader_parameter("u_sun_color", Vector3.ZERO)
			sky_material.set_shader_parameter("u_moon_color", Vector3.ZERO)
			sky_material.set_shader_parameter("u_has_clouds", false)
		else:
			sky_material.set_shader_parameter("u_sky_base", env.get_sky_base())
			sky_material.set_shader_parameter("u_sky_bright", env.get_sky_bright())
			sky_material.set_shader_parameter("u_sky_highlight", env.get_sky_highlight())
			sky_material.set_shader_parameter("u_horizon_color", env.get_horizon_color())
			sky_material.set_shader_parameter("u_ground_fog_color", env.get_ground_fog_color())
			sky_material.set_shader_parameter("u_secondary_ambient", env.get_secondary_ambient())
			sky_material.set_shader_parameter("u_cloud_tint", env.get_cloud_tint())
			sky_material.set_shader_parameter("u_sun_color", env.get_sun_color())
			sky_material.set_shader_parameter("u_moon_color", env.get_moon_color())

		sky_material.set_shader_parameter("u_sun_dir", env.get_sun_direction())
		sky_material.set_shader_parameter("u_moon_dir", env.get_moon_direction())
		sky_material.set_shader_parameter("u_clear_color", env.get_skyfog_color())
		sky_speed = env.get_sky_speed()
		sky_height = env.get_sky_height()
		sky_material.set_shader_parameter("u_sky_height", sky_height)

		if not clouds_set and env_data and env_data.get_advanced_clouds() != 0:
			var tex1: Texture2D = env.get_sky_map1_tex()
			var tex2: Texture2D = env.get_sky_map2_tex()
			if tex1 or tex2:
				sky_material.set_shader_parameter("u_cloud_tex1", tex1 if tex1 else tex2)
				sky_material.set_shader_parameter("u_cloud_tex2", tex2 if tex2 else tex1)
				sky_material.set_shader_parameter("u_has_clouds", true)
				clouds_set = true

	var factor := sky_speed * 0.000229
	sky_scroll1 += delta * factor
	sky_scroll2_x += delta * factor * (2.0 / 3.0)
	sky_scroll2_y += delta * factor * (4.0 / 3.0)
	sky_material.set_shader_parameter("u_scroll_offset1", Vector2(sky_scroll1, sky_scroll1))
	sky_material.set_shader_parameter("u_scroll_offset2", Vector2(sky_scroll2_x, sky_scroll2_y))


func _find_camera() -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := get_viewport()
	return viewport.get_camera_3d() if viewport else null
