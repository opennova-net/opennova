extends GutTest

# The methods of tests/sky_dome_test.gd that need a RenderingDevice, split out of it so
# the windowed run selects them (G-100).

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"


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
