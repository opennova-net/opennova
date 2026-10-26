extends GutTest

# The methods of tests/water_test.gd that need a RenderingDevice, split out of it so the
# windowed run selects them (G-100).

# The engine tick [docs/engine-primer.md: 62 Hz].
const TICK := 1.0 / 62.0


func test_noise_pattern_is_fixed_in_the_world_at_one_texture_per_32_units() -> void:
	# The strip samples its noise pair at world-fixed texcoords (water_test.gd): a
	# texture whose u half [0, 0.5) is black and [0.5, 1) white paints bands
	# 16 units wide along Godot z, repeating every 32 units.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
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
	var water := Water.new()
	viewport.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.global_position = Vector3(3.0, 27.0, 16.0)
	cam.rotation_degrees = Vector3(-60.0, 0.0, 0.0)
	cam.make_current()
	water.advance_frame(TICK)
	var mesh := water.get_mesh_instance().mesh as ArrayMesh
	assert_gt(mesh.get_surface_count(), 0, "the fixture view must march strip rows")

	var bands := Image.create(64, 4, false, Image.FORMAT_RGBA8)
	for x in 64:
		for y in 4:
			bands.set_pixel(x, y, Color.BLACK if x < 32 else Color.WHITE)
	var material := water.get_water_material()
	material.set_shader_parameter("u_noise_color", ImageTexture.create_from_image(bands))
	var neutral := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	neutral.fill(Color(0.5, 0.5, 1.0, 1.0))
	material.set_shader_parameter("u_noise_normal", ImageTexture.create_from_image(neutral))
	material.set_shader_parameter("u_has_reflection", false)
	material.set_shader_parameter("u_water_color", Vector3.ONE)
	material.set_shader_parameter("u_fog_color", Vector3.ZERO)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var image := viewport.get_texture().get_image()
	# z = 8 -> u = 0.25 (black band); z = -8 -> u = -0.25, wrapping to 0.75
	# (white band).
	var black_px := cam.unproject_position(Vector3(3.0, 7.0, 8.0))
	var white_px := cam.unproject_position(Vector3(3.0, 7.0, -8.0))
	var black := image.get_pixelv(Vector2i(black_px))
	var white := image.get_pixelv(Vector2i(white_px))
	assert_gt(white.r, black.r + 0.2,
			"the band at world z = -8 samples u = 0.75 and the one at z = 8 u = 0.25 "
			+ "(black %s, white %s)" % [black, white])
	water.release_runtime_renderer_resources()


func _depth_fixture(camera_position: Vector3, pitch_deg: float) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
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
	var water := Water.new()
	viewport.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.global_position = camera_position
	cam.rotation_degrees = Vector3(pitch_deg, 0.0, 0.0)
	# The retail scene far is the fog distance's integer word + 1 over the
	# envless water's 1000-unit fog (Render_ProcessMainSceneFrame
	# @ 0x5CA4BA..0x5CA4D0), near 0.2.
	cam.near = 0.2
	cam.far = 1001.0
	cam.make_current()
	return {"viewport": viewport, "water": water, "camera": cam}


func _render_water_frame(fixture: Dictionary, fog_color: Vector3,
		water_color := Vector3(0.408, 0.314, 0.224)) -> Image:
	var water: Water = fixture["water"]
	water.advance_frame(TICK)
	var material := water.get_water_material()
	material.set_shader_parameter("u_has_reflection", false)
	material.set_shader_parameter("u_fog_color", fog_color)
	material.set_shader_parameter("u_water_color", water_color)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		return null
	return (fixture["viewport"] as SubViewport).get_texture().get_image()


func test_water_beyond_the_far_plane_reaches_the_horizon() -> void:
	# The pre-transformed strip is never far-clipped: rows past the scene far
	# plane clamp to the viewport MaxZ (retail Render_WaterStripDetailed
	# @ 0x5c2c1f..0x5c2c4a) and draw fully fogged water up to the horizon.
	var fixture := _depth_fixture(Vector3(0.0, 107.0, 0.0), -5.0)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var cam: Camera3D = fixture["camera"]
	# The strip's first row is the plane point 2000 units ahead
	# (Terrain_ProjectSectorToScreen @ 0x5c0c7c..0x5c0d25); 1500 units out
	# lies between the 1001 far plane and that row.
	var far_px := cam.unproject_position(Vector3(0.0, 7.0, -1500.0))
	var far_pixel := image.get_pixelv(Vector2i(far_px))
	assert_gt(far_pixel.g, 0.5,
			"water 1500 units out (past the 1001 far plane) draws fogged: %s" % far_pixel)
	water.release_runtime_renderer_resources()


