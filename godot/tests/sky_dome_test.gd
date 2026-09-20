extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: render_skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what SkyDome pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"
const SKY_SHADER := "res://shaders/sky.gdshader"
const TICK := 1.0 / 62.0


func _make() -> Dictionary:
	var env_node: Node = MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	add_child_autofree(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var sky: Node3D = SkyDome.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	add_child_autofree(sky)
	return {"sky": sky, "env_node": env_node, "env": env}


func test_keyframed_path_pushes_spec_uniforms() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	assert_eq(mat.get_shader_parameter("u_flat_pass"), false, "advanced clouds take the keyframed path")
	assert_eq(mat.get_shader_parameter("u_sky_base"), ctx.env_node.get_sky_base() * 2.0, "c11 skybase")
	assert_eq(mat.get_shader_parameter("u_cloud_base"), ctx.env_node.get_cloud_base() * 2.0, "c24 cloudbase")
	assert_eq(mat.get_shader_parameter("u_cloud_highlight"), ctx.env_node.get_cloud_highlight() * 2.0, "c27 cloudhighlight")
	assert_eq(mat.get_shader_parameter("u_cloud_edge"), ctx.env_node.get_cloud_edge() * 2.0, "c26 cloudedge")
	# The dome shader and the getters both serve GODOT-world vectors (the
	# util/axes.h swap applies once at each device seam — 2026-08-20
	# celestial-axis correction).
	assert_eq(mat.get_shader_parameter("u_sun_dir"),
		ctx.env_node.get_sun_direction(),
		"pass 1 is always sun-driven [orig: render_skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"),
		ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: render_skybox @ 0x579291]")
	assert_eq(mat.get_shader_parameter("u_fog_color"), ctx.env_node.get_skyfog_color(),
		"the dome fogs with the dedicated skyfog block [orig: sky fog wrapper @ 0x579cb0]")
	assert_eq(float(mat.get_shader_parameter("u_fog_end")), ctx.env_node.get_fog_level(), "dome fog end distance")


func test_keyframed_colors_use_retail_upload_scale_without_redoubling_fog() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var actual := [
		_shader_color_units(mat, "u_sky_base"),
		_shader_color_units(mat, "u_sky_bright"),
		_shader_color_units(mat, "u_sky_highlight"),
		_shader_color_units(mat, "u_cloud_base"),
		_shader_color_units(mat, "u_cloud_highlight"),
		_shader_color_units(mat, "u_cloud_edge"),
		_shader_color_units(mat, "u_fog_color"),
	]
	# synth_full.env's 1200 keyframe (tests/fixtures/minimal_env_gen.cpp):
	# skybase 60,80,140; skybright 20,20,32; skyhighlight 150,160,150;
	# cloudbase 140,140,140; cloudhighlight 20,24,10; cloudedge 150,160,170;
	# skyfog 80,96,140 (its doubled blue saturates).
	assert_eq(actual, [
		[120, 160, 280],
		[40, 40, 64],
		[300, 320, 300],
		[280, 280, 280],
		[40, 48, 20],
		[300, 320, 340],
		[160, 192, 255],
	], "six sky/cloud constants use retail 2/255; active packed skyfog stays byte/255")


func test_weather_writes_all_dome_colors_back_to_environment() -> void:
	var ctx := _make()
	var weather := Weather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	weather.advance_frame(TICK)

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
	ctx.sky.advance_frame(TICK)

	var dome_fog: Vector3 = ctx.sky.get_sky_material().get_shader_parameter("u_fog_color")
	assert_eq(dome_fog, ctx.env_node.get_skyfog_color())
	assert_ne(dome_fog, ctx.env_node.get_fog_color(),
		"the sky wrapper swaps to skyfog while the world keeps ordinary fog")


func test_underwater_dome_keeps_the_smoothed_fog_end_without_rewriting_sky_wrapper() -> void:
	# render_skybox loads c9.x from the raw smoothed Env_FogDistCurrent with no
	# underwater leg (env-tod-re.md, the VS-constant table), so the dome's fog
	# end is the same unscaled distance on both sides of the water plane while
	# the world passes fog to the murk-derived end.
	var ctx := _make()
	ctx.sky.advance_frame(TICK)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var dry_sky_base: Vector3 = mat.get_shader_parameter("u_sky_base")
	var dry_fog_end := float(mat.get_shader_parameter("u_fog_end"))
	assert_almost_eq(dry_fog_end, ctx.env_node.get_fog_level(), 0.001,
			"the dome fog end is the raw smoothed distance above water")
	ctx.env_node.set_underwater_view(true)
	ctx.sky.advance_frame(TICK)

	assert_eq(Vector3(mat.get_shader_parameter("u_fog_color")),
			ctx.env_node.get_skyfog_color(),
			"the dome keeps the witnessed skyfog wrapper underwater")
	assert_almost_eq(float(mat.get_shader_parameter("u_fog_end")), dry_fog_end, 0.001,
			"the dome keeps the unscaled smoothed fog end underwater")
	assert_true(absf(ctx.env_node.get_scene_fog_end() - dry_fog_end) > 1.0,
			"the world's murk end differs from the dome's, so the pin is live")
	assert_eq(Vector3(mat.get_shader_parameter("u_sky_base")), dry_sky_base,
			"pass fog selection does not rewrite authored sky/TOD colors")


