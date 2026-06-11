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
		# The dome follows the camera in xz and rides at HALF the camera height
		# [orig: render_skybox @ 0x5790d0 - world translation z = camHeight >> 1].
		var cam_pos := _cached_cam.global_position
		mesh_instance.global_position = Vector3(cam_pos.x, cam_pos.y * 0.5, cam_pos.z)

	var sky_speed := 15.0
	var sky_height := 175.0
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
			sky_material.set_shader_parameter("u_sky_base", env.get_sky_base())
			sky_material.set_shader_parameter("u_sky_bright", env.get_sky_bright())
			sky_material.set_shader_parameter("u_sky_highlight", env.get_sky_highlight())
			sky_material.set_shader_parameter("u_cloud_base", env.get_cloud_base())
			sky_material.set_shader_parameter("u_cloud_highlight", env.get_cloud_highlight())
			sky_material.set_shader_parameter("u_cloud_edge", env.get_cloud_edge())

		# Pass 1 is always sun-driven; the cloud pass follows the active light,
		# moon at night [orig: render_skybox @ 0x579287 Terrain_GetSunDirectionAsFloat
		# vs @ 0x579291 Environment_GetLightDirectionFloat].
		sky_material.set_shader_parameter("u_sun_dir", env.get_sun_direction())
		sky_material.set_shader_parameter("u_light_dir", env.get_light_direction())
		# The dome fogs with the same scene fog state as terrain - the active fog
		# block color, no dome-specific derivation
		# [orig: CD3DDevice_SetActiveFogColor @ 0x677040].
		sky_material.set_shader_parameter("u_fog_color", env.get_fog_color())
		sky_material.set_shader_parameter("u_fog_end", env.get_fog_level())
		sky_speed = env.get_sky_speed()
		sky_height = env.get_sky_height()
		sky_material.set_shader_parameter("u_sky_height", sky_height)

		if not clouds_set and env_data:
			var tex1: Texture2D = env.get_sky_map1_tex()
			var tex2: Texture2D = env.get_sky_map2_tex()
			if tex1 or tex2:
				sky_material.set_shader_parameter("u_cloud_tex1", tex1 if tex1 else tex2)
				sky_material.set_shader_parameter("u_cloud_tex2", tex2 if tex2 else tex1)
				sky_material.set_shader_parameter("u_has_clouds", true)
				clouds_set = true

	# Cloud scroll [orig: render_skybox @ 0x5791de + Environment_UpdateWeatherTick
	# @ 0x57f1a5]: accumulators advance at rate x {1, 1, 2/3, 4/3} per 62 Hz tick
	# (rate = sky_speed << 10), and UVs are (camera + acc) / 2^28 for layer 1 and
	# / 2^29 for layer 2 (half UV scale, anisotropic 4/3 U / 2/3 V drift).
	var factor := sky_speed * (1024.0 * 62.0 / 268435456.0)
	sky_scroll1 += delta * factor
	sky_scroll2_x += delta * factor * (4.0 / 3.0) * 0.5
	sky_scroll2_y += delta * factor * (2.0 / 3.0) * 0.5
	var cam_anchor := Vector2.ZERO
	if _cached_cam:
		cam_anchor = Vector2(_cached_cam.global_position.x, _cached_cam.global_position.z)
	const UV_PER_UNIT_L1 := 0.000244140625 # 2^-28 on 16.16 world coords
	const UV_PER_UNIT_L2 := 0.0001220703125 # 2^-29
	sky_material.set_shader_parameter("u_scroll_offset1", cam_anchor * UV_PER_UNIT_L1 + Vector2(sky_scroll1, sky_scroll1))
	sky_material.set_shader_parameter("u_scroll_offset2", cam_anchor * UV_PER_UNIT_L2 + Vector2(sky_scroll2_x, sky_scroll2_y))


func _find_camera() -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := get_viewport()
	return viewport.get_camera_3d() if viewport else null
