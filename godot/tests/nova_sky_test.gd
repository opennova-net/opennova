extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: render_skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what NovaSky pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"
const SKY_SHADER := "res://shaders/sky.gdshader"
const TICK := 1.0 / 62.0


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
	assert_eq(mat.get_shader_parameter("u_sky_base"), ctx.env_node.get_sky_base() * 2.0, "c11 skybase")
	assert_eq(mat.get_shader_parameter("u_cloud_base"), ctx.env_node.get_cloud_base() * 2.0, "c24 cloudbase")
	assert_eq(mat.get_shader_parameter("u_cloud_highlight"), ctx.env_node.get_cloud_highlight() * 2.0, "c27 cloudhighlight")
	assert_eq(mat.get_shader_parameter("u_cloud_edge"), ctx.env_node.get_cloud_edge() * 2.0, "c26 cloudedge")
	assert_eq(mat.get_shader_parameter("u_sun_dir"), ctx.env_node.get_sun_direction(),
		"pass 1 is always sun-driven [orig: render_skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"), ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: render_skybox @ 0x579291]")
	assert_eq(mat.get_shader_parameter("u_fog_color"), ctx.env_node.get_skyfog_color(),
		"the dome fogs with the dedicated skyfog block [orig: sky fog wrapper @ 0x579cb0]")
	assert_eq(float(mat.get_shader_parameter("u_fog_end")), ctx.env_node.get_fog_level(), "dome fog end distance")


