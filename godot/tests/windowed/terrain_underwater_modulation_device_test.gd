extends GutTest

# The methods of tests/terrain_underwater_modulation_test.gd that need a RenderingDevice,
# split out of it so the windowed run selects them (G-100).


func test_reflected_pass_clips_terrain_below_the_plane_less_0_05() -> void:
	# Retail's reflected pass arms a terrain clip plane of wh - 0.1 (retail
	# Render_MainScene @ 0x5c1561..0x5c1578) and the sector batch's texgen
	# u = y + 0.45 - plane under AlphaRef 0x80 (Terrain_RenderSectorBatch
	# @ 0x6092c6..0x60935b) keeps y >= wh - 0.05. The terrain shader's LOD
	# debug colour marks every kept fragment green over a 2x2 card spanning
	# y -1..1 across 64 rows: rows 32/33 sit at y = -0.016 / -0.047, rows
	# 34/35 at -0.078 / -0.109.
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.global_shader_parameter_set("opennova_water_active", true)
	RenderingServer.global_shader_parameter_set("opennova_water_height", 0.0)
	var reflected: Image = await _render_terrain_card(Water.REFLECTION_CULL_MASK)
	var beauty: Image = await _render_terrain_card(0xFFFFF)
	ShaderGlobals.restore_defaults(["opennova_water_active", "opennova_water_height"])
	assert_gt(reflected.get_pixel(32, 32).g, 0.5, "0.016 below the plane is kept")
	assert_gt(reflected.get_pixel(32, 33).g, 0.5, "0.047 below the plane is kept")
	assert_lt(reflected.get_pixel(32, 34).g, 0.05, "0.078 below the plane is clipped")
	assert_lt(reflected.get_pixel(32, 35).g, 0.05, "0.109 below the plane is clipped")
	assert_gt(beauty.get_pixel(32, 40).g, 0.5, "a camera drawing the water layer never clips")


func _render_terrain_card(cull_mask: int) -> Image:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.0
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.cull_mask = cull_mask
	camera.current = true
	viewport.add_child(camera)
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/terrain.gdshader") as Shader
	material.set_shader_parameter("u_debug_mode", 1)
	var card := MeshInstance3D.new()
	var card_mesh := QuadMesh.new()
	card_mesh.size = Vector2(2.0, 2.0)
	card.mesh = card_mesh
	card.material_override = material
	viewport.add_child(card)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()
