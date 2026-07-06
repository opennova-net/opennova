extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: render_skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what NovaSky pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"
const SKY_SHADER := "res://shaders/sky.gdshader"


func _make() -> Dictionary:
	var env_node: Node = NovaEnvironment.new()
	add_child_autofree(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var sky: Node3D = NovaSky.new()
	add_child_autofree(sky)
	sky._cached_env = env_node
	return {"sky": sky, "env_node": env_node, "env": env}


func test_keyframed_path_pushes_spec_uniforms() -> void:
	var ctx := _make()
	ctx.sky._process(0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	assert_eq(mat.get_shader_parameter("u_flat_pass"), false, "advanced clouds take the keyframed path")
	assert_eq(mat.get_shader_parameter("u_sky_base"), ctx.env_node.get_sky_base(), "c11 skybase")
	assert_eq(mat.get_shader_parameter("u_cloud_base"), ctx.env_node.get_cloud_base(), "c24 cloudbase")
	assert_eq(mat.get_shader_parameter("u_cloud_highlight"), ctx.env_node.get_cloud_highlight(), "c27 cloudhighlight")
	assert_eq(mat.get_shader_parameter("u_cloud_edge"), ctx.env_node.get_cloud_edge(), "c26 cloudedge")
	assert_eq(mat.get_shader_parameter("u_sun_dir"), ctx.env_node.get_sun_direction(),
		"pass 1 is always sun-driven [orig: render_skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"), ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: render_skybox @ 0x579291]")
	assert_eq(mat.get_shader_parameter("u_fog_color"), ctx.env_node.get_fog_color(),
		"the dome fogs with the shared scene fog color [orig: CD3DDevice_SetActiveFogColor @ 0x677040]")
	assert_eq(float(mat.get_shader_parameter("u_fog_end")), ctx.env_node.get_fog_level(), "dome fog end distance")


func test_flat_pass_does_not_stuff_keyframed_uniforms() -> void:
	var ctx := _make()
	ctx.sky._process(0.016)
	var keyframed_base: Vector3 = ctx.sky.sky_material.get_shader_parameter("u_sky_base")
	ctx.env.set_advanced_clouds(0)
	ctx.sky._process(0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	assert_eq(mat.get_shader_parameter("u_flat_pass"), true, "advanced_clouds 0 takes the flat pass")
	assert_eq(mat.get_shader_parameter("u_flat_color"), ctx.env_node.get_cloud_tint(),
		"the flat dome color is cloud_rgb [orig: render_skybox @ 0x579b42]")
	assert_eq(mat.get_shader_parameter("u_sky_base"), keyframed_base,
		"the flat pass no longer overwrites the keyframed uniforms")


func test_cloud_tint_uniform_is_gone_from_the_shader() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_false(shader.code.contains("u_cloud_tint"),
		"the fabricated keyframed-path cloud tint is deleted (divergence #20 fix)")
	assert_false(shader.code.contains("u_moon_dir"),
		"sun/moon glow terms are deleted - celestial bodies are NovaCelestial's job")


func test_dome_rides_at_half_camera_height() -> void:
	var ctx := _make()
	var cam := Camera3D.new()
	add_child_autofree(cam)
	cam.global_position = Vector3(10.0, 8.0, 6.0)
	ctx.sky._cached_cam = cam
	ctx.sky._process(0.016)
	assert_eq(ctx.sky.mesh_instance.global_position, Vector3(10.0, 4.0, 6.0),
		"dome anchor = camera xz at HALF the camera height [orig: render_skybox @ 0x5790d0]")


func test_dome_mesh_comes_from_the_libs_builder() -> void:
	var ctx := _make()
	var mesh: ArrayMesh = ctx.sky.mesh_instance.mesh
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	assert_eq(positions.size(), 441, "441 dome vertices [orig: build_sky_dome_mesh @ 0x578db0]")
	assert_eq(indices.size(), 2400, "800 triangles")
	assert_eq(normals.size(), 441, "the witnessed FVF 0x212 normals ride along")
	# The witnessed winding head (i, i+22, i+21), (i, i+1, i+22).
	assert_eq(Array(indices.slice(0, 6)), [0, 22, 21, 0, 1, 22], "witnessed quad winding")
	assert_almost_eq(positions[0].y, EnvFile.dome_reference_height(), 0.001,
		"built at the reference height - the Y scale lives in the vertex shader (env #20)")


func test_scroll_offsets_come_from_the_weather_core() -> void:
	var ctx := _make()
	var weather: Node3D = NovaWeather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	ctx.sky.weather_path = weather.get_path()
	simulate(weather, 8, 0.016)
	simulate(ctx.sky, 1, 0.016)
	var off1: Vector2 = ctx.sky.sky_material.get_shader_parameter("u_scroll_offset1")
	var off2: Vector2 = ctx.sky.sky_material.get_shader_parameter("u_scroll_offset2")
	assert_eq(off1, weather.get_cloud_uv_offset1(0.0, 0.0),
		"sky reads layer 1 from the weather core [orig: @ 0x57f1a5]")
	assert_eq(off2, weather.get_cloud_uv_offset2(0.0, 0.0),
		"sky reads layer 2 from the weather core")
	# The accumulator rides U NEGATIVELY, V positively (no camera here, so the
	# offsets are the pure accumulator terms) [orig: render_skybox @ 0x5791de].
	assert_lt(off1.x, 0.0, "layer-1 U accumulator term is negative (env #26)")
	assert_gt(off1.y, 0.0, "layer-1 V accumulator term is positive")


func test_scroll_falls_back_to_a_private_core_without_weather() -> void:
	var ctx := _make()
	simulate(ctx.sky, 1, 0.016)
	var first: Vector2 = ctx.sky.sky_material.get_shader_parameter("u_scroll_offset1")
	simulate(ctx.sky, 1, 0.016)
	var second: Vector2 = ctx.sky.sky_material.get_shader_parameter("u_scroll_offset2")
	var off1: Vector2 = ctx.sky.sky_material.get_shader_parameter("u_scroll_offset1")
	assert_ne(off1, first, "the fallback core keeps ticking the accumulators")
	assert_lt(off1.x, 0.0, "fallback layer-1 U is negative too")
	assert_ne(second, Vector2.ZERO, "layer 2 advances as well")