func _shader_color_units(material: ShaderMaterial, parameter: StringName) -> Array[int]:
	var value: Vector3 = material.get_shader_parameter(parameter)
	return [roundi(value.x * 255.0), roundi(value.y * 255.0), roundi(value.z * 255.0)]


func test_flat_pass_does_not_stuff_keyframed_uniforms() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var keyframed_base: Vector3 = ctx.sky.get_sky_material().get_shader_parameter("u_sky_base")
	ctx.env.set_advanced_clouds(0)
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	assert_eq(mat.get_shader_parameter("u_flat_pass"), true, "advanced_clouds 0 takes the flat pass")
	assert_eq(mat.get_shader_parameter("u_flat_color"), ctx.env_node.get_cloud_tint(),
		"the flat dome color is cloud_rgb [orig: render_skybox @ 0x579b42]")
	assert_eq(mat.get_shader_parameter("u_sky_base"), keyframed_base,
		"the flat pass no longer overwrites the keyframed uniforms")

func test_cloud_textures_rebind_and_clear_after_environment_edits() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_sky_material()
	var before: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_not_null(before, "fixture starts with a bound cloud layer")

	ctx.env.set_sky_map1(ctx.env.get_sky_map2())
	ctx.sky.advance_frame(0.016)
	var after: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_ne(after, before,
			"editing a cloud map refreshes the live sky binding")

	ctx.env.set_sky_map1("no_such_cloud_a.pcx")
	ctx.env.set_sky_map2("no_such_cloud_b.pcx")
	ctx.sky.advance_frame(0.016)
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
	assert_true(ctx.sky.get_mesh_instance().extra_cull_margin >= 1.0e5,
			"the CPU AABB stays conservative while the shader moves the dome per pass")
	assert_true(ctx.sky.get_mesh_instance().ignore_occlusion_culling,
			"reflection-pass sky must reach the vertex shader even when the main view occludes it")


func test_cloud_tint_uniform_is_gone_from_the_shader() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_false(shader.code.contains("u_cloud_tint"),
		"the fabricated keyframed-path cloud tint is deleted (divergence #20 fix)")
	assert_false(shader.code.contains("u_moon_dir"),
		"sun/moon glow terms are deleted - celestial bodies are Celestial's job")


func test_cloud_layers_keep_the_recovered_anisotropic_stage_filter() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_eq(shader.code.count("filter_linear_mipmap_anisotropic"), 2,
		"both active cloud stages use the reference device's anisotropic minification")


func test_dome_rides_at_half_camera_height() -> void:
	var ctx := _make()
	var cam := Camera3D.new()
	add_child_autofree(cam)
	cam.global_position = Vector3(10.0, 8.0, 6.0)
	cam.make_current()
	ctx.sky.advance_frame(0.016)
	assert_eq(ctx.sky.get_mesh_instance().global_position, Vector3(10.0, 4.0, 6.0),
		"dome anchor = camera xz at HALF the camera height [orig: render_skybox @ 0x5790d0]")


func test_dome_mesh_comes_from_the_libs_builder() -> void:
	var ctx := _make()
	var mesh: ArrayMesh = ctx.sky.get_mesh_instance().mesh
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
	var weather: Node3D = Weather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	ctx.sky.weather_path = weather.get_path()
	for _i in 8:
		weather.advance_frame(0.016)
	ctx.sky.advance_frame(0.016)
	var off1: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	var off2: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset2")
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
	ctx.sky.advance_frame(0.016)
	var first: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	ctx.sky.advance_frame(0.016)
	var second: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset2")
	var off1: Vector2 = ctx.sky.get_sky_material().get_shader_parameter("u_scroll_offset1")
	assert_ne(off1, first, "the fallback core keeps ticking the accumulators")
	assert_lt(off1.x, 0.0, "fallback layer-1 U is negative too")
	assert_ne(second, Vector2.ZERO, "layer 2 advances as well")


func test_rendered_dome_fog_matches_the_exposed_background() -> void:
	if DisplayServer.get_name() == "headless":
		pending("sky/background color continuity needs a windowed renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	viewport.add_child(clear)
	var env_node := MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	viewport.add_child(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var camera := Camera3D.new()
	camera.far = 501.0
	viewport.add_child(camera)
	camera.make_current()
	var sky := SkyDome.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	sky.frame_clear_environment = clear.environment
	viewport.add_child(sky)
	sky.advance_frame(0.0)
	# Fully fogged dome pixels and the open area beneath it must be the same
	# color. Comparing rendered pixels catches Godot's background sRGB decode;
	# comparing uniforms alone cannot detect that device conversion.
	sky.get_sky_material().set_shader_parameter("u_fog_end", 1.0)
	var samples: Array[Color] = []
	for direction in [Vector3.UP, Vector3.DOWN]:
		camera.look_at_from_position(Vector3.ZERO, direction, Vector3.FORWARD)
		await RenderingServer.frame_post_draw
		await RenderingServer.frame_post_draw
		samples.append(viewport.get_texture().get_image().get_pixel(32, 32))
	assert_gt(samples[0].r, 0.1, "the sky sample is lit, not an empty viewport")
	for channel in 3:
		assert_almost_eq(samples[0][channel], samples[1][channel], 0.01,
				"fogged sky and frame clear agree in channel %d" % channel)