func test_keyframed_colors_use_retail_upload_scale_without_redoubling_fog() -> void:
	var ctx := _make()
	simulate(ctx.sky, 1, 0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	var actual := [
		_shader_color_units(mat, "u_sky_base"),
		_shader_color_units(mat, "u_sky_bright"),
		_shader_color_units(mat, "u_sky_highlight"),
		_shader_color_units(mat, "u_cloud_base"),
		_shader_color_units(mat, "u_cloud_highlight"),
		_shader_color_units(mat, "u_cloud_edge"),
		_shader_color_units(mat, "u_fog_color"),
	]
	assert_eq(actual, [
		[114, 154, 276],
		[38, 38, 62],
		[292, 324, 294],
		[274, 274, 274],
		[38, 46, 18],
		[308, 316, 330],
		[154, 182, 255],
	], "six sky/cloud constants use retail 2/255; active packed skyfog stays byte/255")


func test_weather_core_ticks_every_world_driven_sky_block_and_doubles_fog_afterward() -> void:
	var core := NovaWeatherCore.new()
	var black := Color8(0, 0, 0)
	core.snap_colors(black, black, black, black)
	core.snap_sky_colors(black, black, black, black, black, black, black, black, black, black)
	core.set_sky_color_targets(
		Color8(8, 16, 24),
		Color8(16, 32, 48),
		Color8(24, 40, 56),
		Color8(32, 64, 96),
		Color8(40, 72, 104),
		Color8(48, 80, 112),
		Color8(56, 88, 120),
		Color8(64, 96, 128),
		Color8(72, 104, 136),
		Color8(80, 112, 144))
	core.tick(black, black, Color8(200, 104, 48), black, Color.WHITE, 0.0)

	assert_eq(_color_units(core.get_fog()), [50, 26, 12],
		"fog smooths in authored bytes before the saturating x2 render tail")
	assert_eq(_color_units(core.get_skyfog()), [2, 4, 6],
		"skyfog smooths, horizon-blends, then doubles (no blend at 1024)")
	assert_eq(_color_units(core.get_ceiling()), [2, 4, 6])
	assert_eq(_color_units(core.get_cloud()), [3, 5, 7])
	assert_eq(_color_units(core.get_floor()), [4, 8, 12])
	assert_eq(_color_units(core.get_skybase()), [5, 9, 13])
	assert_eq(_color_units(core.get_skybright()), [6, 10, 14])
	assert_eq(_color_units(core.get_skyhighlight()), [7, 11, 15])
	assert_eq(_color_units(core.get_cloudbase()), [8, 12, 16])
	assert_eq(_color_units(core.get_cloudhighlight()), [9, 13, 17])
	assert_eq(_color_units(core.get_cloudedge()), [10, 14, 18])


func test_weather_writes_all_dome_colors_back_to_environment() -> void:
	var ctx := _make()
	var weather := NovaWeather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	simulate(weather, 1, TICK)

	assert_eq(ctx.env_node.get_skyfog_color(), weather.get_smooth_skyfog())
	assert_eq(ctx.env_node.get_ceiling_color(), weather.get_smooth_ceiling())
	assert_eq(ctx.env_node.get_cloud_tint(), weather.get_smooth_cloud())
	assert_eq(ctx.env_node.get_floor_color(), weather.get_smooth_floor())
	assert_eq(ctx.env_node.get_sky_base(), weather.get_smooth_sky_base())
	assert_eq(ctx.env_node.get_sky_bright(), weather.get_smooth_sky_bright())
	assert_eq(ctx.env_node.get_sky_highlight(), weather.get_smooth_sky_highlight())
	assert_eq(ctx.env_node.get_cloud_base(), weather.get_smooth_cloud_base())
	assert_eq(ctx.env_node.get_cloud_highlight(), weather.get_smooth_cloud_highlight())
	assert_eq(ctx.env_node.get_cloud_edge(), weather.get_smooth_cloud_edge())


func test_dome_fog_uses_skyfog_instead_of_world_fog() -> void:
	var ctx := _make()
	for keyframe in ctx.env.get_tod_keyframes():
		keyframe.set_fog_color(Color8(10, 20, 30))
		keyframe.set_skyfog_color(Color8(40, 50, 60))
	ctx.env.set_fog_level(1024.0)
	simulate(ctx.sky, 1, TICK)

	var dome_fog: Vector3 = ctx.sky.sky_material.get_shader_parameter("u_fog_color")
	assert_eq(dome_fog, ctx.env_node.get_skyfog_color())
	assert_ne(dome_fog, ctx.env_node.get_fog_color(),
		"the sky wrapper swaps to skyfog while the world keeps ordinary fog")


func _shader_color_units(material: ShaderMaterial, parameter: StringName) -> Array[int]:
	var value: Vector3 = material.get_shader_parameter(parameter)
	return [roundi(value.x * 255.0), roundi(value.y * 255.0), roundi(value.z * 255.0)]


func _color_units(value: Color) -> Array[int]:
	return [roundi(value.r * 255.0), roundi(value.g * 255.0), roundi(value.b * 255.0)]


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

func test_cloud_textures_rebind_and_clear_after_environment_edits() -> void:
	var ctx := _make()
	simulate(ctx.sky, 1, 0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	var before: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_not_null(before, "fixture starts with a bound cloud layer")

	ctx.env.set_sky_map1(ctx.env.get_sky_map2())
	simulate(ctx.sky, 1, 0.016)
	var after: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_ne(after, before,
			"editing a cloud map refreshes the live sky binding")

	ctx.env.set_sky_map1("no_such_cloud_a.pcx")
	ctx.env.set_sky_map2("no_such_cloud_b.pcx")
	simulate(ctx.sky, 1, 0.016)
	assert_eq(mat.get_shader_parameter("u_has_clouds"), false,
			"removing both maps disables stale cloud sampling")


func test_dome_shader_anchors_to_each_render_pass_camera() -> void:
	var shader := load(SKY_SHADER) as Shader
	var code := shader.code
	assert_true(code.contains("vec3 world_pos = vec3(eye.x, eye.y * 0.5, eye.z) + scaled;"),
			"the dome anchor comes from the active render pass camera")
	assert_true(code.contains("POSITION = clip;"),
			"the pass-relative world point overrides the final clip position")
	assert_false(code.contains("MODEL_MATRIX * vec4(scaled"),
			"the reflection pass must not reuse the main-camera model anchor")


func test_proximity_uses_d3d_depth_without_changing_godot_position() -> void:
	var shader := load(SKY_SHADER) as Shader
	var code := shader.code
	assert_true(code.contains("return vec3(clip.xy, clip.w - clip.z);"),
			"proximity converts Godot reverse-Z to the original D3D depth convention")
	assert_true(code.contains("POSITION = clip;"),
			"the render position stays in Godot's native clip convention")


func test_dome_cannot_be_culled_before_reflection_pass_reanchor() -> void:
	var ctx := _make()
	assert_true(ctx.sky.mesh_instance.extra_cull_margin >= 1.0e5,
			"the CPU AABB stays conservative while the shader moves the dome per pass")
	assert_true(ctx.sky.mesh_instance.ignore_occlusion_culling,
			"reflection-pass sky must reach the vertex shader even when the main view occludes it")


func test_cloud_tint_uniform_is_gone_from_the_shader() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_false(shader.code.contains("u_cloud_tint"),
		"the fabricated keyframed-path cloud tint is deleted (divergence #20 fix)")
	assert_false(shader.code.contains("u_moon_dir"),
		"sun/moon glow terms are deleted - celestial bodies are NovaCelestial's job")


func test_cloud_layers_keep_the_recovered_anisotropic_stage_filter() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_eq(shader.code.count("filter_linear_mipmap_anisotropic"), 2,
		"both active cloud stages use the reference device's anisotropic minification")


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
