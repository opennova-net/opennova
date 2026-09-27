extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: Render_Skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what SkyDome pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"
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
		"pass 1 is always sun-driven [orig: Render_Skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"),
		ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: Render_Skybox @ 0x579291]")
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
	# Render_Skybox loads c9.x from the raw smoothed g_EnvFogDistCurrent with no
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
		"the flat dome color is cloud_rgb [orig: Render_Skybox @ 0x579b42]")
	assert_eq(mat.get_shader_parameter("u_sky_base"), keyframed_base,
		"the flat pass no longer overwrites the keyframed uniforms")

func test_cloud_textures_rebind_and_clear_after_environment_edits() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var mat: ShaderMaterial = ctx.sky.get_cloud_material()
	var before: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_not_null(before, "fixture starts with a bound cloud layer")

	ctx.env.set_sky_map1(ctx.env.get_sky_map2())
	ctx.sky.advance_frame(0.016)
	var after: Texture2D = mat.get_shader_parameter("u_cloud_tex1")
	assert_ne(after, before,
			"editing a cloud map refreshes the live sky binding")

	assert_eq(ctx.sky.get_sky_material().next_pass, mat,
			"bound cloud layers draw the cloud pass")
	ctx.env.set_sky_map1("no_such_cloud_a.pcx")
	ctx.env.set_sky_map2("no_such_cloud_b.pcx")
	ctx.sky.advance_frame(0.016)
	assert_null(ctx.sky.get_sky_material().next_pass,
			"removing both maps drops the cloud pass (no stale cloud sampling)")


func test_dome_cannot_be_culled_before_reflection_pass_reanchor() -> void:
	var ctx := _make()
	assert_true(ctx.sky.get_mesh_instance().extra_cull_margin >= 1.0e5,
			"the CPU AABB stays conservative while the shader moves the dome per pass")
	assert_true(ctx.sky.get_mesh_instance().ignore_occlusion_culling,
			"reflection-pass sky must reach the vertex shader even when the main view occludes it")


func test_dome_rides_at_half_camera_height() -> void:
	var ctx := _make()
	var cam := Camera3D.new()
	add_child_autofree(cam)
	cam.global_position = Vector3(10.0, 8.0, 6.0)
	cam.make_current()
	ctx.sky.advance_frame(0.016)
	assert_eq(ctx.sky.get_mesh_instance().global_position, Vector3(10.0, 4.0, 6.0),
		"dome anchor = camera xz at HALF the camera height [orig: Render_Skybox @ 0x5790d0]")


func test_dome_mesh_comes_from_the_libs_builder() -> void:
	var ctx := _make()
	var mesh: ArrayMesh = ctx.sky.get_mesh_instance().mesh
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	assert_eq(positions.size(), 441, "441 dome vertices [orig: SkyDome_BuildMesh @ 0x578db0]")
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
	var off1: Vector2 = ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset1")
	var off2: Vector2 = ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset2")
	assert_eq(off1, weather.get_cloud_uv_offset1(0.0, 0.0),
		"sky reads layer 1 from the weather core [orig: @ 0x57f1a5]")
	assert_eq(off2, weather.get_cloud_uv_offset2(0.0, 0.0),
		"sky reads layer 2 from the weather core")
	# The accumulator rides U NEGATIVELY, V positively (no camera here, so the
	# offsets are the pure accumulator terms) [orig: Render_Skybox @ 0x5791de].
	assert_lt(off1.x, 0.0, "layer-1 U accumulator term is negative (env #26)")
	assert_gt(off1.y, 0.0, "layer-1 V accumulator term is positive")


func test_scroll_falls_back_to_a_private_core_without_weather() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var first: Vector2 = ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset1")
	ctx.sky.advance_frame(0.016)
	var second: Vector2 = ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset2")
	var off1: Vector2 = ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset1")
	assert_ne(off1, first, "the fallback core keeps ticking the accumulators")
	assert_lt(off1.x, 0.0, "fallback layer-1 U is negative too")
	assert_ne(second, Vector2.ZERO, "layer 2 advances as well")


