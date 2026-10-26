extends GutTest

# The methods of tests/target_projection_xr_interface_test.gd that need a RenderingDevice,
# split out of it so the windowed run selects them (G-100).

const SIDE := 512
const FOV_H := 80.0
# The 2000 x 1200 fixture frame (the registered 16:10 comparison profile).
const FRAME_ASPECT := 2000.0 / 1200.0


func after_each() -> void:
	# Never leave a foreign interface primary for the next test file.
	var primary := XRServer.primary_interface
	if primary != null and not (primary is TargetProjectionXrInterface):
		XRServer.primary_interface = null


func _device_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus" and \
			RenderingServer.get_rendering_device() != null


func _make_target() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(SIDE, SIDE)
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
	var cam := Camera3D.new()
	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.fov = FOV_H
	cam.near = 0.2
	cam.far = 1000.0
	viewport.add_child(cam)
	cam.make_current()
	return {"viewport": viewport, "camera": cam}


# A white unshaded card facing the camera (which sits at the origin looking
# down -Z): view-space x from `left` rightwards and y from `top` downwards, far
# past the frustum on both, at `depth` units out.
func _card(viewport: SubViewport, left: float, top: float, depth: float) -> void:
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(200.0, 200.0)
	quad.mesh = mesh
	var white := StandardMaterial3D.new()
	white.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	white.albedo_color = Color.WHITE
	quad.material_override = white
	quad.position = Vector3(left + 100.0, top - 100.0, -depth)
	viewport.add_child(quad)


func _raster(viewport: SubViewport) -> Image:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


func _first_lit_column(image: Image, row: int) -> int:
	for x in image.get_width():
		if image.get_pixel(x, row).r > 0.5:
			return x
	return -1


func _first_lit_row(image: Image, column: int) -> int:
	for y in image.get_height():
		if image.get_pixel(column, y).r > 0.5:
			return y
	return -1


# The pixel whose centre first lies past an edge at `edge` pixels.
func _first_pixel_past(edge: float) -> float:
	return ceilf(edge - 0.5)


func test_a_served_square_target_rasterises_the_served_frustum() -> void:
	if not _device_available():
		pending("requires a windowed Forward+ RenderingDevice run")
		return
	var fixture := _make_target()
	var viewport: SubViewport = fixture["viewport"]
	var cam: Camera3D = fixture["camera"]
	var depth := 50.0
	var right_angle := deg_to_rad(20.0)
	var up_angle := deg_to_rad(12.0)
	_card(viewport, depth * tan(right_angle), depth * tan(up_angle), depth)
	# The frame's frustum: the horizontal fov across the width, the vertical
	# half-extent tan(fov_h/2) / aspect across the height.
	var projection := Projection.create_perspective(FOV_H, FRAME_ASPECT, cam.near, cam.far, true)
	assert_true(TargetProjectionXrInterface.serve(viewport, cam.get_camera_transform(), projection))
	assert_true(viewport.use_xr, "serving draws the target through the XR branch")
	assert_eq(viewport.size, Vector2i(SIDE, SIDE), "the served raster is the 512 square")
	var image: Image = await _raster(viewport)
	assert_eq(image.get_size(), Vector2i(SIDE, SIDE))
	var half := SIDE * 0.5
	var tan_h := tan(deg_to_rad(FOV_H * 0.5))
	# Retail's column for a view angle a: 256 + 256 tan(a) / tan(fov_h/2); its
	# row for b: 256 - 256 tan(b) / (tan(fov_h/2) / aspect).
	var edge_x := half + half * tan(right_angle) / tan_h
	var edge_y := half - half * tan(up_angle) * FRAME_ASPECT / tan_h
	var column := int(edge_x) + 4
	var row := int(edge_y) + 4
	assert_almost_eq(float(_first_lit_column(image, row)), _first_pixel_past(edge_x), 1.0,
			"the vertical edge lands on the frame frustum's column")
	assert_almost_eq(float(_first_lit_row(image, column)), _first_pixel_past(edge_y), 1.0,
			"the horizontal edge lands on the frame frustum's row (non-square texels)")

	# Released, the target draws through its camera's own square frustum: the
	# same column, but the row of tan(fov_h/2) across the height.
	TargetProjectionXrInterface.release(viewport)
	assert_false(viewport.use_xr, "released, the target leaves the XR branch")
	var square: Image = await _raster(viewport)
	var square_y := half - half * tan(up_angle) / tan_h
	assert_gt(absf(square_y - edge_y), 20.0, "the two frusta put the edge rows apart")
	assert_almost_eq(float(_first_lit_row(square, column)), _first_pixel_past(square_y), 1.0,
			"the camera's square frustum draws again once released")