func test_water_loses_a_depth_tie_to_geometry_on_its_plane() -> void:
	# The strip depth replicates the scene curve with far = w while the scene
	# draws at far = w + 1, so the water sits just behind its true depth and a
	# surface exactly on the plane wins the LESSEQUAL test (retail depth chain
	# @ 0x5c2c0c..0x5c2c4a against Render_ProcessMainSceneFrame
	# @ 0x5CA4BA..0x5CA4D0).
	var fixture := _depth_fixture(Vector3(0.0, 27.0, 20.0), -45.0)
	var quad := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40.0, 40.0)
	quad.mesh = plane
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color.RED
	quad.material_override = red
	quad.position = Vector3(0.0, 7.0, 0.0)
	(fixture["viewport"] as SubViewport).add_child(quad)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var cam: Camera3D = fixture["camera"]
	var pixel := image.get_pixelv(Vector2i(cam.unproject_position(Vector3(0.0, 7.0, 0.0))))
	assert_gt(pixel.r, 0.9, "the coplanar surface keeps its pixel: %s" % pixel)
	assert_lt(pixel.g, 0.1, "the water behind it is depth-rejected: %s" % pixel)
	water.release_runtime_renderer_resources()


func test_water_depth_follows_the_retail_scene_curve_under_any_camera_near() -> void:
	# The replica depth is tested against retail's scene curve (near 0.2,
	# far = w + 1), whatever near plane the Godot camera carries: a wall
	# standing in the water keeps the pixels just above the waterline, where
	# the water behind it lies only a little farther than the wall.
	var fixture := _depth_fixture(Vector3(0.0, 9.0, 0.0), -10.0)
	var cam: Camera3D = fixture["camera"]
	cam.near = 0.05
	var wall := MeshInstance3D.new()
	var wall_mesh := QuadMesh.new()
	wall_mesh.size = Vector2(40.0, 10.0)
	wall.mesh = wall_mesh
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color.RED
	wall.material_override = red
	wall.position = Vector3(0.0, 7.0, -20.0)
	(fixture["viewport"] as SubViewport).add_child(wall)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	# 0.5 above the water, 20 out: the ray behind it meets the water ~27 out.
	var pixel := image.get_pixelv(Vector2i(cam.unproject_position(Vector3(0.0, 7.5, -20.0))))
	assert_gt(pixel.r, 0.9, "the wall keeps its pixel above the waterline: %s" % pixel)
	assert_lt(pixel.g, 0.1, "the water behind the wall is depth-rejected: %s" % pixel)
	water.release_runtime_renderer_resources()


func test_wake_rings_face_up_and_hide_from_below() -> void:
	# The wake pass flags (0x100000, retail WaterRing_Draw
	# @ 0x5DE245) carry no cull-none bit, so the rings keep the device's
	# back-face cull. A card wound like the compiled ring (its right-hand
	# normal down, renderer/water_wake_frame.cpp) draws for an eye above it
	# and not for one below.
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/water_wake.gdshader") as Shader
	var white := ImageTexture.create_from_image(
			Image.create_from_data(1, 1, false, Image.FORMAT_RGBA8,
					PackedByteArray([255, 255, 255, 255])))
	material.set_shader_parameter("wake_texture", white)
	material.set_shader_parameter("gradient_texture", white)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-1.0, 0.0, -1.0), Vector3(1.0, 0.0, -1.0),
		Vector3(1.0, 0.0, 1.0), Vector3(-1.0, 0.0, 1.0)])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
	arrays[Mesh.ARRAY_TEX_UV2] = arrays[Mesh.ARRAY_TEX_UV]
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var above: Image = await _render_wake_card(mesh, material, 5.0)
	var below: Image = await _render_wake_card(mesh, material, -5.0)
	if above == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(above.get_pixel(32, 32).r, 0.7, "an eye above sees the ring (0.4 x 2)")
	assert_lt(below.get_pixel(32, 32).r, 0.02, "an eye below the water sees none")


func _render_wake_card(mesh: ArrayMesh, material: ShaderMaterial, eye_y: float) -> Image:
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
	var card := MeshInstance3D.new()
	card.mesh = mesh
	card.material_override = material
	viewport.add_child(card)
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.position = Vector3(0.0, eye_y, 0.0)
	cam.rotation_degrees = Vector3(-90.0 if eye_y > 0.0 else 90.0, 0.0, 0.0)
	cam.make_current()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		return null
	return viewport.get_texture().get_image()