func test_cloud_texture_axes_follow_the_render_basis() -> void:
	# The builder's UVs are the render x/z scaled (layer 1 by 1/320, layer 2 by
	# 3/2048) and retail draws the dome in that basis, so texture u runs along
	# render x = Godot z and v along render z = Godot x. Drawn identity, u ran
	# along Godot x: a mirrored cloud field drifting the other way (OT-E6,
	# 2026-09-27).
	var ctx := _make()
	var mesh: ArrayMesh = ctx.sky.get_mesh_instance().mesh
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var uv1: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
	var uv2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
	var worst1 := 0.0
	var worst2 := 0.0
	for i in positions.size():
		var along := Vector2(positions[i].z, positions[i].x)
		worst1 = maxf(worst1, (uv1[i] - along * 0.003125).length())
		worst2 = maxf(worst2, (uv2[i] - along * (3.0 / 2048.0)).length())
	assert_lt(worst1, 1.0e-4, "layer-1 u follows Godot z (render x), v Godot x")
	assert_lt(worst2, 1.0e-4, "layer-2 u follows Godot z (render x), v Godot x")
	assert_gt(Vector2(positions[25].x, positions[25].z).length(), 50.0,
			"the probe vertices sit off the dome axis")


func test_cloud_scroll_camera_term_rides_the_render_axes() -> void:
	# The scroll's camera term is +camera/4096 on layer 1 along the RENDER axes
	# (the same basis as the dome UVs): an eye moved along Godot z (render x)
	# scrolls U, one moved along Godot x (render z) scrolls V.
	var ctx := _make()
	var weather: Node3D = Weather.new()
	weather.environment_path = ctx.env_node.get_path()
	add_child_autofree(weather)
	ctx.sky.weather_path = weather.get_path()
	weather.advance_frame(TICK)
	var cam := Camera3D.new()
	add_child_autofree(cam)
	cam.make_current()
	var offsets: Array[Vector2] = []
	for eye in [Vector3(0.0, 8.0, 0.0), Vector3(0.0, 8.0, 409.6), Vector3(409.6, 8.0, 0.0)]:
		cam.global_position = eye
		ctx.sky.advance_frame(0.0)
		offsets.append(ctx.sky.get_cloud_material().get_shader_parameter("u_scroll_offset1"))
	assert_almost_eq(offsets[1].x - offsets[0].x, 0.1, 1.0e-5, "Godot z (render x) scrolls U")
	assert_almost_eq(offsets[1].y - offsets[0].y, 0.0, 1.0e-5, "Godot z leaves V")
	assert_almost_eq(offsets[2].x - offsets[0].x, 0.0, 1.0e-5, "Godot x leaves U")
	assert_almost_eq(offsets[2].y - offsets[0].y, 0.1, 1.0e-5, "Godot x (render z) scrolls V")


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
	viewport.add_child(sky)
	sky.advance_frame(0.0)
	# GameWorld.update_frame_clear_color is the one frame-clear writer: above
	# water outside thermal it writes the env's frame clear, pre-encoded to sRGB
	# (game_world_test pins that writer). Stage the same device write here.
	var skyfog: Vector3 = env_node.get_frame_clear_color()
	clear.environment.background_color = Color(skyfog.x, skyfog.y, skyfog.z).linear_to_srgb()
	# Fully fogged dome pixels and the open area beneath it must be the same
	# color. Comparing rendered pixels catches Godot's background sRGB decode;
	# comparing uniforms alone cannot detect that device conversion.
	# Both dome passes fog (the cloud pass draws over the gradient).
	sky.get_sky_material().set_shader_parameter("u_fog_end", 1.0)
	sky.get_cloud_material().set_shader_parameter("u_fog_end", 1.0)
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


