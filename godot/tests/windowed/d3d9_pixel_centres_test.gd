extends GutTest

# Where the original's pixels sit (engine renderer/d3d9_raster.h): it rasterises
# through Direct3D 9, pixel i's centre on the integer window coordinate, so its
# image of the world and its HUD land half a pixel right and down of a raster whose
# centres sit at i + 0.5. The 3D pass builds a plain projection
# (Render_SetViewAndProjectionMatrices @0x58D9DE) over an integer viewport
# (Render_SetViewport @0x58A720); the HUD draws pre-transformed vertices at its
# coordinates as given (GDynamicVB_DrawPrimitive @0x6788E0). Both pins render
# through the real device: a tests/windowed/ script, run by
# `scripts/test_godot.sh --suite core --windowed` (a headless run pends it).

const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"
const TICK_COUNT := 2

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


# A BGRA TGA checkerboard: texel (x, y) is `on` where x + y is even, black otherwise.
func _checker_tga(size: Vector2i, on: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + size.x * size.y * 4)
	bytes[2] = 2
	bytes.encode_u16(12, size.x)
	bytes.encode_u16(14, size.y)
	bytes[16] = 32
	bytes[17] = 0x28
	for y in size.y:
		for x in size.x:
			var texel := on if (x + y) % 2 == 0 else Color(0, 0, 0, 1)
			var at := 18 + (y * size.x + x) * 4
			bytes[at] = texel.b8
			bytes[at + 1] = texel.g8
			bytes[at + 2] = texel.r8
			bytes[at + 3] = texel.a8
	return bytes


func _surface(size: Vector2i) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.own_world_3d = true
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	return viewport


func _render(viewport: SubViewport) -> Image:
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


# The HUD's draw lists are the original's window coordinates: a quad at integer
# coordinates covers the same pixels on either raster, but D3D9 samples its
# texture at the texels' shared corners (pixel i's centre is the quad's x0 + k),
# so a texel-for-texel checkerboard filters to one even grey; drawn at Godot's
# centres it would sample every texel's own centre and keep the checker.
func test_the_hud_samples_its_textures_where_d3d9_does() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var dir := TestFs.cache_dir(self, "d3d9_pixel_centres")
	_temp_dirs.append(dir)
	TestFs.write_bytes(self, dir.path_join("frame.tga"),
			_checker_tga(Vector2i(32, 16), Color(0.25, 0.25, 0.25, 1)))
	TestFs.write_text(self, dir.path_join("hudpos.def"),
			"\n".join(PackedStringArray(HudFixture.DECLUTTER_ROWS)
					+ PackedStringArray(["STATICFRAME frame.tga 100 100"])))
	var layout := HudPos.new()
	assert_eq(layout.load(dir.path_join("hudpos.def")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var viewport := _surface(Vector2i(1024, 768))
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	viewport.add_child(hud)
	hud.configure(layout, root)
	hud.set_player_state(100, 1.0, 0, 80.0)
	var image: Image = await _render(viewport)
	# The frame's footprint (authored at 100, 100 of the 1024 x 768 design, drawn
	# 1:1 on this surface): every pixel the quad lit around it.
	var background := image.get_pixel(70, 70).r
	var low := Vector2i(1 << 20, 1 << 20)
	var high := Vector2i(-1, -1)
	for y in range(80, 140):
		for x in range(80, 160):
			if absf(image.get_pixel(x, y).r - background) > 0.02:
				low = low.min(Vector2i(x, y))
				high = high.max(Vector2i(x, y))
	var lit := Rect2i(low, high - low + Vector2i.ONE)
	assert_eq(lit.size, Vector2i(32, 16),
			"the quad at integer coordinates covers its 32 x 16 pixels, as on either raster")
	if lit.size.x < 8 or lit.size.y < 8:
		return
	var first := image.get_pixel(lit.position.x + 3, lit.position.y + 3).r
	assert_gt(first, 0.1, "the checker's mean is drawn")
	for y in range(lit.position.y + 2, lit.end.y - 2):
		for x in range(lit.position.x + 2, lit.end.x - 2):
			var value := image.get_pixel(x, y).r
			if absf(value - first) > 0.02:
				fail_test("pixel (%d, %d) = %.3f against %.3f: the checker survived, the HUD sampled texel centres" % [x, y, value, first])
				return
	pass_test("every interior pixel filters four texels evenly, as D3D9's pixel centres do")


# The frame's camera draws through the surface's D3D9 half pixel: an edge the
# original's projection puts a quarter pixel past a pixel centre lights the
# next pixel on, on both axes (D3D9: centre >= edge), where a plain projection
# on this raster would light the pixel the edge falls in.
func test_the_frame_camera_rasterises_on_d3d9_pixel_centres() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var world := WorldFixture.boot_minimal(self)
	var surface := _surface(Vector2i(64, 48))
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	environment.environment.background_color = Color(0, 0, 0, 1)
	surface.add_child(environment)
	var camera := Camera3D.new()
	surface.add_child(camera)
	camera.make_current()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, ControlsModel.new())
	world.get_sim().set_local_player_aspect_mode(-1)
	var dt := Simulation.tick_dt()
	for i in TICK_COUNT:
		var frame_input := presenter.before_world_tick(dt, false, true)
		world.tick(camera.global_position, camera.global_transform, dt, frame_input)
		presenter.after_world_tick()
	assert_eq(camera.projection, Camera3D.PROJECTION_FRUSTUM,
			"the frame camera draws through its offset frustum")
	assert_null(presenter.projection_viewport(), "the native mode draws the surface directly")
	# A white card in front of the eye whose top-left corner the original's
	# projection (screen_projection: the raster shift taken out) puts at window
	# (32.25, 24.25) on the 64 x 48 surface.
	var screen := presenter.screen_projection()
	var depth := 5.0
	var ndc := Vector2(32.25 / 64.0 * 2.0 - 1.0, 1.0 - 24.25 / 48.0 * 2.0)
	var corner := Vector3(ndc.x * depth / screen.x.x, ndc.y * depth / screen.y.y, -depth)
	var mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([corner, corner + Vector3(4, 0, 0),
			corner + Vector3(4, -4, 0), corner, corner + Vector3(4, -4, 0),
			corner + Vector3(0, -4, 0)])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.disable_fog = true
	material.albedo_color = Color(1, 1, 1, 1)
	var card := MeshInstance3D.new()
	card.mesh = mesh
	card.material_override = material
	card.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	surface.add_child(card)
	card.global_transform = camera.get_camera_transform()
	camera.cull_mask = card.layers
	var image: Image = await _render(surface)
	assert_lt(image.get_pixel(32, 30).r, 0.5, "column 32's centre (32) lies left of the edge at 32.25")
	assert_gt(image.get_pixel(33, 30).r, 0.5, "column 33's centre (33) lies past it")
	assert_lt(image.get_pixel(40, 24).r, 0.5, "row 24's centre (24) lies above the edge at 24.25")
	assert_gt(image.get_pixel(40, 25).r, 0.5, "row 25's centre (25) lies below it")
	presenter.teardown()
	assert_eq(camera.projection, Camera3D.PROJECTION_PERSPECTIVE,
			"teardown hands the camera its own projection back")