func test_world_beyond_the_dome_surface_draws_over_the_sky() -> void:
	# Retail draws the dome with z-write off and ZFUNC ALWAYS before any world
	# geometry (pass flags 0x300000 [orig: Render_Skybox @ 0x579883]), so the
	# world overdraws it wherever it is. From 300 u up the dome (anchored at
	# half the eye height) meets a horizontal view ray ~400 u out; a surface
	# 600 u out must still draw over it.
	if DisplayServer.get_name() == "headless":
		pending("dome/world depth ordering needs a windowed renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var env_node := MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	viewport.add_child(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var camera := Camera3D.new()
	camera.far = 2000.0
	viewport.add_child(camera)
	camera.look_at_from_position(Vector3(0.0, 300.0, 0.0), Vector3(0.0, 300.0, -1.0), Vector3.UP)
	camera.make_current()
	var sky := SkyDome.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	viewport.add_child(sky)
	var wall := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(200.0, 200.0, 10.0)
	wall.mesh = box
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color(1.0, 0.0, 0.0)
	wall.material_override = red
	viewport.add_child(wall)
	wall.global_position = Vector3(0.0, 300.0, -600.0)
	sky.advance_frame(0.0)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	assert_gt(pixel.r, 0.9, "the far wall draws over the dome (red %s)" % pixel)
	assert_lt(pixel.g, 0.1, "no dome colour survives over the wall (%s)" % pixel)
	assert_lt(pixel.b, 0.1, "no dome colour survives over the wall (%s)" % pixel)


func test_cloud_pass_draws_after_the_bodies_on_the_sky_cloud_rung() -> void:
	# Retail draws the gradient, then Render_CelestialBodies(0), then the
	# cloud pass [orig: Render_Skybox @ 0x5798e0 / @ 0x5798f1..0x579b15]: the
	# cloud pass is its own transparent draw one rung after the sky bodies.
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	var clouds: ShaderMaterial = ctx.sky.get_cloud_material()
	assert_eq(ctx.sky.get_sky_material().next_pass, clouds,
			"the fixture's cloud layers attach the cloud pass")
	assert_eq(clouds.render_priority, ObjectShaderCache.RENDER_RUNG_SKY_CLOUDS)
	assert_gt(ObjectShaderCache.RENDER_RUNG_SKY_CLOUDS, ObjectShaderCache.RENDER_RUNG_SKY_BODY,
			"the clouds veil the sun/moon discs")
	assert_eq(clouds.get_shader_parameter("u_cloud_base"),
			ctx.sky.get_sky_material().get_shader_parameter("u_cloud_base"),
			"both passes share the dome constants")


func _sky_view(clouds_opaque: bool, body_color: Color = Color(0, 0, 0, 0)) -> Dictionary:
	# A 64 x 64 view up into the dome over the synthetic Full_00 environment.
	# clouds_opaque: bind solid white cloud maps (the cloud pass then covers
	# the gradient) or fully transparent ones. body_color.a > 0 adds a
	# sky-body-rung quad 50 u ahead, drawn like a disc (blended, no depth
	# write).
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var env_node := MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	viewport.add_child(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var camera := Camera3D.new()
	camera.far = 2000.0
	viewport.add_child(camera)
	camera.look_at_from_position(Vector3.ZERO, Vector3(0.0, 1.0, -1.0), Vector3.UP)
	camera.make_current()
	var sky := SkyDome.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	viewport.add_child(sky)
	if body_color.a > 0.0:
		var quad := MeshInstance3D.new()
		var mesh := QuadMesh.new()
		mesh.size = Vector2(200.0, 200.0)
		quad.mesh = mesh
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		material.albedo_color = body_color
		material.cull_mode = BaseMaterial3D.CULL_DISABLED
		material.render_priority = ObjectShaderCache.RENDER_RUNG_SKY_BODY
		quad.material_override = material
		viewport.add_child(quad)
		quad.global_transform = camera.global_transform.translated_local(Vector3(0.0, 0.0, -50.0))
	sky.advance_frame(0.0)
	var cloud_image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	cloud_image.fill(Color(1.0, 1.0, 1.0, 1.0 if clouds_opaque else 0.0))
	var cloud_texture := ImageTexture.create_from_image(cloud_image)
	var clouds: ShaderMaterial = sky.get_cloud_material()
	clouds.set_shader_parameter("u_cloud_tex1", cloud_texture)
	clouds.set_shader_parameter("u_cloud_tex2", cloud_texture)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	return {"pixel": viewport.get_texture().get_image().get_pixel(32, 32),
			"has_clouds": sky.get_sky_material().next_pass == clouds}


func test_the_gradient_pass_opens_the_sky_pass() -> void:
	# The gradient draw precedes the bodies and the clouds inside Render_Skybox
	# [orig: Render_Skybox @ 0x5798dc, @ 0x5798e0, @ 0x5798f1..0x579b15]. The
	# pass writes no depth, so Godot orders it in the transparent list by its
	# rung; on rung 0 it painted over both.
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	assert_eq(ctx.sky.get_sky_material().render_priority,
			ObjectShaderCache.RENDER_RUNG_SKY_DOME)
	assert_lt(ObjectShaderCache.RENDER_RUNG_SKY_DOME, ObjectShaderCache.RENDER_RUNG_SKY_BODY)
	if DisplayServer.get_name() == "headless":
		pending("dome pass ordering needs a windowed renderer")
		return
	var body: Dictionary = await _sky_view(false, Color(1.0, 0.0, 1.0, 1.0))
	var pixel: Color = body.pixel
	assert_gt(pixel.r, 0.9, "a sky-body draw shows over the gradient (%s)" % pixel)
	assert_lt(pixel.g, 0.1, "no gradient paints over the sky body (%s)" % pixel)
	assert_gt(pixel.b, 0.9, "a sky-body draw shows over the gradient (%s)" % pixel)
	var bare: Dictionary = await _sky_view(false)
	var covered: Dictionary = await _sky_view(true)
	assert_true(bool(covered.has_clouds), "the fixture's cloud layers attach the cloud pass")
	var a: Color = bare.pixel
	var b: Color = covered.pixel
	assert_gt(absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b), 0.05,
			"opaque cloud maps change the dome: the cloud pass draws over the gradient (%s vs %s)"
			% [a, b])


func test_flat_pass_drops_the_cloud_pass() -> void:
	# advanced_clouds 0 draws the single flat dome [orig: Render_Skybox
	# @ 0x579b42..0x579c76] and no cloud pass.
	var ctx := _make()
	ctx.env.set_advanced_clouds(0)
	ctx.sky.advance_frame(0.016)
	assert_null(ctx.sky.get_sky_material().next_pass)


func test_pass_gates_reach_both_dome_passes() -> void:
	var ctx := _make()
	ctx.sky.advance_frame(0.016)
	ctx.sky.set_pass_gates(false, true)
	assert_false(ctx.sky.is_beauty_pass_drawn())
	assert_true(ctx.sky.is_mirror_pass_drawn())
	for mat: ShaderMaterial in [ctx.sky.get_sky_material(), ctx.sky.get_cloud_material()]:
		assert_eq(mat.get_shader_parameter("u_beauty_pass_drawn"), false)
		assert_eq(mat.get_shader_parameter("u_mirror_pass_drawn"), true)


func test_closed_beauty_gate_draws_no_dome_in_the_main_view() -> void:
	# The main frame skips its sky bracket (sky letter / eye at or below the
	# water [orig: Render_ProcessMainSceneFrame @ 0x5ca7c4]): the clear shows.
	if DisplayServer.get_name() == "headless":
		pending("per-pass dome gating needs a windowed renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	clear.environment.background_color = Color(0.0, 1.0, 0.0)
	viewport.add_child(clear)
	var env_node := MissionEnvironment.new()
	env_node.name = "SkyTestEnv"
	viewport.add_child(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var camera := Camera3D.new()
	camera.far = 2000.0
	viewport.add_child(camera)
	camera.look_at_from_position(Vector3.ZERO, Vector3.UP, Vector3.FORWARD)
	camera.make_current()
	var sky := SkyDome.new()
	sky.environment_path = NodePath("../SkyTestEnv")
	viewport.add_child(sky)
	sky.advance_frame(0.0)
	sky.set_pass_gates(false, true)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	assert_almost_eq(pixel.g, 1.0, 0.02, "the green clear shows (%s)" % pixel)
	assert_lt(pixel.r, 0.05, "no dome pixel in the gated beauty view (%s)" % pixel)
